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
#include <iterator>

#include "library/autodj/mix/mixtransitioneditor.h"
#include "library/autodj/mix/mixtransitionplayer.h"
#include "moc_wmixtransitionpanel.cpp"
#include "track/track.h"

namespace {

// Written as UTF-8 escapes: the sources aren't compiled as UTF-8 on MSVC.
const char* const kArrow = "\xE2\x86\x92"; // ->
const char* const kSwap = "\xE2\x87\x84";  // <->
const char* const kDot = "\xE2\x80\xA2";   // bullet

struct LaneButton {
    MixTransitionEditor::LaneGroup group;
    const char* label;
    MixLane laneA;
    MixLane laneB;
};
const LaneButton kLaneButtons[] = {
        {MixTransitionEditor::LaneGroup::Volume, "Volume", MixLane::VolumeA, MixLane::VolumeB},
        {MixTransitionEditor::LaneGroup::Bass, "Bass", MixLane::LowA, MixLane::LowB},
        {MixTransitionEditor::LaneGroup::Mid, "Mid", MixLane::MidA, MixLane::MidB},
        {MixTransitionEditor::LaneGroup::Treble, "Treble", MixLane::HighA, MixLane::HighB},
        {MixTransitionEditor::LaneGroup::Filter, "Filter", MixLane::FilterA, MixLane::FilterB},
        {MixTransitionEditor::LaneGroup::Tempo, "Tempo", MixLane::Tempo, MixLane::Tempo},
};

QString trackName(const TrackPointer& pTrack) {
    if (!pTrack) {
        return QString();
    }
    QString title = pTrack->getTitle();
    if (title.isEmpty()) {
        title = pTrack->getInfo();
    }
    constexpr int kMaxChars = 26;
    if (title.size() > kMaxChars) {
        title = title.left(kMaxChars - 3).trimmed() + QStringLiteral("...");
    }
    return title;
}

} // anonymous namespace

WMixTransitionPanel::WMixTransitionPanel(QWidget* pParent,
        MixTransitionStore* pStore,
        MixTransitionEditor* pEditor,
        MixTransitionPlayer* pPlayer)
        : QWidget(pParent),
          m_pStore(pStore),
          m_pEditor(pEditor),
          m_pPlayer(pPlayer),
          m_updating(false) {
    setObjectName(QStringLiteral("MixTransitionPanel"));

    // --- row 1: what, direction, edit mode, which line, status, arm ---
    auto* pTitle = new QLabel(tr("Transition"), this);
    pTitle->setObjectName(QStringLiteral("MixTransitionTitle"));
    m_pTracks = new QLabel(this);
    m_pTracks->setTextFormat(Qt::RichText);
    m_pTracks->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_pSwap = new QPushButton(QString::fromUtf8(kSwap), this);
    m_pSwap->setFixedWidth(34);
    m_pSwap->setToolTip(tr("Swap direction (mix from the other deck)"));
    m_pEdit = new QPushButton(tr("Edit on waveform"), this);
    m_pEdit->setCheckable(true);
    m_pEdit->setToolTip(
            tr("Edit the transition on the main waveforms:\n"
               "- drag the edges of the shaded region to move the mix point / length\n"
               "- click inside it to add a point to the selected line, drag to move\n"
               "- right-click a point to delete it\n"
               "Outside the regions the waveform works as usual."));
    m_pLaneButtons = new QButtonGroup(this);
    auto* pLanes = new QHBoxLayout();
    pLanes->setSpacing(2);
    for (int i = 0; i < static_cast<int>(std::size(kLaneButtons)); ++i) {
        auto* pButton = new QPushButton(tr(kLaneButtons[i].label), this);
        pButton->setCheckable(true);
        pButton->setProperty("segment", true);
        m_pLaneButtons->addButton(pButton, i);
        pLanes->addWidget(pButton);
    }
    m_pLaneButtons->button(0)->setChecked(true);
    m_pSnap = new QCheckBox(tr("Snap"), this);
    m_pSnap->setChecked(true);
    m_pSnap->setToolTip(tr("Snap mix points and region edges to beats"));
    m_pStatus = new QLabel(this);
    m_pStatus->setObjectName(QStringLiteral("MixTransitionStatus"));
    m_pReset = new QPushButton(tr("Reset"), this);
    m_pReset->setToolTip(tr("Delete the saved transition for these two tracks"));
    m_pArm = new QPushButton(tr("Armed"), this);
    m_pArm->setCheckable(true);
    m_pArm->setToolTip(tr("Fire saved transitions automatically when the playing deck\n"
                          "reaches its mix-out point (by hand and in Auto DJ)."));

    auto* pRow1 = new QHBoxLayout();
    pRow1->setSpacing(6);
    pRow1->addWidget(pTitle);
    pRow1->addWidget(m_pTracks, 1);
    pRow1->addWidget(m_pSwap);
    pRow1->addSpacing(6);
    pRow1->addWidget(m_pEdit);
    pRow1->addLayout(pLanes);
    pRow1->addWidget(m_pSnap);
    pRow1->addSpacing(6);
    pRow1->addWidget(m_pStatus);
    pRow1->addWidget(m_pReset);
    pRow1->addWidget(m_pArm);

    // --- row 2: mix points from the decks, length, tempo, preview ---
    m_pMixOut = new QPushButton(tr("A out here"), this);
    m_pMixOut->setToolTip(tr("Set deck A's mix-out point to where deck A is now"));
    m_pMixIn = new QPushButton(tr("B in here"), this);
    m_pMixIn->setToolTip(tr("Set deck B's mix-in point to where deck B is now"));
    auto* pLengthLabel = new QLabel(tr("Length"), this);
    m_pLength = new QDoubleSpinBox(this);
    m_pLength->setRange(0.0, 256.0);
    m_pLength->setDecimals(0);
    m_pLength->setSingleStep(4.0);
    m_pLength->setSuffix(tr(" beats"));
    m_pLength->setKeyboardTracking(false);
    m_pLength->setToolTip(tr("Overlap length in beats of deck A (0 = cut)"));
    m_pTempoMode = new QComboBox(this);
    m_pTempoMode->addItem(tr("Tempo: leave"), static_cast<int>(MixTransition::TempoMode::Off));
    m_pTempoMode->addItem(tr("Tempo: keep A"), static_cast<int>(MixTransition::TempoMode::KeepA));
    m_pTempoMode->addItem(tr("Tempo: go to B"), static_cast<int>(MixTransition::TempoMode::UseB));
    m_pTempoMode->addItem(tr("Tempo: custom"), static_cast<int>(MixTransition::TempoMode::Custom));
    m_pTempoMode->setToolTip(
            tr("B joins at A's tempo so both are beatmatched. 'Go to B' / 'custom'\n"
               "then move both decks to the target along the Ramp or Tempo line."));
    m_pCustomBpm = new QDoubleSpinBox(this);
    m_pCustomBpm->setRange(40.0, 250.0);
    m_pCustomBpm->setDecimals(1);
    m_pCustomBpm->setSuffix(tr(" BPM"));
    m_pCustomBpm->setKeyboardTracking(false);
    m_pRamp = new QCheckBox(tr("Ramp"), this);
    m_pRamp->setToolTip(tr("Glide the tempo across the transition instead of\n"
                           "switching once A is out"));
    m_pPreview = new QPushButton(tr("Preview"), this);
    m_pPreview->setProperty("primary", true);
    m_pPreview->setToolTip(tr("Cue B at its mix-in point and play A from a few seconds\n"
                              "before its mix-out point, so the transition plays."));
    m_pHint = new QLabel(this);
    m_pHint->setObjectName(QStringLiteral("MixTransitionStatus"));
    m_pHint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto* pRow2 = new QHBoxLayout();
    pRow2->setSpacing(6);
    pRow2->addWidget(m_pMixOut);
    pRow2->addWidget(m_pMixIn);
    pRow2->addSpacing(8);
    pRow2->addWidget(pLengthLabel);
    pRow2->addWidget(m_pLength);
    pRow2->addSpacing(8);
    pRow2->addWidget(m_pTempoMode);
    pRow2->addWidget(m_pCustomBpm);
    pRow2->addWidget(m_pRamp);
    pRow2->addSpacing(8);
    pRow2->addWidget(m_pPreview);
    pRow2->addWidget(m_pHint, 1);

    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(6, 4, 6, 4);
    pLayout->setSpacing(4);
    pLayout->addLayout(pRow1);
    pLayout->addLayout(pRow2);

    // --- wiring ---
    connect(m_pEditor, &MixTransitionEditor::changed, this, &WMixTransitionPanel::refresh);
    connect(m_pSwap, &QPushButton::clicked, m_pEditor, &MixTransitionEditor::swap);
    connect(m_pReset, &QPushButton::clicked, m_pEditor, &MixTransitionEditor::reset);
    connect(m_pMixOut, &QPushButton::clicked, m_pEditor, &MixTransitionEditor::setMixOutHere);
    connect(m_pMixIn, &QPushButton::clicked, m_pEditor, &MixTransitionEditor::setMixInHere);
    connect(m_pPreview, &QPushButton::clicked, m_pEditor, &MixTransitionEditor::preview);
    connect(m_pEdit, &QPushButton::toggled, this, [this](bool on) {
        if (!m_updating) {
            m_pEditor->setEditMode(on);
        }
    });
    connect(m_pSnap, &QCheckBox::toggled, m_pEditor, &MixTransitionEditor::setSnap);
    connect(m_pLaneButtons, &QButtonGroup::idClicked, this, [this](int id) {
        m_pEditor->setLaneGroup(static_cast<MixTransitionEditor::LaneGroup>(id));
        if (!m_pEditor->editMode()) {
            m_pEditor->setEditMode(true); // picking a line means editing it
        }
    });
    connect(m_pLength, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        if (!m_updating) {
            m_pEditor->setLengthBeats(v);
        }
    });
    auto onTempo = [this]() {
        if (!m_updating) {
            applyTempoControls();
        }
    };
    connect(m_pTempoMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, onTempo);
    connect(m_pCustomBpm, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onTempo);
    connect(m_pRamp, &QCheckBox::toggled, this, onTempo);
    m_pArm->setChecked(m_pStore->isEnabled());
    connect(m_pArm, &QPushButton::toggled, m_pStore, &MixTransitionStore::setEnabled);
    connect(m_pStore, &MixTransitionStore::enabledChanged, this, [this](bool on) {
        if (m_pArm->isChecked() != on) {
            m_pArm->setChecked(on);
        }
    });
    if (m_pPlayer) {
        connect(m_pPlayer, &MixTransitionPlayer::runningChanged, this, &WMixTransitionPanel::refresh);
    }
    refresh();
}

void WMixTransitionPanel::applyTempoControls() {
    MixTransition t = m_pEditor->transition();
    t.tempoMode = static_cast<MixTransition::TempoMode>(m_pTempoMode->currentData().toInt());
    t.customBpm = t.tempoMode == MixTransition::TempoMode::Custom ? m_pCustomBpm->value() : 0.0;
    // An unedited tempo line follows the Ramp switch.
    const bool tempoEdited = t.isLaneActive(MixLane::Tempo);
    const QVector<MixPoint> tempo = t.lane(MixLane::Tempo);
    t.tempoRamp = m_pRamp->isChecked();
    if (tempoEdited) {
        t.setLane(MixLane::Tempo, tempo);
    } else {
        t.lanes.remove(static_cast<int>(MixLane::Tempo));
    }
    m_pEditor->setTransition(t);
}

void WMixTransitionPanel::refresh() {
    m_updating = true;
    const bool ready = m_pEditor->hasPair();
    const MixTransition& t = m_pEditor->transition();

    for (QWidget* pWidget : std::initializer_list<QWidget*>{m_pSwap,
                 m_pEdit,
                 m_pSnap,
                 m_pMixOut,
                 m_pMixIn,
                 m_pLength,
                 m_pTempoMode,
                 m_pCustomBpm,
                 m_pPreview}) {
        pWidget->setEnabled(ready);
    }
    for (auto* pButton : m_pLaneButtons->buttons()) {
        pButton->setEnabled(ready);
    }
    m_pEdit->setChecked(m_pEditor->editMode());
    m_pLaneButtons->button(static_cast<int>(m_pEditor->laneGroup()))->setChecked(true);

    if (!ready) {
        m_pTracks->setText(QStringLiteral("<span style='color:#6b6e78'>%1</span>")
                                   .arg(tr("Load tracks on deck 1 and deck 2")));
        m_pStatus->clear();
        m_pHint->clear();
        m_pReset->setEnabled(false);
        m_updating = false;
        return;
    }

    const int from = m_pEditor->fromDeck();
    m_pTracks->setText(QStringLiteral(
            "<span style='color:#ff8a3d'>A</span> %1 <span style='color:#6b6e78'>%2</span>"
            " &nbsp;%3&nbsp; "
            "<span style='color:#33c3f0'>B</span> %4 <span style='color:#6b6e78'>%5</span>")
                               .arg(trackName(m_pEditor->fromTrack()).toHtmlEscaped(),
                                       QString::number(m_pEditor->deckBpm(from), 'f', 1),
                                       QString::fromUtf8(kArrow),
                                       trackName(m_pEditor->toTrack()).toHtmlEscaped(),
                                       QString::number(m_pEditor->toTrack()->getBpm(), 'f', 1)));
    m_pTracks->setToolTip(tr("A = deck %1 (mixing out), B = deck %2 (mixing in)")
                                  .arg(from + 1)
                                  .arg(2 - from));
    m_pMixOut->setText(tr("A out here (deck %1)").arg(from + 1));
    m_pMixIn->setText(tr("B in here (deck %1)").arg(2 - from));

    m_pLength->setValue(std::round(m_pEditor->lengthBeats()));
    const int modeIndex = m_pTempoMode->findData(static_cast<int>(t.tempoMode));
    m_pTempoMode->setCurrentIndex(modeIndex >= 0 ? modeIndex : 1);
    m_pCustomBpm->setVisible(t.tempoMode == MixTransition::TempoMode::Custom);
    m_pCustomBpm->setValue(t.customBpm > 0 ? t.customBpm : std::max(40.0, m_pEditor->startBpm()));
    m_pRamp->setChecked(t.tempoRamp);
    m_pRamp->setEnabled(t.tempoMode == MixTransition::TempoMode::UseB ||
            t.tempoMode == MixTransition::TempoMode::Custom);

    for (int i = 0; i < static_cast<int>(std::size(kLaneButtons)); ++i) {
        const LaneButton& b = kLaneButtons[i];
        const bool active = t.isLaneActive(b.laneA) || t.isLaneActive(b.laneB);
        const bool showDot = active && b.group != MixTransitionEditor::LaneGroup::Volume;
        m_pLaneButtons->button(i)->setText(showDot
                        ? tr(b.label) + QStringLiteral(" ") + QString::fromUtf8(kDot)
                        : tr(b.label));
    }

    const bool running = m_pPlayer && m_pPlayer->isRunning();
    m_pReset->setEnabled(m_pEditor->isSaved());
    m_pStatus->setText(running                    ? tr("Mixing...")
                    : m_pEditor->isSaving()       ? tr("Saving...")
                    : m_pEditor->isSaved()        ? tr("Saved")
                                                  : tr("Suggested"));
    const int seconds = static_cast<int>(t.fromTrigger);
    m_pHint->setText(m_pEditor->editMode()
                    ? tr("Drag the shaded region's edges; click inside to add points, "
                         "right-click to delete.")
                    : tr("Mixes out of A at %1:%2.")
                              .arg(seconds / 60)
                              .arg(seconds % 60, 2, 10, QChar('0')));
    m_updating = false;
}
