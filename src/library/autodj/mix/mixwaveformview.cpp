#include "library/autodj/mix/mixwaveformview.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <QtConcurrent>
#include <cmath>
#include <limits>

#include "analyzer/constants.h"
#include "moc_mixwaveformview.cpp"
#include "sources/audiosourcestereoproxy.h"
#include "sources/soundsourceproxy.h"
#include "track/beats.h"
#include "track/track.h"
#include "util/samplebuffer.h"

namespace {

constexpr double kBinsPerSecond = 50.0;
constexpr double kMinSpan = 4.0;
constexpr double kMaxSpan = 300.0;
constexpr int kRulerHeight = 22;
constexpr int kLaneGap = 6;
constexpr int kHandleGrab = 7;
constexpr int kMaxBeatTicks = 1500;

const QColor kBackground(0x16, 0x16, 0x18);
const QColor kLaneBackground(0x22, 0x22, 0x26);
const QColor kColorA(0xff, 0x7a, 0x1a);
const QColor kColorB(0x2b, 0xb6, 0xe0);
const QColor kTextColor(0xc8, 0xc8, 0xcc);

/// Decodes the whole track and returns a peak envelope with
/// kBinsPerSecond values per second. Runs on a worker thread.
QVector<float> computeEnvelope(TrackPointer pTrack) {
    QVector<float> envelope;
    if (!pTrack) {
        return envelope;
    }
    mixxx::AudioSource::OpenParams openParams;
    openParams.setChannelCount(mixxx::kAnalysisChannels);
    const mixxx::AudioSourcePointer pSource =
            SoundSourceProxy(pTrack).openAudioSource(openParams);
    if (!pSource) {
        return envelope;
    }
    mixxx::AudioSourceStereoProxy proxy(pSource, mixxx::kAnalysisFramesPerChunk);
    const double sampleRate = proxy.getSignalInfo().getSampleRate();
    if (sampleRate <= 0) {
        return envelope;
    }
    const SINT framesPerBin = qMax<SINT>(1, static_cast<SINT>(sampleRate / kBinsPerSecond));
    envelope.reserve(static_cast<int>(proxy.frameIndexRange().length() / framesPerBin + 1));

    mixxx::SampleBuffer buffer(mixxx::kAnalysisSamplesPerChunk);
    float peak = 0.0f;
    SINT framesInBin = 0;
    auto pushFrame = [&](float value) {
        peak = std::max(peak, value);
        if (++framesInBin >= framesPerBin) {
            envelope.push_back(peak);
            peak = 0.0f;
            framesInBin = 0;
        }
    };

    mixxx::IndexRange remaining = proxy.frameIndexRange();
    while (!remaining.empty()) {
        const auto chunk = remaining.splitAndShrinkFront(
                std::min(mixxx::kAnalysisFramesPerChunk, remaining.length()));
        const auto readable = proxy.readSampleFrames(mixxx::WritableSampleFrames(
                chunk, mixxx::SampleBuffer::WritableSlice(buffer)));
        const CSAMPLE* pData = readable.readableData();
        const SINT samples = readable.readableLength();
        for (SINT i = 0; i + 1 < samples; i += 2) {
            pushFrame((std::fabs(pData[i]) + std::fabs(pData[i + 1])) * 0.5f);
        }
        // Keep the timeline aligned if the decoder skipped frames.
        for (SINT i = samples / 2; i < chunk.length(); ++i) {
            pushFrame(0.0f);
        }
    }
    // Normalize so quiet masters still show a readable shape.
    float maxValue = 0.0f;
    for (float v : std::as_const(envelope)) {
        maxValue = std::max(maxValue, v);
    }
    if (maxValue > 0.0f) {
        for (float& v : envelope) {
            v /= maxValue;
        }
    }
    return envelope;
}

QString formatTime(double seconds) {
    const bool negative = seconds < 0;
    seconds = std::fabs(seconds);
    const int minutes = static_cast<int>(seconds / 60);
    const double rest = seconds - minutes * 60;
    return QStringLiteral("%1%2:%3")
            .arg(negative ? QStringLiteral("-") : QString())
            .arg(minutes)
            .arg(rest, 4, 'f', 1, QChar('0'));
}

} // anonymous namespace

MixWaveformView::MixWaveformView(QWidget* pParent)
        : QWidget(pParent),
          m_viewStart(-16.0),
          m_viewSpan(40.0),
          m_dragStartViewStart(0.0),
          m_snap(true),
          m_playhead(std::numeric_limits<double>::quiet_NaN()),
          m_drag(Drag::None) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

MixWaveformView::~MixWaveformView() {
    for (Lane& lane : m_lanes) {
        if (lane.pWatcher) {
            lane.pWatcher->disconnect(this);
        }
    }
}

QSize MixWaveformView::sizeHint() const {
    return QSize(900, 260);
}

QSize MixWaveformView::minimumSizeHint() const {
    return QSize(400, 160);
}

void MixWaveformView::setTracks(const TrackPointer& pFrom, const TrackPointer& pTo) {
    m_lanes[0].pTrack = pFrom;
    m_lanes[1].pTrack = pTo;
    loadEnvelope(0);
    loadEnvelope(1);
    update();
}

void MixWaveformView::loadEnvelope(int laneIndex) {
    Lane& lane = m_lanes[laneIndex];
    lane.envelope.clear();
    if (!lane.pTrack) {
        return;
    }
    lane.loading = true;
    if (lane.pWatcher) {
        lane.pWatcher->disconnect(this);
        lane.pWatcher->deleteLater();
    }
    lane.pWatcher = new QFutureWatcher<QVector<float>>(this);
    connect(lane.pWatcher,
            &QFutureWatcher<QVector<float>>::finished,
            this,
            [this, laneIndex]() {
                Lane& lane = m_lanes[laneIndex];
                lane.envelope = lane.pWatcher->result();
                lane.loading = false;
                lane.pWatcher->deleteLater();
                lane.pWatcher = nullptr;
                update();
            });
    lane.pWatcher->setFuture(QtConcurrent::run(computeEnvelope, lane.pTrack));
}

void MixWaveformView::setTransition(const MixTransition& transition) {
    m_transition = transition;
    update();
}

void MixWaveformView::setSnapToBeats(bool snap) {
    m_snap = snap;
}

void MixWaveformView::setPlayhead(double mixTime) {
    m_playhead = mixTime;
    update();
}

void MixWaveformView::frameTransition() {
    const double padding = qMax(8.0, m_transition.length * 0.6);
    m_viewSpan = qBound(kMinSpan, m_transition.length + 2 * padding, kMaxSpan);
    m_viewStart = -(m_viewSpan - m_transition.length) / 2;
    update();
}

double MixWaveformView::beatLength() const {
    const TrackPointer& pTrack = m_lanes[0].pTrack;
    if (!pTrack || pTrack->getBpm() <= 0) {
        return 0.0;
    }
    return 60.0 / pTrack->getBpm();
}

double MixWaveformView::snapToBeat(int lane, double seconds) const {
    const TrackPointer& pTrack = m_lanes[lane].pTrack;
    if (!pTrack) {
        return seconds;
    }
    const mixxx::BeatsPointer pBeats = pTrack->getBeats();
    const double sampleRate = pTrack->getSampleRate();
    if (!pBeats || sampleRate <= 0) {
        return seconds;
    }
    const auto closest = pBeats->findClosestBeat(mixxx::audio::FramePos(seconds * sampleRate));
    if (!closest.isValid()) {
        return seconds;
    }
    return closest.value() / sampleRate;
}

void MixWaveformView::applySnap() {
    if (!m_snap) {
        return;
    }
    if (m_drag == Drag::LaneA) {
        m_transition.fromTrigger = qMax(0.0, snapToBeat(0, m_transition.fromTrigger));
    } else if (m_drag == Drag::LaneB) {
        m_transition.toStart = qMax(0.0, snapToBeat(1, m_transition.toStart));
    } else if (m_drag == Drag::Length) {
        const double beat = beatLength();
        if (beat > 0) {
            m_transition.length = qMax(0.0, std::round(m_transition.length / beat) * beat);
        }
    }
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

QRectF MixWaveformView::rulerRect() const {
    return QRectF(0, 0, width(), kRulerHeight);
}

QRectF MixWaveformView::laneRect(int lane) const {
    const double top = kRulerHeight + kLaneGap;
    const double laneHeight = (height() - top - kLaneGap) / 2.0;
    return QRectF(0, top + lane * (laneHeight + kLaneGap), width(), laneHeight);
}

double MixWaveformView::xForMixTime(double mixTime) const {
    return (mixTime - m_viewStart) / m_viewSpan * width();
}

double MixWaveformView::mixTimeForX(double x) const {
    return m_viewStart + x / qMax(1, width()) * m_viewSpan;
}

double MixWaveformView::laneTime(int lane, double mixTime) const {
    return (lane == 0 ? m_transition.fromTrigger : m_transition.toStart) + mixTime;
}

double MixWaveformView::envelopeAt(int lane, double trackSeconds) const {
    const QVector<float>& envelope = m_lanes[lane].envelope;
    const int index = static_cast<int>(trackSeconds * kBinsPerSecond);
    if (index < 0 || index >= envelope.size()) {
        return -1.0; // outside the track
    }
    return envelope.at(index);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void MixWaveformView::paintEvent(QPaintEvent* /*pEvent*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.fillRect(rect(), kBackground);

    paintRuler(&painter);
    for (int lane = 0; lane < 2; ++lane) {
        painter.fillRect(laneRect(lane), kLaneBackground);
        paintLane(&painter, lane);
        paintBeats(&painter, lane);
    }
    paintOverlap(&painter);

    if (!std::isnan(m_playhead)) {
        const double x = xForMixTime(m_playhead);
        painter.setPen(QPen(Qt::white, 2));
        painter.drawLine(QPointF(x, kRulerHeight), QPointF(x, height()));
    }

    if (hasFocus()) {
        painter.setPen(QPen(QColor(255, 255, 255, 40), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
    }
}

void MixWaveformView::paintRuler(QPainter* pPainter) {
    const QRectF ruler = rulerRect();
    pPainter->fillRect(ruler, QColor(0x1c, 0x1c, 0x20));
    // Choose a tick spacing that leaves ~70px between labels.
    const double secondsPerPixel = m_viewSpan / qMax(1, width());
    double step = 1.0;
    for (double candidate : {1.0, 2.0, 4.0, 5.0, 10.0, 15.0, 30.0, 60.0}) {
        step = candidate;
        if (candidate / secondsPerPixel >= 70) {
            break;
        }
    }
    pPainter->setPen(kTextColor);
    QFont font = pPainter->font();
    font.setPointSizeF(font.pointSizeF() * 0.85);
    pPainter->setFont(font);
    const double first = std::ceil(m_viewStart / step) * step;
    for (double t = first; t <= m_viewStart + m_viewSpan; t += step) {
        const double x = xForMixTime(t);
        pPainter->drawLine(QPointF(x, ruler.bottom() - 5), QPointF(x, ruler.bottom()));
        const QString label = t == 0 ? QStringLiteral("0")
                                     : QStringLiteral("%1%2s").arg(t > 0 ? "+" : "").arg(t);
        pPainter->drawText(QRectF(x + 3, 0, 60, ruler.height() - 4),
                Qt::AlignLeft | Qt::AlignVCenter,
                label);
    }
}

void MixWaveformView::paintLane(QPainter* pPainter, int lane) {
    const QRectF r = laneRect(lane);
    const QColor color = lane == 0 ? kColorA : kColorB;
    const double centerY = r.center().y();
    const double halfHeight = r.height() / 2.0 - 3;
    const double mixEnd = m_transition.style == MixTransition::Style::Cut
            ? 0.0
            : m_transition.length;

    if (m_lanes[lane].envelope.isEmpty()) {
        pPainter->setPen(kTextColor);
        pPainter->drawText(r,
                Qt::AlignCenter,
                m_lanes[lane].loading ? tr("Loading waveform...") : tr("No audio"));
    } else {
        const int w = width();
        for (int x = 0; x < w; ++x) {
            const double mixTime = mixTimeForX(x + 0.5);
            const double value = envelopeAt(lane, laneTime(lane, mixTime));
            if (value < 0) {
                continue;
            }
            // A is heard until the mix ends, B from the mix start on.
            const bool audible = lane == 0 ? mixTime <= mixEnd : mixTime >= 0;
            QColor c = color;
            if (!audible) {
                c.setAlpha(70);
            }
            pPainter->setPen(c);
            const double h = qMax(1.0, value * halfHeight);
            pPainter->drawLine(QPointF(x, centerY - h), QPointF(x, centerY + h));
        }
    }

    // Lane label with the track name and time at the mix point.
    const TrackPointer& pTrack = m_lanes[lane].pTrack;
    if (pTrack) {
        const QString label = QStringLiteral("%1  %2  @ %3")
                                      .arg(lane == 0 ? QStringLiteral("A") : QStringLiteral("B"),
                                              pTrack->getTitle().isEmpty()
                                                      ? pTrack->getInfo()
                                                      : pTrack->getTitle(),
                                              formatTime(lane == 0
                                                              ? m_transition.fromTrigger
                                                              : m_transition.toStart));
        pPainter->setPen(Qt::white);
        pPainter->drawText(r.adjusted(6, 3, -6, -3), Qt::AlignLeft | Qt::AlignTop, label);
    }
}

void MixWaveformView::paintBeats(QPainter* pPainter, int lane) {
    const TrackPointer& pTrack = m_lanes[lane].pTrack;
    if (!pTrack) {
        return;
    }
    const mixxx::BeatsPointer pBeats = pTrack->getBeats();
    const double sampleRate = pTrack->getSampleRate();
    if (!pBeats || sampleRate <= 0) {
        return;
    }
    const QRectF r = laneRect(lane);
    const double startSeconds = qMax(0.0, laneTime(lane, m_viewStart));
    const double endSeconds = laneTime(lane, m_viewStart + m_viewSpan);
    // Skip ticks when they'd be closer than 4px.
    if (beatLength() > 0 && beatLength() / m_viewSpan * width() < 4) {
        return;
    }
    pPainter->setPen(QColor(255, 255, 255, 55));
    auto pos = pBeats->findNextBeat(mixxx::audio::FramePos(startSeconds * sampleRate));
    for (int i = 0; i < kMaxBeatTicks && pos.isValid(); ++i) {
        const double seconds = pos.value() / sampleRate;
        if (seconds > endSeconds) {
            break;
        }
        const double mixTime = seconds - laneTime(lane, 0.0);
        const double x = xForMixTime(mixTime);
        pPainter->drawLine(QPointF(x, r.top()), QPointF(x, r.top() + 8));
        pPainter->drawLine(QPointF(x, r.bottom() - 8), QPointF(x, r.bottom()));
        const auto next = pBeats->findNextBeat(pos + 1);
        if (!next.isValid() || next <= pos) {
            break;
        }
        pos = next;
    }
}

void MixWaveformView::paintOverlap(QPainter* pPainter) {
    const QRectF laneA = laneRect(0);
    const QRectF laneB = laneRect(1);
    const double length = m_transition.style == MixTransition::Style::Cut
            ? 0.0
            : m_transition.length;
    const double x0 = xForMixTime(0.0);
    const double x1 = xForMixTime(length);
    const QRectF area(QPointF(x0, laneA.top()), QPointF(x1, laneB.bottom()));
    pPainter->fillRect(area, QColor(255, 255, 255, 22));

    // Volume curves: A's level over lane A, B's level over lane B.
    const TrackPointer& pFrom = m_lanes[0].pTrack;
    const TrackPointer& pTo = m_lanes[1].pTrack;
    const MixTransition::Style style = m_transition.effectiveStyle(
            pFrom ? pFrom->getBpm() : 0.0, pTo ? pTo->getBpm() : 0.0);
    if (x1 - x0 > 2) {
        pPainter->setRenderHint(QPainter::Antialiasing, true);
        QPainterPath pathA;
        QPainterPath pathB;
        const int steps = 60;
        for (int i = 0; i <= steps; ++i) {
            const double p = static_cast<double>(i) / steps;
            const double t = MixTransition::crossfadePosition(style, m_transition.curve, p);
            const double x = x0 + (x1 - x0) * p;
            const QPointF pointA(x, laneA.bottom() - (1.0 - t) * (laneA.height() - 4) - 2);
            const QPointF pointB(x, laneB.bottom() - t * (laneB.height() - 4) - 2);
            if (i == 0) {
                pathA.moveTo(pointA);
                pathB.moveTo(pointB);
            } else {
                pathA.lineTo(pointA);
                pathB.lineTo(pointB);
            }
        }
        pPainter->setBrush(Qt::NoBrush);
        pPainter->setPen(QPen(Qt::white, 2));
        pPainter->drawPath(pathA);
        pPainter->drawPath(pathB);
        pPainter->setRenderHint(QPainter::Antialiasing, false);
    }

    // Start and end markers.
    pPainter->setPen(QPen(QColor(255, 255, 255, 200), 1, Qt::DashLine));
    pPainter->drawLine(QPointF(x0, kRulerHeight), QPointF(x0, height()));
    pPainter->setPen(QPen(QColor(255, 255, 255, 200), 1));
    pPainter->drawLine(QPointF(x1, kRulerHeight), QPointF(x1, height()));
    // Grab handle for the length.
    const QRectF handle(x1 - 5, laneA.bottom() - 4, 10, kLaneGap + 8);
    pPainter->setBrush(Qt::white);
    pPainter->drawRoundedRect(handle, 2, 2);
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

void MixWaveformView::mousePressEvent(QMouseEvent* pEvent) {
    if (pEvent->button() != Qt::LeftButton) {
        return;
    }
    const QPointF pos = pEvent->position();
    m_dragStartPos = pos;
    m_dragStartTransition = m_transition;
    m_dragStartViewStart = m_viewStart;
    const double endX = xForMixTime(
            m_transition.style == MixTransition::Style::Cut ? 0.0 : m_transition.length);
    if (std::fabs(pos.x() - endX) <= kHandleGrab && pos.y() > kRulerHeight &&
            m_transition.style != MixTransition::Style::Cut) {
        m_drag = Drag::Length;
    } else if (laneRect(0).contains(pos)) {
        m_drag = Drag::LaneA;
    } else if (laneRect(1).contains(pos)) {
        m_drag = Drag::LaneB;
    } else {
        m_drag = Drag::Pan;
    }
    setCursor(m_drag == Drag::Length ? Qt::SizeHorCursor : Qt::ClosedHandCursor);
}

void MixWaveformView::mouseMoveEvent(QMouseEvent* pEvent) {
    const QPointF pos = pEvent->position();
    if (m_drag == Drag::None) {
        const double endX = xForMixTime(m_transition.length);
        const bool overHandle = std::fabs(pos.x() - endX) <= kHandleGrab &&
                pos.y() > kRulerHeight &&
                m_transition.style != MixTransition::Style::Cut;
        setCursor(overHandle ? Qt::SizeHorCursor : Qt::OpenHandCursor);
        return;
    }
    const double dx = (pos.x() - m_dragStartPos.x()) / qMax(1, width()) * m_viewSpan;
    const double fromDuration = m_lanes[0].pTrack ? m_lanes[0].pTrack->getDuration() : 1e9;
    const double toDuration = m_lanes[1].pTrack ? m_lanes[1].pTrack->getDuration() : 1e9;
    switch (m_drag) {
    case Drag::LaneA:
        // Dragging the waveform right means mixing out earlier in the track.
        m_transition.fromTrigger = qBound(0.0,
                m_dragStartTransition.fromTrigger - dx,
                qMax(0.0, fromDuration - 1.0));
        break;
    case Drag::LaneB:
        m_transition.toStart = qBound(0.0,
                m_dragStartTransition.toStart - dx,
                qMax(0.0, toDuration - 1.0));
        break;
    case Drag::Length:
        m_transition.length = qBound(0.0, mixTimeForX(pos.x()), 120.0);
        break;
    case Drag::Pan:
        m_viewStart = m_dragStartViewStart - dx;
        update();
        return;
    case Drag::None:
        break;
    }
    applySnap();
    update();
    emit transitionEdited();
}

void MixWaveformView::mouseReleaseEvent(QMouseEvent* /*pEvent*/) {
    m_drag = Drag::None;
    setCursor(Qt::OpenHandCursor);
}

void MixWaveformView::wheelEvent(QWheelEvent* pEvent) {
    const double anchor = mixTimeForX(pEvent->position().x());
    const double factor = pEvent->angleDelta().y() > 0 ? 0.8 : 1.25;
    const double newSpan = qBound(kMinSpan, m_viewSpan * factor, kMaxSpan);
    // Keep the time under the mouse fixed while zooming.
    m_viewStart = anchor - (anchor - m_viewStart) * newSpan / m_viewSpan;
    m_viewSpan = newSpan;
    update();
    pEvent->accept();
}
