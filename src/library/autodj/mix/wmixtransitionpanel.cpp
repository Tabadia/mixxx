#include "library/autodj/mix/wmixtransitionpanel.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <cmath>
#include <limits>

#include "control/controlproxy.h"
#include "library/autodj/mix/mixtransitionplayer.h"
#include "mixer/playerinfo.h"
#include "moc_wmixtransitionpanel.cpp"
#include "track/beats.h"
#include "track/cue.h"
#include "track/track.h"

namespace {

const QString kDeckGroups[2] = {QStringLiteral("[Channel1]"), QStringLiteral("[Channel2]")};
constexpr int kSaveDelayMs = 400;

// Written as UTF-8 escapes: the sources aren't compiled as UTF-8 on MSVC.
const char* const kArrow = "\xE2\x86\x92"; // ->
const char* const kSwap = "\xE2\x87\x84";  // <->
const char* const kDot = "\xE2\x80\xA2";   // bullet

QString trackName(const TrackPointer& pTrack) {
    if (!pTrack) {
        return QString();
    }
    QString title = pTrack->getTitle();
    if (title.isEmpty()) {
        title = pTrack->getInfo();
    }
    constexpr int kMaxChars = 28;
    if (title.size() > kMaxChars) {
        title = title.left(kMaxChars - 3).trimmed() + QStringLiteral("...");
    }
    return title;
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

double snapToBeat(const TrackPointer& pTrack, double seconds) {
    if (!pTrack) {
        return seconds;
    }
    const mixxx::BeatsPointer pBeats = pTrack->getBeats();
    const double sampleRate = pTrack->getSampleRate();
    if (!pBeats || sampleRate <= 0) {
        return seconds;
    }
    const auto closest = pBeats->findClosestBeat(mixxx::audio::FramePos(seconds * sampleRate));
    return closest.isValid() ? closest.value() / sampleRate : seconds;
}

struct LaneButton {
    MixWaveformView::LaneGroup group;
    const char* label;
    MixLane laneA;
    MixLane laneB;
};
const LaneButton kLaneButtons[] = {
        {MixWaveformView::LaneGroup::Volume, "Volume", MixLane::VolumeA, MixLane::VolumeB},
        {MixWaveformView::LaneGroup::Bass, "Bass", MixLane::LowA, MixLane::LowB},
        {MixWaveformView::LaneGroup::Mid, "Mid", MixLane::MidA, MixLane::MidB},
        {MixWaveformView::LaneGroup::Treble, "Treble", MixLane::HighA, MixLane::HighB},
        {MixWaveformView::LaneGroup::Filter, "Filter", MixLane::FilterA, MixLane::FilterB},
        {MixWaveformView::LaneGroup::Tempo, "Tempo", MixLane::Tempo, MixLane::Tempo},
};

} // anonymous namespace

WMixTransitionPanel::WMixTransitionPanel(QWidget* pParent,
        MixTransitionStore* pStore,
        MixTransitionPlayer* pPlayer)
        : QWidget(pParent),
          m_pStore(pStore),
          m_pPlayer(pPlayer),
          m_fromDeck(0),
          m_manualDirection(false),
          m_updating(false),
          m_saved(false) {
    setObjectName(QStringLiteral("MixTransitionPanel"));

    // --- header row ---
    auto* pTitle = new QLabel(tr("Transition"), this);
    pTitle->setObjectName(QStringLiteral("MixTransitionTitle"));
    m_pTracks = new QLabel(this);
    m_pTracks->setTextFormat(Qt::RichText);
    m_pTracks->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_pTracks->setMinimumWidth(80);
    m_pSwap = new QPushButton(QString::fromUtf8(kSwap), this);
    m_pSwap->setToolTip(tr("Swap direction: transition from deck 2 into deck 1"));
    m_pSwap->setFixedWidth(34);

    m_pLaneButtons = new QButtonGroup(this);
    m_pLaneButtons->setExclusive(true);
    auto* pLaneRow = new QHBoxLayout();
    pLaneRow->setSpacing(2);
    for (int i = 0; i < static_cast<int>(std::size(kLaneButtons)); ++i) {
        auto* pButton = new QPushButton(tr(kLaneButtons[i].label), this);
        pButton->setCheckable(true);
        pButton->setProperty("segment", true);
        pButton->setToolTip(i == static_cast<int>(MixWaveformView::LaneGroup::Tempo)
                        ? tr("Edit the tempo curve (0 = start BPM, top = target BPM)")
                        : tr("Edit the %1 lines of both decks").arg(tr(kLaneButtons[i].label)));
        m_pLaneButtons->addButton(pButton, i);
        pLaneRow->addWidget(pButton);
    }
    m_pLaneButtons->button(0)->setChecked(true);

    m_pTempoMode = new QComboBox(this);
    m_pTempoMode->addItem(tr("Tempo: leave"), static_cast<int>(MixTransition::TempoMode::Off));
    m_pTempoMode->addItem(tr("Tempo: keep A"), static_cast<int>(MixTransition::TempoMode::KeepA));
    m_pTempoMode->addItem(tr("Tempo: go to B"), static_cast<int>(MixTransition::TempoMode::UseB));
    m_pTempoMode->addItem(tr("Tempo: custom"), static_cast<int>(MixTransition::TempoMode::Custom));
    m_pTempoMode->setToolTip(
            tr("Which BPM the mix ends at. B always joins at A's tempo so the\n"
               "two are beatmatched; 'go to B' / 'custom' then move both decks\n"
               "to the target, along the Ramp or the Tempo line."));
    m_pCustomBpm = new QDoubleSpinBox(this);
    m_pCustomBpm->setRange(40.0, 250.0);
    m_pCustomBpm->setDecimals(1);
    m_pCustomBpm->setSuffix(tr(" BPM"));
    m_pCustomBpm->setKeyboardTracking(false);
    m_pRamp = new QCheckBox(tr("Ramp"), this);
    m_pRamp->setToolTip(tr("Glide the tempo across the transition instead of\n"
                           "switching once A is out."));
    m_pSnap = new QCheckBox(tr("Snap"), this);
    m_pSnap->setChecked(true);
    m_pSnap->setToolTip(tr("Snap the trigger, entry point and length to beats"));

    m_pArm = new QPushButton(tr("Armed"), this);
    m_pArm->setCheckable(true);
    m_pArm->setToolTip(tr("When armed, saved transitions fire automatically when the\n"
                          "playing deck reaches the trigger (by hand and in Auto DJ)."));
    m_pReset = new QPushButton(tr("Reset"), this);
    m_pReset->setToolTip(tr("Delete the saved transition for these two tracks"));
    m_pStatus = new QLabel(this);
    m_pStatus->setObjectName(QStringLiteral("MixTransitionStatus"));

    auto* pTop = new QHBoxLayout();
    pTop->setContentsMargins(0, 0, 0, 0);
    pTop->setSpacing(6);
    pTop->addWidget(pTitle);
    pTop->addWidget(m_pTracks, 1);
    pTop->addWidget(m_pSwap);
    pTop->addSpacing(8);
    pTop->addLayout(pLaneRow);
    pTop->addSpacing(8);
    pTop->addWidget(m_pTempoMode);
    pTop->addWidget(m_pCustomBpm);
    pTop->addWidget(m_pRamp);
    pTop->addWidget(m_pSnap);
    pTop->addSpacing(8);
    pTop->addWidget(m_pStatus);
    pTop->addWidget(m_pReset);
    pTop->addWidget(m_pArm);

    m_pView = new MixWaveformView(this);

    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(6, 4, 6, 4);
    pLayout->setSpacing(4);
    pLayout->addLayout(pTop);
    pLayout->addWidget(m_pView, 1);

    for (int i = 0; i < 2; ++i) {
        m_pBpm[i] = std::make_unique<ControlProxy>(kDeckGroups[i],
                QStringLiteral("bpm"),
                nullptr,
                ControlFlag::AllowMissingOrInvalid);
    }

    // --- wiring ---
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(kSaveDelayMs);
    connect(&m_saveTimer, &QTimer::timeout, this, &WMixTransitionPanel::slotSave);
    connect(m_pView, &MixWaveformView::transitionEdited, this, &WMixTransitionPanel::slotEdited);
    connect(m_pSwap, &QPushButton::clicked, this, &WMixTransitionPanel::slotSwap);
    connect(m_pReset, &QPushButton::clicked, this, &WMixTransitionPanel::slotReset);
    connect(m_pLaneButtons, &QButtonGroup::idClicked, this, [this](int id) {
        m_pView->setLaneGroup(static_cast<MixWaveformView::LaneGroup>(id));
    });
    auto onControl = [this]() {
        if (m_updating) {
            return;
        }
        MixTransition t = currentTransition();
        m_pView->setTransition(t);
        slotEdited();
    };
    connect(m_pTempoMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, onControl);
    connect(m_pCustomBpm, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onControl);
    connect(m_pRamp, &QCheckBox::toggled, this, onControl);
    connect(m_pSnap, &QCheckBox::toggled, m_pView, &MixWaveformView::setSnapToBeats);
    m_pArm->setChecked(m_pStore->isEnabled());
    connect(m_pArm, &QPushButton::toggled, this, [this](bool on) {
        m_pStore->setEnabled(on);
        updateInfo();
    });
    connect(m_pStore, &MixTransitionStore::enabledChanged, this, [this](bool on) {
        if (m_pArm->isChecked() != on) {
            m_pArm->setChecked(on);
        }
    });
    connect(&PlayerInfo::instance(),
            &PlayerInfo::trackChanged,
            this,
            &WMixTransitionPanel::slotTrackChanged);
    if (m_pPlayer) {
        connect(m_pPlayer,
                &MixTransitionPlayer::progressChanged,
                this,
                &WMixTransitionPanel::slotProgress);
        connect(m_pPlayer,
                &MixTransitionPlayer::runningChanged,
                this,
                &WMixTransitionPanel::slotRunningChanged);
    }

    reload(true);
}

WMixTransitionPanel::~WMixTransitionPanel() {
    if (m_saveTimer.isActive()) {
        slotSave();
    }
}

TrackPointer WMixTransitionPanel::track(int deck) const {
    return PlayerInfo::instance().getTrackInfo(kDeckGroups[deck]);
}

double WMixTransitionPanel::deckBpm(int deck) const {
    const double bpm = m_pBpm[deck]->valid() ? m_pBpm[deck]->get() : 0.0;
    if (bpm > 0) {
        return bpm;
    }
    const TrackPointer pTrack = track(deck);
    return pTrack ? pTrack->getBpm() : 0.0;
}

double WMixTransitionPanel::targetBpm(const MixTransition& t) const {
    const double start = deckBpm(m_fromDeck);
    switch (t.tempoMode) {
    case MixTransition::TempoMode::Off:
        return 0.0;
    case MixTransition::TempoMode::KeepA:
        return start;
    case MixTransition::TempoMode::UseB:
        return m_pToTrack && m_pToTrack->getBpm() > 0 ? m_pToTrack->getBpm() : start;
    case MixTransition::TempoMode::Custom:
        return t.customBpm > 0 ? t.customBpm : start;
    }
    return start;
}

void WMixTransitionPanel::slotTrackChanged(const QString& group,
        TrackPointer /*pNewTrack*/,
        TrackPointer /*pOldTrack*/) {
    if (group != kDeckGroups[0] && group != kDeckGroups[1]) {
        return;
    }
    if (m_pPlayer && m_pPlayer->isRunning()) {
        return; // don't swap the editor under a running transition
    }
    if (m_saveTimer.isActive()) {
        slotSave(); // keep pending edits of the previous pair
    }
    m_manualDirection = false;
    reload(true);
}

void WMixTransitionPanel::reload(bool reframe) {
    if (!m_manualDirection) {
        // Transition out of the deck that is playing; deck 1 -> 2 otherwise.
        ControlProxy play2(kDeckGroups[1], QStringLiteral("play"), nullptr,
                ControlFlag::AllowMissingOrInvalid);
        ControlProxy play1(kDeckGroups[0], QStringLiteral("play"), nullptr,
                ControlFlag::AllowMissingOrInvalid);
        m_fromDeck = (play2.get() > 0 && play1.get() <= 0) ? 1 : 0;
    }
    m_pFromTrack = track(m_fromDeck);
    m_pToTrack = track(1 - m_fromDeck);
    m_pView->setTracks(m_pFromTrack, m_pToTrack);

    if (!m_pFromTrack || !m_pToTrack) {
        m_pView->setPlaceholderText(
                tr("Load tracks on deck 1 and deck 2 to set up the transition between them."));
        m_saved = false;
        updateInfo();
        return;
    }
    m_pView->setPlaceholderText(QString());
    const std::optional<MixTransition> saved =
            m_pStore->get(m_pFromTrack->getId(), m_pToTrack->getId());
    m_saved = saved.has_value();
    m_pView->setTransition(saved ? *saved : suggestTransition());
    if (reframe) {
        m_pView->frameTransition();
    }
    updateControls();
}

void WMixTransitionPanel::updateControls() {
    const MixTransition& t = m_pView->transition();
    m_updating = true;
    const int modeIndex = m_pTempoMode->findData(static_cast<int>(t.tempoMode));
    m_pTempoMode->setCurrentIndex(modeIndex >= 0 ? modeIndex : 1);
    m_pCustomBpm->setValue(t.customBpm > 0 ? t.customBpm : std::max(40.0, deckBpm(m_fromDeck)));
    m_pRamp->setChecked(t.tempoRamp);
    m_updating = false;
    updateInfo();
}

MixTransition WMixTransitionPanel::currentTransition() const {
    MixTransition t = m_pView->transition();
    t.tempoMode = static_cast<MixTransition::TempoMode>(m_pTempoMode->currentData().toInt());
    t.customBpm = t.tempoMode == MixTransition::TempoMode::Custom ? m_pCustomBpm->value() : 0.0;
    // A tempo line equal to the old default follows the Ramp switch.
    const bool wasActive = t.isLaneActive(MixLane::Tempo);
    const QVector<MixPoint> tempo = t.lane(MixLane::Tempo);
    t.tempoRamp = m_pRamp->isChecked();
    if (wasActive) {
        t.setLane(MixLane::Tempo, tempo);
    } else {
        t.lanes.remove(static_cast<int>(MixLane::Tempo));
    }
    return t;
}

void WMixTransitionPanel::updateLaneButtons() {
    const MixTransition& t = m_pView->transition();
    for (int i = 0; i < static_cast<int>(std::size(kLaneButtons)); ++i) {
        const LaneButton& b = kLaneButtons[i];
        const bool active = t.isLaneActive(b.laneA) || t.isLaneActive(b.laneB);
        const bool showDot = active && b.group != MixWaveformView::LaneGroup::Volume;
        m_pLaneButtons->button(i)->setText(showDot
                        ? tr(b.label) + QStringLiteral(" ") + QString::fromUtf8(kDot)
                        : tr(b.label));
    }
}

void WMixTransitionPanel::updateInfo() {
    const bool ready = m_pFromTrack && m_pToTrack;
    for (QWidget* pWidget : std::initializer_list<QWidget*>{
                 m_pTempoMode, m_pRamp, m_pSnap, m_pReset, m_pSwap}) {
        pWidget->setEnabled(ready);
    }
    for (auto* pButton : m_pLaneButtons->buttons()) {
        pButton->setEnabled(ready);
    }
    const MixTransition t = m_pView->transition();
    const bool custom = t.tempoMode == MixTransition::TempoMode::Custom;
    m_pCustomBpm->setVisible(custom);
    m_pRamp->setEnabled(ready && t.tempoMode != MixTransition::TempoMode::Off &&
            t.tempoMode != MixTransition::TempoMode::KeepA);

    if (!ready) {
        m_pTracks->setText(QString());
        m_pStatus->setText(QString());
        m_pReset->setEnabled(false);
        return;
    }
    const QString fromLetter = QStringLiteral("A");
    const QString toLetter = QStringLiteral("B");
    m_pTracks->setToolTip(tr("A = deck %1 (mixing out), B = deck %2 (mixing in)")
                                  .arg(m_fromDeck + 1)
                                  .arg(2 - m_fromDeck));
    m_pTracks->setText(QStringLiteral(
            "<span style='color:#ff8a3d'>%1</span> %2 <span style='color:#6b6e78'>%3</span>"
            " &nbsp;%4&nbsp; "
            "<span style='color:#33c3f0'>%5</span> %6 <span style='color:#6b6e78'>%7</span>")
                               .arg(fromLetter,
                                       trackName(m_pFromTrack).toHtmlEscaped(),
                                       QString::number(deckBpm(m_fromDeck), 'f', 1),
                                       QString::fromUtf8(kArrow),
                                       toLetter,
                                       trackName(m_pToTrack).toHtmlEscaped(),
                                       QString::number(m_pToTrack->getBpm(), 'f', 1)));
    m_pView->setTempoRange(deckBpm(m_fromDeck), targetBpm(t));
    m_pReset->setEnabled(m_saved);
    m_pStatus->setText(m_saveTimer.isActive() ? tr("Saving...")
                    : m_saved                 ? tr("Saved")
                                              : tr("Suggested - edit to save"));
    updateLaneButtons();
}

void WMixTransitionPanel::slotEdited() {
    if (!m_pFromTrack || !m_pToTrack) {
        return;
    }
    m_saveTimer.start();
    updateInfo();
}

void WMixTransitionPanel::slotSave() {
    m_saveTimer.stop();
    if (!m_pFromTrack || !m_pToTrack) {
        return;
    }
    const MixTransition t = currentTransition();
    m_pView->setTransition(t);
    m_pStore->set(m_pFromTrack, m_pToTrack, t);
    m_saved = true;
    updateInfo();
}

void WMixTransitionPanel::slotSwap() {
    if (m_saveTimer.isActive()) {
        slotSave();
    }
    m_fromDeck = 1 - m_fromDeck;
    m_manualDirection = true;
    reload(true);
}

void WMixTransitionPanel::slotReset() {
    m_saveTimer.stop();
    if (m_pFromTrack && m_pToTrack) {
        m_pStore->remove(m_pFromTrack->getId(), m_pToTrack->getId());
    }
    reload(true);
}

void WMixTransitionPanel::slotProgress(int fromDeck, double progress) {
    if (fromDeck != m_fromDeck) {
        return;
    }
    m_pView->setPlayhead(progress * m_pView->transition().length);
}

void WMixTransitionPanel::slotRunningChanged(bool running) {
    if (!running) {
        m_pView->setPlayhead(std::numeric_limits<double>::quiet_NaN());
    }
    m_pStatus->setText(running ? tr("Mixing...") : (m_saved ? tr("Saved") : QString()));
}

MixTransition WMixTransitionPanel::suggestTransition() const {
    MixTransition t;
    if (!m_pFromTrack || !m_pToTrack) {
        return t;
    }
    const double durationA = m_pFromTrack->getDuration();
    const double bpmA = m_pFromTrack->getBpm();
    const double bpmB = m_pToTrack->getBpm();
    const double beat = bpmA > 0 ? 60.0 / bpmA : 0.5;

    const double outroStart = cueSeconds(m_pFromTrack, mixxx::CueType::Outro, false);
    const double outroEnd = cueSeconds(m_pFromTrack, mixxx::CueType::Outro, true);
    const double introStart = cueSeconds(m_pToTrack, mixxx::CueType::Intro, false);
    const double introEnd = cueSeconds(m_pToTrack, mixxx::CueType::Intro, true);
    double length = 16 * beat;
    if (outroStart >= 0 && outroEnd > outroStart) {
        length = outroEnd - outroStart;
    } else if (introStart >= 0 && introEnd > introStart) {
        length = introEnd - introStart;
    }
    t.length = std::clamp(length, 4.0, 32.0);
    if (outroStart >= 0 && outroStart < durationA) {
        t.fromTrigger = outroStart;
    } else {
        t.fromTrigger = snapToBeat(m_pFromTrack, std::max(0.0, durationA - t.length - 16 * beat));
    }
    if (introStart >= 0) {
        t.toStart = introStart;
    } else {
        const mixxx::BeatsPointer pBeats = m_pToTrack->getBeats();
        const double sampleRate = m_pToTrack->getSampleRate();
        t.toStart = (pBeats && sampleRate > 0 && pBeats->firstBeat().isValid())
                ? pBeats->firstBeat().value() / sampleRate
                : 0.0;
    }
    t.tempoMode = (bpmA > 0 && bpmB > 0) ? MixTransition::TempoMode::KeepA
                                         : MixTransition::TempoMode::Off;
    t.tempoRamp = true;
    return t;
}
