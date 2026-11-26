#include "library/autodj/mix/mixwaveformview.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <QtConcurrent>
#include <algorithm>
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
constexpr int kRulerHeight = 18;
constexpr int kLaneGap = 4;
constexpr int kHandleGrab = 7;
constexpr int kPointRadius = 4;
constexpr int kPointGrab = 8;
constexpr int kMaxBeatTicks = 1500;
constexpr double kNeutralSnap = 0.04;

const QColor kBackground(0x0e, 0x0f, 0x12);
const QColor kLaneBackground(0x15, 0x16, 0x1a);
const QColor kColorA(0xff, 0x8a, 0x3d);
const QColor kColorB(0x33, 0xc3, 0xf0);
const QColor kColorTempo(0xe8, 0xe9, 0xed);
const QColor kTextColor(0x9a, 0x9c, 0xa5);

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
    const SINT framesPerBin = std::max<SINT>(1, static_cast<SINT>(sampleRate / kBinsPerSecond));
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
        for (SINT i = samples / 2; i < chunk.length(); ++i) {
            pushFrame(0.0f);
        }
    }
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

bool isDeckBLane(MixLane lane) {
    switch (lane) {
    case MixLane::VolumeB:
    case MixLane::LowB:
    case MixLane::MidB:
    case MixLane::HighB:
    case MixLane::FilterB:
        return true;
    default:
        return false;
    }
}

bool hasNeutralMiddle(MixLane lane) {
    return lane != MixLane::VolumeA && lane != MixLane::VolumeB && lane != MixLane::Tempo;
}

} // anonymous namespace

MixWaveformView::MixWaveformView(QWidget* pParent)
        : QWidget(pParent),
          m_group(LaneGroup::Volume),
          m_viewStart(-16.0),
          m_viewSpan(40.0),
          m_dragStartViewStart(0.0),
          m_snap(true),
          m_playhead(std::numeric_limits<double>::quiet_NaN()),
          m_startBpm(0.0),
          m_targetBpm(0.0),
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
    return QSize(900, 150);
}

QSize MixWaveformView::minimumSizeHint() const {
    return QSize(300, 90);
}

void MixWaveformView::setTracks(const TrackPointer& pFrom, const TrackPointer& pTo) {
    const bool fromChanged = pFrom != m_lanes[0].pTrack;
    const bool toChanged = pTo != m_lanes[1].pTrack;
    m_lanes[0].pTrack = pFrom;
    m_lanes[1].pTrack = pTo;
    if (fromChanged) {
        loadEnvelope(0);
    }
    if (toChanged) {
        loadEnvelope(1);
    }
    update();
}

void MixWaveformView::loadEnvelope(int laneIndex) {
    Lane& lane = m_lanes[laneIndex];
    lane.envelope.clear();
    if (lane.pWatcher) {
        lane.pWatcher->disconnect(this);
        lane.pWatcher->deleteLater();
        lane.pWatcher = nullptr;
    }
    if (!lane.pTrack) {
        lane.loading = false;
        return;
    }
    lane.loading = true;
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

void MixWaveformView::setLaneGroup(LaneGroup group) {
    m_group = group;
    update();
}

void MixWaveformView::setSnapToBeats(bool snap) {
    m_snap = snap;
}

void MixWaveformView::setTempoRange(double startBpm, double targetBpm) {
    m_startBpm = startBpm;
    m_targetBpm = targetBpm;
    update();
}

void MixWaveformView::setPlayhead(double mixTime) {
    m_playhead = mixTime;
    update();
}

void MixWaveformView::setPlaceholderText(const QString& text) {
    m_placeholder = text;
    update();
}

void MixWaveformView::frameTransition() {
    const double padding = std::max(6.0, m_transition.length * 0.5);
    m_viewSpan = std::clamp(m_transition.length + 2 * padding, kMinSpan, kMaxSpan);
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
        m_transition.fromTrigger = std::max(0.0, snapToBeat(0, m_transition.fromTrigger));
    } else if (m_drag == Drag::LaneB) {
        m_transition.toStart = std::max(0.0, snapToBeat(1, m_transition.toStart));
    } else if (m_drag == Drag::Length) {
        const double beat = beatLength();
        if (beat > 0) {
            m_transition.length = std::max(0.0, std::round(m_transition.length / beat) * beat);
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

QRectF MixWaveformView::curveRect(MixLane lane) const {
    if (lane == MixLane::Tempo) {
        return QRectF(laneRect(0).topLeft(), laneRect(1).bottomRight()).adjusted(0, 6, 0, -6);
    }
    return laneRect(isDeckBLane(lane) ? 1 : 0).adjusted(0, 6, 0, -6);
}

double MixWaveformView::xForMixTime(double mixTime) const {
    return (mixTime - m_viewStart) / m_viewSpan * width();
}

double MixWaveformView::mixTimeForX(double x) const {
    return m_viewStart + x / std::max(1, width()) * m_viewSpan;
}

double MixWaveformView::laneTime(int lane, double mixTime) const {
    return (lane == 0 ? m_transition.fromTrigger : m_transition.toStart) + mixTime;
}

double MixWaveformView::envelopeAt(int lane, double trackSeconds) const {
    const QVector<float>& envelope = m_lanes[lane].envelope;
    const int index = static_cast<int>(trackSeconds * kBinsPerSecond);
    if (index < 0 || index >= envelope.size()) {
        return -1.0;
    }
    return envelope.at(index);
}

double MixWaveformView::overlapLength() const {
    return std::max(0.0, m_transition.length);
}

QPointF MixWaveformView::pointPos(MixLane lane, const MixPoint& point) const {
    const QRectF r = curveRect(lane);
    const double x0 = xForMixTime(0.0);
    const double x1 = xForMixTime(overlapLength());
    return QPointF(x0 + (x1 - x0) * point.x, r.bottom() - point.y * r.height());
}

MixPoint MixWaveformView::pointFromPos(MixLane lane, const QPointF& pos) const {
    const QRectF r = curveRect(lane);
    const double x0 = xForMixTime(0.0);
    const double x1 = xForMixTime(overlapLength());
    MixPoint p;
    p.x = x1 > x0 ? std::clamp((pos.x() - x0) / (x1 - x0), 0.0, 1.0) : 0.0;
    p.y = r.height() > 0 ? std::clamp((r.bottom() - pos.y()) / r.height(), 0.0, 1.0) : 0.0;
    if (hasNeutralMiddle(lane) && std::fabs(p.y - 0.5) < kNeutralSnap) {
        p.y = 0.5;
    }
    return p;
}

QVector<MixLane> MixWaveformView::editableLanes() const {
    switch (m_group) {
    case LaneGroup::Volume:
        return {MixLane::VolumeA, MixLane::VolumeB};
    case LaneGroup::Bass:
        return {MixLane::LowA, MixLane::LowB};
    case LaneGroup::Mid:
        return {MixLane::MidA, MixLane::MidB};
    case LaneGroup::Treble:
        return {MixLane::HighA, MixLane::HighB};
    case LaneGroup::Filter:
        return {MixLane::FilterA, MixLane::FilterB};
    case LaneGroup::Tempo:
        return {MixLane::Tempo};
    }
    return {};
}

bool MixWaveformView::inOverlap(const QPointF& pos) const {
    const double x0 = xForMixTime(0.0);
    const double x1 = xForMixTime(overlapLength());
    return x1 - x0 > 6 && pos.x() >= x0 - kPointGrab && pos.x() <= x1 + kPointGrab &&
            pos.y() > kRulerHeight;
}

MixLane MixWaveformView::laneAt(const QPointF& pos) const {
    for (MixLane lane : editableLanes()) {
        if (curveRect(lane).adjusted(0, -6, 0, 6).contains(pos)) {
            return lane;
        }
    }
    return MixLane::Count;
}

MixWaveformView::PointHit MixWaveformView::hitPoint(const QPointF& pos) const {
    PointHit best;
    double bestDistance = kPointGrab;
    for (MixLane lane : editableLanes()) {
        const QVector<MixPoint> points = m_transition.lane(lane);
        for (int i = 0; i < points.size(); ++i) {
            const QPointF p = pointPos(lane, points[i]);
            const double distance = std::hypot(p.x() - pos.x(), p.y() - pos.y());
            if (distance <= bestDistance) {
                bestDistance = distance;
                best.lane = lane;
                best.index = i;
            }
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void MixWaveformView::paintEvent(QPaintEvent* /*pEvent*/) {
    QPainter painter(this);
    painter.fillRect(rect(), kBackground);

    if (!m_placeholder.isEmpty()) {
        painter.setPen(kTextColor);
        painter.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, m_placeholder);
        return;
    }

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
}

void MixWaveformView::paintRuler(QPainter* pPainter) {
    const QRectF ruler = rulerRect();
    pPainter->fillRect(ruler, QColor(0x12, 0x13, 0x17));
    const double secondsPerPixel = m_viewSpan / std::max(1, width());
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
        pPainter->drawLine(QPointF(x, ruler.bottom() - 4), QPointF(x, ruler.bottom()));
        const QString label = t == 0 ? QStringLiteral("0")
                                     : QStringLiteral("%1%2s").arg(t > 0 ? "+" : "").arg(t);
        pPainter->drawText(QRectF(x + 3, 0, 60, ruler.height() - 2),
                Qt::AlignLeft | Qt::AlignVCenter,
                label);
    }
}

void MixWaveformView::paintLane(QPainter* pPainter, int lane) {
    const QRectF r = laneRect(lane);
    const QColor color = lane == 0 ? kColorA : kColorB;
    const double centerY = r.center().y();
    const double halfHeight = r.height() / 2.0 - 2;
    const double mixEnd = overlapLength();

    if (m_lanes[lane].envelope.isEmpty()) {
        pPainter->setPen(kTextColor);
        pPainter->drawText(r,
                Qt::AlignCenter,
                m_lanes[lane].loading ? tr("Loading waveform...") : QString());
    } else {
        const int w = width();
        for (int x = 0; x < w; ++x) {
            const double mixTime = mixTimeForX(x + 0.5);
            const double value = envelopeAt(lane, laneTime(lane, mixTime));
            if (value < 0) {
                continue;
            }
            const bool audible = lane == 0 ? mixTime <= mixEnd : mixTime >= 0;
            QColor c = color;
            c.setAlpha(audible ? 150 : 45);
            pPainter->setPen(c);
            const double h = std::max(1.0, value * halfHeight);
            pPainter->drawLine(QPointF(x, centerY - h), QPointF(x, centerY + h));
        }
    }

    const TrackPointer& pTrack = m_lanes[lane].pTrack;
    if (pTrack) {
        const QString label = QStringLiteral("%1  @ %2")
                                      .arg(lane == 0 ? QStringLiteral("A") : QStringLiteral("B"),
                                              formatTime(lane == 0
                                                              ? m_transition.fromTrigger
                                                              : m_transition.toStart));
        pPainter->setPen(Qt::white);
        pPainter->drawText(r.adjusted(6, 2, -6, -2), Qt::AlignLeft | Qt::AlignTop, label);
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
    if (beatLength() > 0 && beatLength() / m_viewSpan * width() < 4) {
        return;
    }
    const QRectF r = laneRect(lane);
    const double startSeconds = std::max(0.0, laneTime(lane, m_viewStart));
    const double endSeconds = laneTime(lane, m_viewStart + m_viewSpan);
    pPainter->setPen(QColor(255, 255, 255, 45));
    auto pos = pBeats->findNextBeat(mixxx::audio::FramePos(startSeconds * sampleRate));
    for (int i = 0; i < kMaxBeatTicks && pos.isValid(); ++i) {
        const double seconds = pos.value() / sampleRate;
        if (seconds > endSeconds) {
            break;
        }
        const double x = xForMixTime(seconds - laneTime(lane, 0.0));
        pPainter->drawLine(QPointF(x, r.top()), QPointF(x, r.top() + 5));
        pPainter->drawLine(QPointF(x, r.bottom() - 5), QPointF(x, r.bottom()));
        const auto next = pBeats->findNextBeat(pos + 1);
        if (!next.isValid() || next <= pos) {
            break;
        }
        pos = next;
    }
}

void MixWaveformView::paintLine(QPainter* pPainter, MixLane lane, bool editable) {
    const QVector<MixPoint> points = m_transition.lane(lane);
    QColor color = lane == MixLane::Tempo ? kColorTempo : (isDeckBLane(lane) ? kColorB : kColorA);
    if (!editable) {
        color.setAlpha(70);
    }
    QPainterPath path;
    for (int i = 0; i < points.size(); ++i) {
        const QPointF p = pointPos(lane, points[i]);
        if (i == 0) {
            path.moveTo(p);
        } else {
            path.lineTo(p);
        }
    }
    if (editable && (lane == MixLane::VolumeA || lane == MixLane::VolumeB)) {
        // Shade the level under the volume lines.
        const QRectF r = curveRect(lane);
        QPainterPath fill = path;
        fill.lineTo(pointPos(lane, points.last()).x(), r.bottom());
        fill.lineTo(pointPos(lane, points.first()).x(), r.bottom());
        fill.closeSubpath();
        QColor shade = color;
        shade.setAlpha(40);
        pPainter->fillPath(fill, shade);
    }
    pPainter->setBrush(Qt::NoBrush);
    pPainter->setPen(QPen(color, editable ? 2.0 : 1.2));
    pPainter->drawPath(path);
    if (editable) {
        pPainter->setBrush(color);
        pPainter->setPen(QPen(kBackground, 1.5));
        for (const MixPoint& point : points) {
            pPainter->drawEllipse(pointPos(lane, point), kPointRadius, kPointRadius);
        }
    }
}

void MixWaveformView::paintOverlap(QPainter* pPainter) {
    const QRectF laneA = laneRect(0);
    const QRectF laneB = laneRect(1);
    const double x0 = xForMixTime(0.0);
    const double x1 = xForMixTime(overlapLength());
    pPainter->fillRect(QRectF(QPointF(x0, laneA.top()), QPointF(x1, laneB.bottom())),
            QColor(255, 255, 255, 14));

    pPainter->setRenderHint(QPainter::Antialiasing, true);
    if (x1 - x0 > 6) {
        const QVector<MixLane> editable = editableLanes();
        // "Unchanged" guide for EQ / filter lines.
        if (m_group != LaneGroup::Volume && m_group != LaneGroup::Tempo) {
            pPainter->setPen(QPen(QColor(255, 255, 255, 50), 1, Qt::DashLine));
            for (MixLane lane : editable) {
                const double y = pointPos(lane, {0.0, 0.5}).y();
                pPainter->drawLine(QPointF(x0, y), QPointF(x1, y));
            }
        }
        // Lines of other groups that carry edits, faint.
        for (int i = 0; i < kMixLaneCount; ++i) {
            const auto lane = static_cast<MixLane>(i);
            if (!editable.contains(lane) && m_transition.isLaneActive(lane)) {
                paintLine(pPainter, lane, false);
            }
        }
        for (MixLane lane : editable) {
            paintLine(pPainter, lane, true);
        }
        if (m_group == LaneGroup::Tempo && m_startBpm > 0) {
            pPainter->setPen(kTextColor);
            const QRectF r = curveRect(MixLane::Tempo);
            pPainter->drawText(QRectF(x0 + 4, r.bottom() - 16, 120, 16),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    QStringLiteral("%1 BPM").arg(m_startBpm, 0, 'f', 1));
            pPainter->drawText(QRectF(x0 + 4, r.top(), 120, 16),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    QStringLiteral("%1 BPM").arg(m_targetBpm, 0, 'f', 1));
        }
    }
    pPainter->setRenderHint(QPainter::Antialiasing, false);

    pPainter->setPen(QPen(QColor(255, 255, 255, 170), 1, Qt::DashLine));
    pPainter->drawLine(QPointF(x0, kRulerHeight), QPointF(x0, height()));
    pPainter->setPen(QPen(QColor(255, 255, 255, 170), 1));
    pPainter->drawLine(QPointF(x1, kRulerHeight), QPointF(x1, height()));
    const QRectF handle(x1 - 4, laneA.bottom() - 3, 8, kLaneGap + 6);
    pPainter->setBrush(Qt::white);
    pPainter->drawRoundedRect(handle, 2, 2);
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

void MixWaveformView::mousePressEvent(QMouseEvent* pEvent) {
    if (!m_placeholder.isEmpty()) {
        return;
    }
    const QPointF pos = pEvent->position();
    m_dragStartPos = pos;
    m_dragStartTransition = m_transition;
    m_dragStartViewStart = m_viewStart;
    const bool alt = pEvent->modifiers() & Qt::AltModifier;

    // Right-click deletes a point.
    if (pEvent->button() == Qt::RightButton) {
        const PointHit hit = hitPoint(pos);
        if (hit.index > 0) {
            QVector<MixPoint> points = m_transition.lane(hit.lane);
            if (hit.index < points.size() - 1) {
                points.remove(hit.index);
                m_transition.setLane(hit.lane, points);
                update();
                emit transitionEdited();
            }
        }
        return;
    }
    if (pEvent->button() != Qt::LeftButton) {
        return;
    }

    const double endX = xForMixTime(overlapLength());
    if (std::fabs(pos.x() - endX) <= kHandleGrab && laneRect(0).bottom() - 8 <= pos.y() &&
            pos.y() <= laneRect(1).top() + 8) {
        m_drag = Drag::Length;
    } else if (!alt && inOverlap(pos) && hitPoint(pos).index >= 0) {
        m_drag = Drag::Point;
        m_dragPoint = hitPoint(pos);
    } else if (!alt && inOverlap(pos) && laneAt(pos) != MixLane::Count) {
        // Click on empty space inside the overlap adds a point there.
        const MixLane lane = laneAt(pos);
        QVector<MixPoint> points = m_transition.lane(lane);
        const MixPoint point = pointFromPos(lane, pos);
        int index = 0;
        while (index < points.size() && points[index].x < point.x) {
            ++index;
        }
        if (index == 0) {
            index = 1; // keep the start point first
        }
        if (index >= points.size()) {
            index = points.size() - 1; // and the end point last
        }
        points.insert(index, point);
        m_transition.lanes.insert(static_cast<int>(lane), points);
        m_drag = Drag::Point;
        m_dragPoint = {lane, index};
        update();
        emit transitionEdited();
    } else if (laneRect(0).contains(pos)) {
        m_drag = Drag::LaneA;
    } else if (laneRect(1).contains(pos)) {
        m_drag = Drag::LaneB;
    } else {
        m_drag = Drag::Pan;
    }
    setCursor(m_drag == Drag::Length ? Qt::SizeHorCursor
                    : m_drag == Drag::Point ? Qt::CrossCursor
                                            : Qt::ClosedHandCursor);
}

void MixWaveformView::mouseMoveEvent(QMouseEvent* pEvent) {
    const QPointF pos = pEvent->position();
    if (m_drag == Drag::None) {
        const double endX = xForMixTime(overlapLength());
        if (std::fabs(pos.x() - endX) <= kHandleGrab && laneRect(0).bottom() - 8 <= pos.y() &&
                pos.y() <= laneRect(1).top() + 8) {
            setCursor(Qt::SizeHorCursor);
        } else if (!(pEvent->modifiers() & Qt::AltModifier) && inOverlap(pos) &&
                laneAt(pos) != MixLane::Count) {
            setCursor(hitPoint(pos).index >= 0 ? Qt::PointingHandCursor : Qt::CrossCursor);
        } else {
            setCursor(Qt::OpenHandCursor);
        }
        return;
    }
    const double dx = (pos.x() - m_dragStartPos.x()) / std::max(1, width()) * m_viewSpan;
    const double fromDuration = m_lanes[0].pTrack ? m_lanes[0].pTrack->getDuration() : 1e9;
    const double toDuration = m_lanes[1].pTrack ? m_lanes[1].pTrack->getDuration() : 1e9;
    switch (m_drag) {
    case Drag::LaneA:
        m_transition.fromTrigger = std::clamp(m_dragStartTransition.fromTrigger - dx,
                0.0,
                std::max(0.0, fromDuration - 1.0));
        break;
    case Drag::LaneB:
        m_transition.toStart = std::clamp(m_dragStartTransition.toStart - dx,
                0.0,
                std::max(0.0, toDuration - 1.0));
        break;
    case Drag::Length:
        m_transition.length = std::clamp(mixTimeForX(pos.x()), 0.0, 120.0);
        break;
    case Drag::Point: {
        QVector<MixPoint> points = m_transition.lane(m_dragPoint.lane);
        if (m_dragPoint.index < 0 || m_dragPoint.index >= points.size()) {
            return;
        }
        MixPoint point = pointFromPos(m_dragPoint.lane, pos);
        const int last = points.size() - 1;
        if (m_dragPoint.index == 0) {
            point.x = 0.0; // the ends stay at the start / end of the overlap
        } else if (m_dragPoint.index == last) {
            point.x = 1.0;
        } else {
            // Keep the order of the points.
            point.x = std::clamp(point.x,
                    points[m_dragPoint.index - 1].x,
                    points[m_dragPoint.index + 1].x);
        }
        points[m_dragPoint.index] = point;
        m_transition.lanes.insert(static_cast<int>(m_dragPoint.lane), points);
        update();
        emit transitionEdited();
        return;
    }
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
    if (m_drag == Drag::Point) {
        // Normalize (drops lines that went back to their default).
        m_transition.setLane(m_dragPoint.lane, m_transition.lane(m_dragPoint.lane));
        emit transitionEdited();
    }
    m_drag = Drag::None;
    setCursor(Qt::OpenHandCursor);
}

void MixWaveformView::mouseDoubleClickEvent(QMouseEvent* pEvent) {
    const PointHit hit = hitPoint(pEvent->position());
    if (hit.index <= 0) {
        return;
    }
    QVector<MixPoint> points = m_transition.lane(hit.lane);
    if (hit.index >= points.size() - 1) {
        return;
    }
    points.remove(hit.index);
    m_transition.setLane(hit.lane, points);
    m_drag = Drag::None;
    update();
    emit transitionEdited();
}

void MixWaveformView::wheelEvent(QWheelEvent* pEvent) {
    const double anchor = mixTimeForX(pEvent->position().x());
    const double factor = pEvent->angleDelta().y() > 0 ? 0.8 : 1.25;
    const double newSpan = std::clamp(m_viewSpan * factor, kMinSpan, kMaxSpan);
    m_viewStart = anchor - (anchor - m_viewStart) * newSpan / m_viewSpan;
    m_viewSpan = newSpan;
    update();
    pEvent->accept();
}
