#include "library/autodj/mix/mixtransitioneditor.h"

#include <algorithm>
#include <cmath>

#include "control/controlproxy.h"
#include "control/controlpushbutton.h"
#include "mixer/playerinfo.h"
#include "moc_mixtransitioneditor.cpp"
#include "track/beats.h"
#include "track/cue.h"
#include "track/track.h"

namespace {

const QString kDeckGroups[2] = {QStringLiteral("[Channel1]"), QStringLiteral("[Channel2]")};
const QString kMixGroup = QStringLiteral("[Mix]");
constexpr int kSaveDelayMs = 400;
constexpr double kPreviewLeadSeconds = 6.0;

const QColor kColorA(0xff, 0x8a, 0x3d);
const QColor kColorB(0x33, 0xc3, 0xf0);
const QColor kColorTempo(0xe8, 0xe9, 0xed);

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

QColor laneColor(MixLane lane) {
    switch (lane) {
    case MixLane::VolumeB:
    case MixLane::LowB:
    case MixLane::MidB:
    case MixLane::HighB:
    case MixLane::FilterB:
        return kColorB;
    case MixLane::Tempo:
        return kColorTempo;
    default:
        return kColorA;
    }
}

} // anonymous namespace

MixTransitionEditor* MixTransitionEditor::s_pInstance = nullptr;

MixTransitionEditor* MixTransitionEditor::instance() {
    return s_pInstance;
}

MixTransitionEditor::MixTransitionEditor(UserSettingsPointer pConfig,
        MixTransitionStore* pStore,
        QObject* pParent)
        : QObject(pParent),
          m_pConfig(pConfig),
          m_pStore(pStore),
          m_fromDeck(0),
          m_manualDirection(false),
          m_saved(false),
          m_group(LaneGroup::Volume),
          m_snap(true),
          m_dragMode(DragMode::None),
          m_dragLane(MixLane::Count),
          m_dragIndex(-1) {
    s_pInstance = this;

    // Master show/hide for everything transition-related on screen (control
    // bar, waveform regions and lines). Remembered between sessions.
    m_pShow = std::make_unique<ControlPushButton>(
            ConfigKey(kMixGroup, QStringLiteral("show")), true, 1.0);
    m_pShow->setButtonMode(ControlPushButton::TOGGLE);
    connect(m_pShow.get(), &ControlObject::valueChanged, this, [this](double v) {
        if (v <= 0 && m_pEditMode && m_pEditMode->get() > 0) {
            m_pEditMode->set(0.0);
        }
        emit changed();
    });
    m_pEditMode = std::make_unique<ControlPushButton>(
            ConfigKey(kMixGroup, QStringLiteral("edit_mode")));
    m_pEditMode->setButtonMode(ControlPushButton::TOGGLE);
    connect(m_pEditMode.get(), &ControlObject::valueChanged, this, [this](double) {
        emit changed();
    });
    m_pPreview = std::make_unique<ControlPushButton>(
            ConfigKey(kMixGroup, QStringLiteral("preview")));
    connect(m_pPreview.get(), &ControlObject::valueChanged, this, [this](double v) {
        if (v > 0) {
            preview();
        }
    });
    for (int i = 0; i < 2; ++i) {
        m_pMixPoint[i] = std::make_unique<ControlPushButton>(
                ConfigKey(kDeckGroups[i], QStringLiteral("mix_point")));
        connect(m_pMixPoint[i].get(), &ControlObject::valueChanged, this, [this, i](double v) {
            if (v <= 0) {
                return;
            }
            if (i == m_fromDeck) {
                setMixOutHere();
            } else {
                setMixInHere();
            }
        });
        const auto flags = ControlFlag::AllowMissingOrInvalid;
        m_pPlay[i] = std::make_unique<ControlProxy>(
                kDeckGroups[i], QStringLiteral("play"), nullptr, flags);
        m_pPlayPos[i] = std::make_unique<ControlProxy>(
                kDeckGroups[i], QStringLiteral("playposition"), nullptr, flags);
        m_pDuration[i] = std::make_unique<ControlProxy>(
                kDeckGroups[i], QStringLiteral("duration"), nullptr, flags);
        m_pBpm[i] = std::make_unique<ControlProxy>(
                kDeckGroups[i], QStringLiteral("bpm"), nullptr, flags);
    }
    m_pAutoDJ = std::make_unique<ControlProxy>(QStringLiteral("[AutoDJ]"),
            QStringLiteral("enabled"),
            nullptr,
            ControlFlag::AllowMissingOrInvalid);

    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(kSaveDelayMs);
    connect(&m_saveTimer, &QTimer::timeout, this, &MixTransitionEditor::save);
    connect(&PlayerInfo::instance(),
            &PlayerInfo::trackChanged,
            this,
            &MixTransitionEditor::slotTrackChanged);
    reload();
}

MixTransitionEditor::~MixTransitionEditor() {
    if (m_saveTimer.isActive()) {
        save();
    }
    if (s_pInstance == this) {
        s_pInstance = nullptr;
    }
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

bool MixTransitionEditor::editMode() const {
    return isVisible() && m_pEditMode && m_pEditMode->get() > 0;
}

bool MixTransitionEditor::isVisible() const {
    return m_pShow && m_pShow->get() > 0;
}

void MixTransitionEditor::setVisible(bool visible) {
    m_pShow->set(visible ? 1.0 : 0.0);
}

void MixTransitionEditor::setEditMode(bool on) {
    m_pEditMode->set(on ? 1.0 : 0.0);
    emit changed();
}

void MixTransitionEditor::setSnap(bool on) {
    m_snap = on;
}

void MixTransitionEditor::setLaneGroup(LaneGroup group) {
    m_group = group;
    emit changed();
}

double MixTransitionEditor::deckBpm(int deck) const {
    const double bpm = m_pBpm[deck]->valid() ? m_pBpm[deck]->get() : 0.0;
    if (bpm > 0) {
        return bpm;
    }
    const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(kDeckGroups[deck]);
    return pTrack ? pTrack->getBpm() : 0.0;
}

double MixTransitionEditor::targetBpm() const {
    const double start = startBpm();
    switch (m_transition.tempoMode) {
    case MixTransition::TempoMode::Off:
        return 0.0;
    case MixTransition::TempoMode::KeepA:
        return start;
    case MixTransition::TempoMode::UseB:
        return m_pTo && m_pTo->getBpm() > 0 ? m_pTo->getBpm() : start;
    case MixTransition::TempoMode::Custom:
        return m_transition.customBpm > 0 ? m_transition.customBpm : start;
    }
    return start;
}

void MixTransitionEditor::setTransition(const MixTransition& transition) {
    m_transition = transition;
    edited();
}

void MixTransitionEditor::edited() {
    if (hasPair()) {
        m_saveTimer.start();
    }
    emit changed();
}

void MixTransitionEditor::save() {
    m_saveTimer.stop();
    if (!hasPair()) {
        return;
    }
    m_pStore->set(m_pFrom, m_pTo, m_transition);
    m_saved = true;
    emit changed();
}

double MixTransitionEditor::lengthBeats() const {
    const double bpm = m_pFrom ? m_pFrom->getBpm() : 0.0;
    return bpm > 0 ? m_transition.length * bpm / 60.0 : 0.0;
}

void MixTransitionEditor::setLengthBeats(double beats) {
    const double bpm = m_pFrom ? m_pFrom->getBpm() : 0.0;
    if (bpm <= 0 || beats < 0) {
        return;
    }
    m_transition.length = std::min(120.0, beats * 60.0 / bpm);
    edited();
}

double MixTransitionEditor::snapToBeat(const TrackPointer& pTrack, double seconds) const {
    if (!m_snap || !pTrack) {
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

void MixTransitionEditor::setMixOutHere() {
    if (!hasPair()) {
        return;
    }
    const int deck = m_fromDeck;
    const double seconds = m_pPlayPos[deck]->get() * m_pDuration[deck]->get();
    m_transition.fromTrigger = std::max(0.0, snapToBeat(m_pFrom, seconds));
    edited();
}

void MixTransitionEditor::setMixInHere() {
    if (!hasPair()) {
        return;
    }
    const int deck = 1 - m_fromDeck;
    const double seconds = m_pPlayPos[deck]->get() * m_pDuration[deck]->get();
    m_transition.toStart = std::max(0.0, snapToBeat(m_pTo, seconds));
    edited();
}

void MixTransitionEditor::swap() {
    if (m_saveTimer.isActive()) {
        save();
    }
    m_fromDeck = 1 - m_fromDeck;
    m_manualDirection = true;
    reload();
}

void MixTransitionEditor::reset() {
    m_saveTimer.stop();
    if (hasPair()) {
        m_pStore->remove(m_pFrom->getId(), m_pTo->getId());
    }
    reload();
}

void MixTransitionEditor::preview() {
    if (!hasPair() || (m_pAutoDJ->valid() && m_pAutoDJ->get() > 0)) {
        return;
    }
    if (m_saveTimer.isActive()) {
        save();
    }
    if (!m_saved) {
        save(); // the player only fires saved transitions
    }
    const int from = m_fromDeck;
    const int to = 1 - m_fromDeck;
    m_pPlay[to]->set(0.0);
    m_pPlay[from]->set(0.0);
    const double durationTo = m_pDuration[to]->get();
    if (durationTo > 0) {
        m_pPlayPos[to]->set(m_transition.toStart / durationTo);
    }
    const double durationFrom = m_pDuration[from]->get();
    if (durationFrom > 0) {
        m_pPlayPos[from]->set(
                std::max(0.0, m_transition.fromTrigger - kPreviewLeadSeconds) / durationFrom);
    }
    m_pPlay[from]->set(1.0);
}

void MixTransitionEditor::slotTrackChanged(const QString& group, TrackPointer, TrackPointer) {
    if (group != kDeckGroups[0] && group != kDeckGroups[1]) {
        return;
    }
    if (m_saveTimer.isActive()) {
        save();
    }
    m_manualDirection = false;
    reload();
}

void MixTransitionEditor::reload() {
    if (!m_manualDirection) {
        // Mix out of the deck that is playing; deck 1 -> 2 otherwise.
        m_fromDeck = (m_pPlay[1]->get() > 0 && m_pPlay[0]->get() <= 0) ? 1 : 0;
    }
    m_pFrom = PlayerInfo::instance().getTrackInfo(kDeckGroups[m_fromDeck]);
    m_pTo = PlayerInfo::instance().getTrackInfo(kDeckGroups[1 - m_fromDeck]);
    if (hasPair()) {
        const std::optional<MixTransition> saved =
                m_pStore->get(m_pFrom->getId(), m_pTo->getId());
        m_saved = saved.has_value();
        m_transition = saved ? *saved : suggest();
    } else {
        m_saved = false;
        m_transition = MixTransition();
    }
    emit changed();
}

MixTransition MixTransitionEditor::suggest() const {
    MixTransition t;
    const double durationA = m_pFrom->getDuration();
    const double bpmA = m_pFrom->getBpm();
    const double bpmB = m_pTo->getBpm();
    const double beat = bpmA > 0 ? 60.0 / bpmA : 0.5;

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
    t.length = std::clamp(length, 4.0, 32.0);
    if (outroStart >= 0 && outroStart < durationA) {
        t.fromTrigger = outroStart;
    } else {
        t.fromTrigger = snapToBeat(m_pFrom, std::max(0.0, durationA - t.length - 16 * beat));
    }
    if (introStart >= 0) {
        t.toStart = introStart;
    } else {
        const mixxx::BeatsPointer pBeats = m_pTo->getBeats();
        const double sampleRate = m_pTo->getSampleRate();
        t.toStart = (pBeats && sampleRate > 0 && pBeats->firstBeat().isValid())
                ? pBeats->firstBeat().value() / sampleRate
                : 0.0;
    }
    t.tempoMode = (bpmA > 0 && bpmB > 0) ? MixTransition::TempoMode::KeepA
                                         : MixTransition::TempoMode::Off;
    return t;
}

// ---------------------------------------------------------------------------
// Overlay & mouse editing
// ---------------------------------------------------------------------------

int MixTransitionEditor::deckIndex(const QString& group) const {
    if (group == kDeckGroups[0]) {
        return 0;
    }
    if (group == kDeckGroups[1]) {
        return 1;
    }
    return -1;
}

MixTransitionEditor::Role MixTransitionEditor::role(const QString& group) const {
    const int deck = deckIndex(group);
    if (deck < 0 || !hasPair()) {
        return Role::None;
    }
    return deck == m_fromDeck ? Role::From : Role::To;
}

double MixTransitionEditor::regionStart(Role r) const {
    return r == Role::From ? m_transition.fromTrigger : m_transition.toStart;
}

QVector<MixLane> MixTransitionEditor::lanesForDeck(Role r) const {
    if (r == Role::From) {
        return {MixLane::VolumeA,
                MixLane::LowA,
                MixLane::MidA,
                MixLane::HighA,
                MixLane::FilterA,
                MixLane::Tempo};
    }
    if (r == Role::To) {
        return {MixLane::VolumeB, MixLane::LowB, MixLane::MidB, MixLane::HighB, MixLane::FilterB};
    }
    return {};
}

MixLane MixTransitionEditor::editableLane(Role r) const {
    const bool a = r == Role::From;
    switch (m_group) {
    case LaneGroup::Volume:
        return a ? MixLane::VolumeA : MixLane::VolumeB;
    case LaneGroup::Bass:
        return a ? MixLane::LowA : MixLane::LowB;
    case LaneGroup::Mid:
        return a ? MixLane::MidA : MixLane::MidB;
    case LaneGroup::Treble:
        return a ? MixLane::HighA : MixLane::HighB;
    case LaneGroup::Filter:
        return a ? MixLane::FilterA : MixLane::FilterB;
    case LaneGroup::Tempo:
        return a ? MixLane::Tempo : MixLane::Count;
    }
    return MixLane::Count;
}

MixTransitionEditor::Overlay MixTransitionEditor::overlay(const QString& group) const {
    Overlay o;
    o.role = isVisible() ? role(group) : Role::None;
    if (o.role == Role::None) {
        return o;
    }
    o.regionStart = regionStart(o.role);
    o.regionEnd = o.regionStart + std::max(0.0, m_transition.length);
    o.color = o.role == Role::From ? kColorA : kColorB;
    o.editMode = editMode();
    const MixLane editable = editableLane(o.role);
    for (MixLane lane : lanesForDeck(o.role)) {
        const bool isEditable = o.editMode && lane == editable;
        // Volume lines always show; the others when edited or being edited.
        const bool isVolume = lane == MixLane::VolumeA || lane == MixLane::VolumeB;
        if (!isEditable && !isVolume && !m_transition.isLaneActive(lane)) {
            continue;
        }
        Overlay::Line line;
        line.points = m_transition.lane(lane);
        line.color = laneColor(lane);
        if (lane == MixLane::Tempo) {
            line.color = kColorTempo;
        }
        line.editable = isEditable;
        if (!isEditable) {
            line.color.setAlpha(o.editMode ? 90 : 150);
        }
        o.lines.append(line);
    }
    // Draw the editable line last, on top.
    std::stable_partition(o.lines.begin(), o.lines.end(), [](const Overlay::Line& l) {
        return !l.editable;
    });
    return o;
}

bool MixTransitionEditor::beginDrag(const QString& group,
        double seconds,
        double value,
        double secondsTolerance,
        double valueTolerance) {
    m_dragMode = DragMode::None;
    const Role r = role(group);
    if (r == Role::None || !editMode()) {
        return false;
    }
    const double start = regionStart(r);
    const double length = std::max(0.0, m_transition.length);
    const double end = start + length;

    const MixLane lane = editableLane(r);
    if (lane != MixLane::Count && length > 0) {
        // An existing point?
        const QVector<MixPoint> points = m_transition.lane(lane);
        for (int i = 0; i < points.size(); ++i) {
            const double px = start + points[i].x * length;
            if (std::fabs(px - seconds) <= secondsTolerance &&
                    std::fabs(points[i].y - value) <= valueTolerance) {
                m_dragMode = DragMode::Point;
                m_dragLane = lane;
                m_dragIndex = i;
                return true;
            }
        }
    }
    // The region edges.
    if (std::fabs(seconds - start) <= secondsTolerance) {
        m_dragMode = DragMode::RegionStart;
        return true;
    }
    if (std::fabs(seconds - end) <= secondsTolerance) {
        m_dragMode = DragMode::RegionEnd;
        return true;
    }
    // Inside the region: add a point on the editable line.
    if (lane != MixLane::Count && length > 0 && seconds > start && seconds < end) {
        QVector<MixPoint> points = m_transition.lane(lane);
        const MixPoint point{(seconds - start) / length, std::clamp(value, 0.0, 1.0)};
        int index = 1;
        while (index < points.size() - 1 && points[index].x < point.x) {
            ++index;
        }
        points.insert(index, point);
        m_transition.lanes.insert(static_cast<int>(lane), points);
        m_dragMode = DragMode::Point;
        m_dragLane = lane;
        m_dragIndex = index;
        edited();
        return true;
    }
    return false;
}

void MixTransitionEditor::drag(const QString& group, double seconds, double value) {
    const Role r = role(group);
    if (r == Role::None || m_dragMode == DragMode::None) {
        return;
    }
    const TrackPointer pTrack = r == Role::From ? m_pFrom : m_pTo;
    const double duration = pTrack ? pTrack->getDuration() : 1e9;
    switch (m_dragMode) {
    case DragMode::RegionStart: {
        const double start = std::clamp(snapToBeat(pTrack, seconds), 0.0, duration);
        if (r == Role::From) {
            m_transition.fromTrigger = start;
        } else {
            m_transition.toStart = start;
        }
        break;
    }
    case DragMode::RegionEnd: {
        const double end = snapToBeat(pTrack, seconds);
        m_transition.length = std::clamp(end - regionStart(r), 0.0, 120.0);
        break;
    }
    case DragMode::Point: {
        const double length = std::max(1e-6, m_transition.length);
        QVector<MixPoint> points = m_transition.lane(m_dragLane);
        if (m_dragIndex < 0 || m_dragIndex >= points.size()) {
            return;
        }
        MixPoint p{(seconds - regionStart(r)) / length, std::clamp(value, 0.0, 1.0)};
        const bool neutralMiddle = m_dragLane != MixLane::VolumeA &&
                m_dragLane != MixLane::VolumeB && m_dragLane != MixLane::Tempo;
        if (neutralMiddle && std::fabs(p.y - 0.5) < 0.04) {
            p.y = 0.5;
        }
        const int last = points.size() - 1;
        if (m_dragIndex == 0) {
            p.x = 0.0;
        } else if (m_dragIndex == last) {
            p.x = 1.0;
        } else {
            p.x = std::clamp(p.x, points[m_dragIndex - 1].x, points[m_dragIndex + 1].x);
        }
        points[m_dragIndex] = p;
        m_transition.lanes.insert(static_cast<int>(m_dragLane), points);
        break;
    }
    case DragMode::None:
        return;
    }
    edited();
}

void MixTransitionEditor::endDrag() {
    if (m_dragMode == DragMode::Point) {
        // Normalizes; drops lines that went back to their default.
        m_transition.setLane(m_dragLane, m_transition.lane(m_dragLane));
        edited();
    }
    m_dragMode = DragMode::None;
}

bool MixTransitionEditor::removePoint(const QString& group,
        double seconds,
        double value,
        double secondsTolerance,
        double valueTolerance) {
    const Role r = role(group);
    if (r == Role::None || !editMode()) {
        return false;
    }
    const MixLane lane = editableLane(r);
    if (lane == MixLane::Count) {
        return false;
    }
    const double start = regionStart(r);
    const double length = std::max(1e-6, m_transition.length);
    QVector<MixPoint> points = m_transition.lane(lane);
    for (int i = 1; i < points.size() - 1; ++i) {
        const double px = start + points[i].x * length;
        if (std::fabs(px - seconds) <= secondsTolerance &&
                std::fabs(points[i].y - value) <= valueTolerance) {
            points.remove(i);
            m_transition.setLane(lane, points);
            edited();
            return true;
        }
    }
    return false;
}
