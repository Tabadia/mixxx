#include "library/autodj/mix/dlgmixtransition.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <cmath>
#include <limits>

#include "control/controlproxy.h"
#include "library/autodj/autodjprocessor.h"
#include "library/autodj/mix/mixwaveformview.h"
#include "mixer/playerinfo.h"
#include "moc_dlgmixtransition.cpp"
#include "track/beats.h"
#include "track/cue.h"
#include "track/track.h"

namespace {

const QString kDeckGroups[2] = {QStringLiteral("[Channel1]"), QStringLiteral("[Channel2]")};
constexpr double kPreviewLeadIn = 6.0;
constexpr double kPreviewTail = 6.0;
constexpr int kPreviewIntervalMs = 25;
constexpr int kLoadTimeoutTicks = 15000 / kPreviewIntervalMs;

QString bpmText(const TrackPointer& pTrack) {
    if (!pTrack || pTrack->getBpm() <= 0) {
        return QObject::tr("BPM ?");
    }
    return QStringLiteral("%1 BPM").arg(pTrack->getBpm(), 0, 'f', 1);
}

QString keyText(const TrackPointer& pTrack) {
    const QString key = pTrack ? pTrack->getKeyText() : QString();
    return key.isEmpty() ? QObject::tr("key ?") : key;
}

QString trackName(const TrackPointer& pTrack) {
    if (!pTrack) {
        return QString();
    }
    return pTrack->getInfo().isEmpty() ? pTrack->getLocation() : pTrack->getInfo();
}

double cueSeconds(const TrackPointer& pTrack, mixxx::CueType type, bool end) {
    if (!pTrack) {
        return -1;
    }
    const CuePointer pCue = pTrack->findCueByType(type);
    const double sampleRate = pTrack->getSampleRate();
    if (!pCue || sampleRate <= 0) {
        return -1;
    }
    const auto pos = end ? pCue->getEndPosition() : pCue->getPosition();
    return pos.isValid() ? pos.value() / sampleRate : -1;
}

/// mm:ss.s editor for a time in seconds.
class TimeSpinBox : public QDoubleSpinBox {
  public:
    explicit TimeSpinBox(QWidget* pParent)
            : QDoubleSpinBox(pParent) {
        setDecimals(1);
        setSingleStep(0.5);
        setAccelerated(true);
        setKeyboardTracking(false);
    }

  protected:
    QString textFromValue(double value) const override {
        const int minutes = static_cast<int>(value / 60);
        return QStringLiteral("%1:%2").arg(minutes).arg(
                value - minutes * 60, 4, 'f', 1, QChar('0'));
    }
    double valueFromText(const QString& text) const override {
        const QStringList parts = text.trimmed().split(':');
        if (parts.size() == 2) {
            return parts.at(0).toInt() * 60 + parts.at(1).toDouble();
        }
        return text.toDouble();
    }
    QValidator::State validate(QString& text, int& /*pos*/) const override {
        static const QRegularExpression re(QStringLiteral("^\\d{0,3}(:\\d{0,2}(\\.\\d?)?)?$"));
        return re.match(text.trimmed()).hasMatch() ? QValidator::Acceptable
                                                   : QValidator::Invalid;
    }
};

} // anonymous namespace

DlgMixTransition::DlgMixTransition(QWidget* pParent,
        AutoDJProcessor* pProcessor,
        TrackPointer pFrom,
        TrackPointer pTo)
        : QDialog(pParent),
          m_pProcessor(pProcessor),
          m_pStore(pProcessor->mixTransitionStore()),
          m_pFrom(pFrom),
          m_pTo(pTo),
          m_updating(false),
          m_previewState(PreviewState::Idle),
          m_previewTicks(0) {
    setWindowTitle(tr("Mix Transition"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(980, 560);

    // --- header ---
    m_pHeader = new QLabel(this);
    m_pHeader->setTextFormat(Qt::RichText);
    m_pHeader->setWordWrap(true);
    m_pMatchInfo = new QLabel(this);
    m_pMatchInfo->setWordWrap(true);

    // --- waveform ---
    m_pView = new MixWaveformView(this);
    m_pView->setTracks(m_pFrom, m_pTo);
    connect(m_pView, &MixWaveformView::transitionEdited, this, &DlgMixTransition::updateFromView);
    auto* pHint = new QLabel(
            tr("Drag the top waveform to choose where the current track mixes "
               "out, drag the bottom one to choose where the next track comes "
               "in, drag the white handle to change the length. Scroll to "
               "zoom, drag the ruler to pan."),
            this);
    pHint->setWordWrap(true);
    QFont hintFont = pHint->font();
    hintFont.setPointSizeF(hintFont.pointSizeF() * 0.9);
    pHint->setFont(hintFont);

    // --- controls ---
    m_pStyle = new QComboBox(this);
    for (const auto style : {MixTransition::Style::Fade, MixTransition::Style::Overlap}) {
        m_pStyle->addItem(MixTransition::styleName(style), static_cast<int>(style));
    }
    m_pStyleDescription = new QLabel(this);
    m_pStyleDescription->setWordWrap(true);

    m_pCurve = new QComboBox(this);
    for (int i = 0; i < MixTransition::kCurveCount; ++i) {
        m_pCurve->addItem(MixTransition::curveName(static_cast<MixTransition::Curve>(i)), i);
    }

    m_pLength = new QDoubleSpinBox(this);
    m_pLength->setRange(0.0, 120.0);
    m_pLength->setDecimals(1);
    m_pLength->setSingleStep(0.5);
    m_pLength->setSuffix(tr(" s"));
    m_pLength->setKeyboardTracking(false);
    m_pLengthBeats = new QLabel(this);

    m_pTrigger = new TimeSpinBox(this);
    m_pTrigger->setRange(0.0, qMax(1.0, m_pFrom->getDuration()));
    m_pStart = new TimeSpinBox(this);
    m_pStart->setRange(0.0, qMax(1.0, m_pTo->getDuration()));

    // The volume curve stays at its default; the picker is kept hidden.
    m_pCurve->hide();

    m_pSync = new QCheckBox(tr("Match BPM (sync B's tempo and beats to A)"), this);
    m_pSnap = new QCheckBox(tr("Snap to beats"), this);
    m_pSnap->setChecked(true);

    auto* pLengthRow = new QHBoxLayout();
    pLengthRow->addWidget(m_pLength);
    pLengthRow->addWidget(m_pLengthBeats, 1);

    auto* pForm = new QFormLayout();
    pForm->addRow(tr("Transition"), m_pStyle);
    pForm->addRow(QString(), m_pStyleDescription);
    pForm->addRow(tr("Overlap length"), pLengthRow);
    auto* pForm2 = new QFormLayout();
    pForm2->addRow(tr("Mix out of A at"), m_pTrigger);
    pForm2->addRow(tr("Bring B in from"), m_pStart);
    pForm2->addRow(QString(), m_pSync);
    pForm2->addRow(QString(), m_pSnap);
    auto* pControls = new QHBoxLayout();
    pControls->addLayout(pForm, 1);
    pControls->addSpacing(24);
    pControls->addLayout(pForm2, 1);

    // --- buttons ---
    auto* pSuggest = new QPushButton(tr("Suggest"), this);
    pSuggest->setToolTip(tr("Pick a transition automatically from the beat grid, "
                            "intro/outro markers and tempos."));
    m_pPreview = new QPushButton(tr("Preview"), this);
    m_pPreview->setToolTip(tr("Play the transition on decks 1 and 2."));
    m_pRemove = new QPushButton(tr("Remove"), this);
    auto* pCancel = new QPushButton(tr("Cancel"), this);
    auto* pSave = new QPushButton(tr("Save"), this);
    pSave->setDefault(true);
    m_pStatus = new QLabel(this);

    auto* pButtons = new QHBoxLayout();
    pButtons->addWidget(pSuggest);
    pButtons->addWidget(m_pPreview);
    pButtons->addWidget(m_pRemove);
    pButtons->addWidget(m_pStatus, 1);
    pButtons->addWidget(pCancel);
    pButtons->addWidget(pSave);

    auto* pLayout = new QVBoxLayout(this);
    pLayout->addWidget(m_pHeader);
    pLayout->addWidget(m_pMatchInfo);
    pLayout->addWidget(m_pView, 1);
    pLayout->addWidget(pHint);
    pLayout->addLayout(pControls);
    pLayout->addLayout(pButtons);

    connect(pSuggest, &QPushButton::clicked, this, &DlgMixTransition::slotSuggest);
    connect(m_pPreview, &QPushButton::clicked, this, &DlgMixTransition::slotTogglePreview);
    connect(m_pRemove, &QPushButton::clicked, this, &DlgMixTransition::slotRemove);
    connect(pCancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(pSave, &QPushButton::clicked, this, &DlgMixTransition::slotSave);

    auto onControlChanged = [this]() {
        if (m_updating) {
            return;
        }
        m_pView->setTransition(currentTransition());
        updateInfo();
    };
    connect(m_pStyle, QOverload<int>::of(&QComboBox::currentIndexChanged), this, onControlChanged);
    connect(m_pCurve, QOverload<int>::of(&QComboBox::currentIndexChanged), this, onControlChanged);
    connect(m_pLength, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onControlChanged);
    connect(m_pTrigger, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onControlChanged);
    connect(m_pStart, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onControlChanged);
    connect(m_pSync, &QCheckBox::toggled, this, onControlChanged);
    connect(m_pSnap, &QCheckBox::toggled, m_pView, &MixWaveformView::setSnapToBeats);

    // --- preview controls ---
    for (int i = 0; i < 2; ++i) {
        m_pPlay[i] = std::make_unique<ControlProxy>(kDeckGroups[i], QStringLiteral("play"));
        m_pPlayPos[i] = std::make_unique<ControlProxy>(
                kDeckGroups[i], QStringLiteral("playposition"));
        m_pDuration[i] = std::make_unique<ControlProxy>(kDeckGroups[i], QStringLiteral("duration"));
        m_pTrackLoaded[i] = std::make_unique<ControlProxy>(
                kDeckGroups[i], QStringLiteral("track_loaded"));
    }
    m_pCrossfader = std::make_unique<ControlProxy>(
            QStringLiteral("[Master]"), QStringLiteral("crossfader"));
    m_pCrossfaderReverse = std::make_unique<ControlProxy>(
            QStringLiteral("[Mixer Profile]"), QStringLiteral("xFaderReverse"));
    m_previewTimer.setInterval(kPreviewIntervalMs);
    connect(&m_previewTimer, &QTimer::timeout, this, &DlgMixTransition::slotPreviewTick);

    // --- initial state ---
    const std::optional<MixTransition> existing =
            m_pStore->get(m_pFrom->getId(), m_pTo->getId());
    m_pRemove->setEnabled(existing.has_value());
    m_pView->setTransition(existing ? *existing : suggestTransition());
    updateControls();
    m_pView->frameTransition();
    if (!existing) {
        m_pStatus->setText(tr("Suggested transition - adjust it and press Save."));
    }
}

DlgMixTransition::~DlgMixTransition() {
    if (m_previewState != PreviewState::Idle) {
        stopPreview();
    }
}

MixTransition DlgMixTransition::currentTransition() const {
    MixTransition t;
    t.style = static_cast<MixTransition::Style>(m_pStyle->currentData().toInt());
    t.curve = static_cast<MixTransition::Curve>(m_pCurve->currentData().toInt());
    t.length = m_pLength->value();
    t.fromTrigger = m_pTrigger->value();
    t.toStart = m_pStart->value();
    t.syncTempo = m_pSync->isChecked();
    return t;
}

void DlgMixTransition::updateFromView() {
    updateControls();
}

void DlgMixTransition::updateControls() {
    const MixTransition& t = m_pView->transition();
    m_updating = true;
    const int styleIndex = m_pStyle->findData(static_cast<int>(t.style));
    m_pStyle->setCurrentIndex(styleIndex >= 0 ? styleIndex : 0);
    m_pCurve->setCurrentIndex(m_pCurve->findData(static_cast<int>(t.curve)));
    m_pLength->setValue(t.length);
    m_pTrigger->setValue(t.fromTrigger);
    m_pStart->setValue(t.toStart);
    m_pSync->setChecked(t.syncTempo);
    m_updating = false;
    updateInfo();
}

void DlgMixTransition::updateInfo() {
    const MixTransition t = currentTransition();
    const double bpmA = m_pFrom->getBpm();
    const double bpmB = m_pTo->getBpm();

    m_pHeader->setText(QStringLiteral(
            "<b><span style='color:#ff7a1a'>A</span> %1</b> &nbsp;(%2 &middot; %3)"
            "<br><b><span style='color:#2bb6e0'>B</span> %4</b> &nbsp;(%5 &middot; %6)")
                               .arg(trackName(m_pFrom).toHtmlEscaped(),
                                       bpmText(m_pFrom),
                                       keyText(m_pFrom).toHtmlEscaped(),
                                       trackName(m_pTo).toHtmlEscaped(),
                                       bpmText(m_pTo),
                                       keyText(m_pTo).toHtmlEscaped()));

    QString match;
    if (bpmA > 0 && bpmB > 0) {
        const double diff = (bpmB - bpmA) / bpmA * 100.0;
        if (MixTransition::bpmsCompatible(bpmA, bpmB, 0.005)) {
            match = tr("BPMs match.");
        } else if (MixTransition::bpmsCompatible(bpmA, bpmB, 0.10)) {
            match = tr("BPMs differ by %1%. Match BPM will line them up.")
                            .arg(diff, 0, 'f', 1);
        } else {
            match = tr("BPMs differ by %1%. Match BPM will change B's speed a "
                       "lot; a short Fade may sound better.")
                            .arg(diff, 0, 'f', 1);
        }
    } else {
        match = tr("Analyze both tracks to see BPM, key and beat grid.");
    }
    m_pMatchInfo->setText(match);

    m_pStyleDescription->setText(MixTransition::styleDescription(t.style));
    const bool isCut = t.style == MixTransition::Style::Cut;
    m_pLength->setEnabled(!isCut);
    m_pCurve->setEnabled(!isCut);
    const double beat = m_pView->beatLength();
    m_pLengthBeats->setText(beat > 0 && !isCut
                    ? tr("= %1 beats (%2 bars)")
                              .arg(t.length / beat, 0, 'f', 1)
                              .arg(t.length / beat / 4.0, 0, 'f', 1)
                    : QString());
}

MixTransition DlgMixTransition::suggestTransition() const {
    MixTransition t;
    const double durationA = m_pFrom->getDuration();
    const double bpmA = m_pFrom->getBpm();
    const double bpmB = m_pTo->getBpm();
    const double beat = bpmA > 0 ? 60.0 / bpmA : 0.5;

    // Length: use the markers if present, otherwise 16 beats (4 bars).
    const double outroStart = cueSeconds(m_pFrom, mixxx::CueType::Outro, false);
    const double outroEnd = cueSeconds(m_pFrom, mixxx::CueType::Outro, true);
    const double introStart = cueSeconds(m_pTo, mixxx::CueType::Intro, false);
    const double introEnd = cueSeconds(m_pTo, mixxx::CueType::Intro, true);
    double length = 16 * beat;
    if (outroStart >= 0 && outroEnd > outroStart) {
        length = outroEnd - outroStart;
    } else if (introStart >= 0 && introEnd > introStart) {
        length = introEnd - introStart;
    }
    t.length = qBound(4.0, length, 32.0);

    // Mix out at the outro if marked, else so the mix ends ~16 beats before
    // the end of the track, on a beat.
    if (outroStart >= 0 && outroStart < durationA) {
        t.fromTrigger = outroStart;
    } else {
        t.fromTrigger = qMax(0.0, durationA - t.length - 16 * beat);
        t.fromTrigger = m_pView->snapToBeat(0, t.fromTrigger);
    }

    // Bring B in at its intro, or its first beat.
    if (introStart >= 0) {
        t.toStart = introStart;
    } else {
        const mixxx::BeatsPointer pBeats = m_pTo->getBeats();
        const double sampleRate = m_pTo->getSampleRate();
        t.toStart = (pBeats && sampleRate > 0 && pBeats->firstBeat().isValid())
                ? pBeats->firstBeat().value() / sampleRate
                : 0.0;
    }

    t.style = MixTransition::Style::Fade;
    t.curve = MixTransition::Curve::Smooth;
    t.syncTempo = MixTransition::bpmsCompatible(bpmA, bpmB, 0.10);
    return t;
}

void DlgMixTransition::slotSuggest() {
    m_pView->setTransition(suggestTransition());
    updateControls();
    m_pView->frameTransition();
    m_pStatus->setText(tr("Suggested transition."));
}

void DlgMixTransition::slotSave() {
    m_pStore->set(m_pFrom, m_pTo, currentTransition());
    m_pProcessor->refreshTransition();
    accept();
}

void DlgMixTransition::slotRemove() {
    m_pStore->remove(m_pFrom->getId(), m_pTo->getId());
    m_pProcessor->refreshTransition();
    accept();
}

// ---------------------------------------------------------------------------
// Preview
// ---------------------------------------------------------------------------

bool DlgMixTransition::deckHasTrack(int deck, const TrackPointer& pTrack) const {
    if (m_pTrackLoaded[deck]->get() <= 0) {
        return false;
    }
    const TrackPointer pLoaded = PlayerInfo::instance().getTrackInfo(kDeckGroups[deck]);
    return pLoaded && pLoaded == pTrack;
}

double DlgMixTransition::deckPosition(int deck) const {
    return m_pPlayPos[deck]->get() * m_pDuration[deck]->get();
}

void DlgMixTransition::setCrossfaderToward(double t) {
    // Deck 1 is on the left (-1), deck 2 on the right (+1).
    double value = -1.0 + 2.0 * qBound(0.0, t, 1.0);
    if (m_pCrossfaderReverse->get() > 0) {
        value = -value;
    }
    m_pCrossfader->set(value);
}

void DlgMixTransition::slotTogglePreview() {
    if (m_previewState != PreviewState::Idle) {
        stopPreview(tr("Preview stopped."));
        return;
    }
    if (m_pProcessor->getState() != AutoDJProcessor::ADJ_DISABLED) {
        QMessageBox::information(this,
                tr("Mix Transition"),
                tr("Turn off Auto DJ to preview a transition on decks 1 and 2."));
        return;
    }
    if (m_pPlay[0]->get() > 0 || m_pPlay[1]->get() > 0) {
        if (QMessageBox::question(this,
                    tr("Mix Transition"),
                    tr("Preview will load the two tracks into decks 1 and 2 and "
                       "stop what is playing there. Continue?")) != QMessageBox::Yes) {
            return;
        }
        m_pPlay[0]->set(0.0);
        m_pPlay[1]->set(0.0);
    }
    m_previewTransition = currentTransition();
    if (m_previewTransition.style == MixTransition::Style::Cut) {
        m_previewTransition.length = 0.0;
    }
    if (!deckHasTrack(0, m_pFrom)) {
        emit loadTrackToPlayer(m_pFrom, kDeckGroups[0], false);
    }
    if (!deckHasTrack(1, m_pTo)) {
        emit loadTrackToPlayer(m_pTo, kDeckGroups[1], false);
    }
    m_previewState = PreviewState::Loading;
    m_previewTicks = 0;
    m_pPreview->setText(tr("Stop"));
    m_pStatus->setText(tr("Loading tracks into decks 1 and 2..."));
    m_previewTimer.start();
}

void DlgMixTransition::slotPreviewTick() {
    ++m_previewTicks;
    const MixTransition& t = m_previewTransition;
    switch (m_previewState) {
    case PreviewState::Idle:
        m_previewTimer.stop();
        return;
    case PreviewState::Loading: {
        if (!deckHasTrack(0, m_pFrom) || !deckHasTrack(1, m_pTo) ||
                m_pDuration[0]->get() <= 0 || m_pDuration[1]->get() <= 0) {
            if (m_previewTicks > kLoadTimeoutTicks) {
                stopPreview(tr("Could not load the tracks into decks 1 and 2."));
            }
            return;
        }
        const double durationA = m_pDuration[0]->get();
        const double durationB = m_pDuration[1]->get();
        m_pPlayPos[0]->set(qMax(0.0, t.fromTrigger - kPreviewLeadIn) / durationA);
        m_pPlayPos[1]->set(t.toStart / durationB);
        setCrossfaderToward(0.0);
        m_pPlay[0]->set(1.0);
        m_previewState = PreviewState::LeadIn;
        m_previewTicks = 0;
        m_pStatus->setText(tr("Playing A..."));
        return;
    }
    case PreviewState::LeadIn: {
        const double posA = deckPosition(0);
        m_pView->setPlayhead(posA - t.fromTrigger);
        if (posA < t.fromTrigger) {
            if (m_previewTicks > 4 && m_pPlay[0]->get() <= 0) {
                stopPreview(tr("Preview stopped."));
            }
            return;
        }
        m_previewDriver.begin(kDeckGroups[0],
                kDeckGroups[1],
                t,
                t.effectiveStyle(m_pFrom->getBpm(), m_pTo->getBpm()));
        m_pPlay[1]->set(1.0);
        m_previewState = PreviewState::Mixing;
        m_pStatus->setText(tr("Mixing..."));
        [[fallthrough]];
    }
    case PreviewState::Mixing: {
        const double posA = deckPosition(0);
        m_pView->setPlayhead(posA - t.fromTrigger);
        const double progress = t.length > 0 ? (posA - t.fromTrigger) / t.length : 1.0;
        if (progress < 1.0) {
            setCrossfaderToward(m_previewDriver.apply(progress));
            return;
        }
        setCrossfaderToward(1.0);
        m_pPlay[0]->set(0.0);
        m_previewDriver.finish();
        m_previewState = PreviewState::Tail;
        m_previewTicks = 0;
        m_pStatus->setText(tr("Playing B..."));
        return;
    }
    case PreviewState::Tail: {
        const double posB = deckPosition(1);
        m_pView->setPlayhead(posB - t.toStart);
        if (m_previewTicks * kPreviewIntervalMs >= kPreviewTail * 1000) {
            stopPreview(tr("Preview finished."));
        }
        return;
    }
    }
}

void DlgMixTransition::stopPreview(const QString& message) {
    m_previewTimer.stop();
    if (m_previewState != PreviewState::Loading) {
        m_pPlay[0]->set(0.0);
        m_pPlay[1]->set(0.0);
    }
    m_previewDriver.finish();
    m_previewState = PreviewState::Idle;
    m_pView->setPlayhead(std::numeric_limits<double>::quiet_NaN());
    m_pPreview->setText(tr("Preview"));
    m_pStatus->setText(message);
}
