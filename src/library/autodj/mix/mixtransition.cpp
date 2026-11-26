#include "library/autodj/mix/mixtransition.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>
#include <cmath>

#include "control/controlproxy.h"
#include "moc_mixtransition.cpp"
#include "track/track.h"
#include "util/logger.h"

namespace {

const mixxx::Logger kLogger("MixTransition");

const QString kFileName = QStringLiteral("mix_transitions.json");
const ConfigKey kMixEnabledKey(QStringLiteral("[Auto DJ]"), QStringLiteral("MixMode"));

const char* const kLaneKeys[kMixLaneCount] = {
        "VolumeA",
        "VolumeB",
        "LowA",
        "MidA",
        "HighA",
        "LowB",
        "MidB",
        "HighB",
        "FilterA",
        "FilterB",
        "Tempo",
};

const QString kVolume = QStringLiteral("volume");
const QString kBpm = QStringLiteral("bpm");
const QString kPhaseSync = QStringLiteral("beatsync_phase");
const QString kFilterItem = QStringLiteral("super1");

QString quickEffectGroup(const QString& deckGroup) {
    return QStringLiteral("[QuickEffectRack1_%1]").arg(deckGroup);
}

QString eqGroup(const QString& deckGroup) {
    return QStringLiteral("[EqualizerRack1_%1_Effect1]").arg(deckGroup);
}

/// EQ line value -> gain factor relative to the user's knob:
/// 0 = kill, 0.5 = unchanged, 1 = x2 (+6 dB).
double eqFactor(double y) {
    y = std::clamp(y, 0.0, 1.0);
    return y <= 0.5 ? 2.0 * y : 1.0 + (y - 0.5) * 2.0;
}

bool sameLine(const QVector<MixPoint>& a, const QVector<MixPoint>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (int i = 0; i < a.size(); ++i) {
        if (std::fabs(a[i].x - b[i].x) > 1e-4 || std::fabs(a[i].y - b[i].y) > 1e-4) {
            return false;
        }
    }
    return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// MixTransition
// ---------------------------------------------------------------------------

QVector<MixPoint> MixTransition::defaultLane(MixLane lane) const {
    switch (lane) {
    case MixLane::VolumeA:
        return {{0.0, 1.0}, {1.0, 0.0}};
    case MixLane::VolumeB:
        return {{0.0, 0.0}, {1.0, 1.0}};
    case MixLane::Tempo:
        return tempoRamp ? QVector<MixPoint>{{0.0, 0.0}, {1.0, 1.0}}
                         : QVector<MixPoint>{{0.0, 0.0}, {1.0, 0.0}};
    default:
        return {{0.0, 0.5}, {1.0, 0.5}};
    }
}

QVector<MixPoint> MixTransition::lane(MixLane lane) const {
    const auto it = lanes.constFind(static_cast<int>(lane));
    if (it == lanes.constEnd() || it->size() < 2) {
        return defaultLane(lane);
    }
    return *it;
}

void MixTransition::setLane(MixLane lane, const QVector<MixPoint>& points) {
    QVector<MixPoint> sorted = points;
    std::sort(sorted.begin(), sorted.end(), [](const MixPoint& a, const MixPoint& b) {
        return a.x < b.x;
    });
    for (MixPoint& p : sorted) {
        p.x = std::clamp(p.x, 0.0, 1.0);
        p.y = std::clamp(p.y, 0.0, 1.0);
    }
    if (sorted.size() < 2 || sameLine(sorted, defaultLane(lane))) {
        lanes.remove(static_cast<int>(lane));
    } else {
        lanes.insert(static_cast<int>(lane), sorted);
    }
}

bool MixTransition::isLaneActive(MixLane lane) const {
    const auto it = lanes.constFind(static_cast<int>(lane));
    return it != lanes.constEnd() && it->size() >= 2 && !sameLine(*it, defaultLane(lane));
}

double MixTransition::evaluate(const QVector<MixPoint>& points, double x) {
    if (points.isEmpty()) {
        return 0.0;
    }
    if (x <= points.first().x) {
        return points.first().y;
    }
    for (int i = 1; i < points.size(); ++i) {
        const MixPoint& a = points[i - 1];
        const MixPoint& b = points[i];
        if (x <= b.x) {
            const double span = b.x - a.x;
            if (span <= 1e-9) {
                return b.y;
            }
            return a.y + (b.y - a.y) * (x - a.x) / span;
        }
    }
    return points.last().y;
}

QString MixTransition::laneName(MixLane lane) {
    return QString::fromLatin1(kLaneKeys[static_cast<int>(lane)]);
}

bool MixTransition::bpmsCompatible(double bpmA, double bpmB, double tolerance) {
    if (bpmA <= 0 || bpmB <= 0) {
        return false;
    }
    for (double factor : {1.0, 2.0, 0.5}) {
        if (std::fabs(bpmA - bpmB * factor) / bpmA <= tolerance) {
            return true;
        }
    }
    return false;
}

QJsonObject MixTransition::toJson() const {
    QJsonObject obj;
    obj.insert(QStringLiteral("version"), 2);
    obj.insert(QStringLiteral("fromTrigger"), fromTrigger);
    obj.insert(QStringLiteral("toStart"), toStart);
    obj.insert(QStringLiteral("length"), length);
    obj.insert(QStringLiteral("tempoMode"), static_cast<int>(tempoMode));
    obj.insert(QStringLiteral("customBpm"), customBpm);
    obj.insert(QStringLiteral("tempoRamp"), tempoRamp);
    QJsonObject laneObj;
    for (auto it = lanes.constBegin(); it != lanes.constEnd(); ++it) {
        if (it.key() < 0 || it.key() >= kMixLaneCount) {
            continue;
        }
        QJsonArray points;
        for (const MixPoint& p : it.value()) {
            points.append(QJsonArray{p.x, p.y});
        }
        laneObj.insert(QString::fromLatin1(kLaneKeys[it.key()]), points);
    }
    obj.insert(QStringLiteral("lanes"), laneObj);
    return obj;
}

std::optional<MixTransition> MixTransition::fromJson(const QJsonObject& obj) {
    if (!obj.contains(QStringLiteral("fromTrigger"))) {
        return std::nullopt;
    }
    MixTransition t;
    t.fromTrigger = std::max(0.0, obj.value(QStringLiteral("fromTrigger")).toDouble());
    t.toStart = std::max(0.0, obj.value(QStringLiteral("toStart")).toDouble());
    t.length = std::clamp(obj.value(QStringLiteral("length")).toDouble(8.0), 0.0, 120.0);

    if (obj.value(QStringLiteral("version")).toInt() >= 2) {
        const int mode = obj.value(QStringLiteral("tempoMode")).toInt(1);
        t.tempoMode = (mode >= 0 && mode <= 3) ? static_cast<TempoMode>(mode) : TempoMode::KeepA;
        t.customBpm = obj.value(QStringLiteral("customBpm")).toDouble();
        t.tempoRamp = obj.value(QStringLiteral("tempoRamp")).toBool(true);
        const QJsonObject laneObj = obj.value(QStringLiteral("lanes")).toObject();
        for (int i = 0; i < kMixLaneCount; ++i) {
            const QJsonArray arr = laneObj.value(QString::fromLatin1(kLaneKeys[i])).toArray();
            QVector<MixPoint> points;
            for (const QJsonValue& v : arr) {
                const QJsonArray p = v.toArray();
                if (p.size() == 2) {
                    points.append({p.at(0).toDouble(), p.at(1).toDouble()});
                }
            }
            if (points.size() >= 2) {
                t.setLane(static_cast<MixLane>(i), points);
            }
        }
    } else {
        // Version 1: style / curve / syncTempo.
        t.tempoMode = obj.value(QStringLiteral("syncTempo")).toBool(true)
                ? TempoMode::KeepA
                : TempoMode::Off;
        t.tempoRamp = false;
        const int style = obj.value(QStringLiteral("style")).toInt();
        if (style == 5) { // Cut
            t.length = 0.0;
        } else if (style == 6) { // Overlap: both at full level, then B only
            t.setLane(MixLane::VolumeA, {{0.0, 1.0}, {0.98, 1.0}, {1.0, 0.0}});
            t.setLane(MixLane::VolumeB, {{0.0, 1.0}, {1.0, 1.0}});
        }
    }
    return t;
}

// ---------------------------------------------------------------------------
// MixTransitionStore
// ---------------------------------------------------------------------------

MixTransitionStore::MixTransitionStore(UserSettingsPointer pConfig, QObject* pParent)
        : QObject(pParent),
          m_pConfig(pConfig),
          m_filePath(QDir(pConfig->getSettingsPath()).filePath(kFileName)) {
    load();
}

QString MixTransitionStore::key(TrackId fromId, TrackId toId) {
    return fromId.toString() + QStringLiteral(">") + toId.toString();
}

std::optional<MixTransition> MixTransitionStore::get(TrackId fromId, TrackId toId) const {
    if (!fromId.isValid() || !toId.isValid()) {
        return std::nullopt;
    }
    const auto it = m_entries.constFind(key(fromId, toId));
    if (it == m_entries.constEnd()) {
        return std::nullopt;
    }
    return MixTransition::fromJson(*it);
}

bool MixTransitionStore::contains(TrackId fromId, TrackId toId) const {
    return m_entries.contains(key(fromId, toId));
}

void MixTransitionStore::set(const TrackPointer& pFrom,
        const TrackPointer& pTo,
        const MixTransition& transition) {
    if (!pFrom || !pTo || !pFrom->getId().isValid() || !pTo->getId().isValid()) {
        return;
    }
    QJsonObject obj = transition.toJson();
    // Informational only; makes the file readable and helps recovery.
    obj.insert(QStringLiteral("from"), pFrom->getInfo());
    obj.insert(QStringLiteral("to"), pTo->getInfo());
    m_entries.insert(key(pFrom->getId(), pTo->getId()), obj);
    save();
    emit transitionsChanged();
}

void MixTransitionStore::remove(TrackId fromId, TrackId toId) {
    if (m_entries.remove(key(fromId, toId)) > 0) {
        save();
        emit transitionsChanged();
    }
}

bool MixTransitionStore::isEnabled() const {
    return m_pConfig->getValue(kMixEnabledKey, true);
}

void MixTransitionStore::setEnabled(bool enabled) {
    if (enabled == isEnabled()) {
        return;
    }
    m_pConfig->setValue(kMixEnabledKey, enabled);
    emit enabledChanged(enabled);
    emit transitionsChanged();
}

void MixTransitionStore::load() {
    QFile file(m_filePath);
    if (!file.exists()) {
        return;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        kLogger.warning() << "Cannot read" << m_filePath;
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonObject transitions = root.value(QStringLiteral("transitions")).toObject();
    for (auto it = transitions.constBegin(); it != transitions.constEnd(); ++it) {
        m_entries.insert(it.key(), it.value().toObject());
    }
    kLogger.info() << "Loaded" << m_entries.size() << "mix transitions";
}

void MixTransitionStore::save() const {
    QJsonObject transitions;
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        transitions.insert(it.key(), it.value());
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), 2);
    root.insert(QStringLiteral("transitions"), transitions);

    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        kLogger.warning() << "Cannot write" << m_filePath;
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        kLogger.warning() << "Failed to save" << m_filePath;
    }
}

// ---------------------------------------------------------------------------
// MixEffectDriver
// ---------------------------------------------------------------------------

MixEffectDriver::MixEffectDriver()
        : m_active(false),
          m_startBpm(0.0),
          m_targetBpm(0.0) {
}

MixEffectDriver::~MixEffectDriver() {
    finish();
}

ControlProxy* MixEffectDriver::control(const QString& group, const QString& item) {
    const QString id = group + item;
    auto it = m_controls.find(id);
    if (it == m_controls.end()) {
        auto pProxy = std::make_unique<ControlProxy>(
                group, item, nullptr, ControlFlag::AllowMissingOrInvalid);
        if (!pProxy->valid()) {
            return nullptr;
        }
        const double original = pProxy->get();
        it = m_controls.emplace(id, Saved{std::move(pProxy), original}).first;
    }
    return it->second.pControl.get();
}

void MixEffectDriver::set(const QString& group, const QString& item, double value) {
    if (ControlProxy* pControl = control(group, item)) {
        pControl->set(value);
    }
}

double MixEffectDriver::get(const QString& group, const QString& item) {
    ControlProxy* pControl = control(group, item);
    return pControl ? pControl->get() : 0.0;
}

double MixEffectDriver::original(const QString& group, const QString& item) {
    if (!control(group, item)) {
        return 1.0;
    }
    return m_controls.at(group + item).original;
}

void MixEffectDriver::begin(const QString& fromGroup,
        const QString& toGroup,
        const MixTransition& transition,
        double toTrackFileBpm) {
    finish();
    m_from = fromGroup;
    m_to = toGroup;
    m_transition = transition;
    m_active = true;

    m_startBpm = get(m_from, kBpm);
    switch (transition.tempoMode) {
    case MixTransition::TempoMode::Off:
        m_targetBpm = 0.0;
        break;
    case MixTransition::TempoMode::KeepA:
        m_targetBpm = m_startBpm;
        break;
    case MixTransition::TempoMode::UseB:
        m_targetBpm = toTrackFileBpm > 0 ? toTrackFileBpm : m_startBpm;
        break;
    case MixTransition::TempoMode::Custom:
        m_targetBpm = transition.customBpm > 0 ? transition.customBpm : m_startBpm;
        break;
    }
    if (transition.tempoMode != MixTransition::TempoMode::Off && m_startBpm > 0) {
        // The incoming deck joins at the outgoing deck's tempo, so the two are
        // beatmatched for the overlap.
        set(m_to, kBpm, m_startBpm);
    }
    kLogger.debug() << "Begin" << m_from << "->" << m_to << "length" << transition.length
                    << "bpm" << m_startBpm << "->" << m_targetBpm;
    apply(0.0);
}

void MixEffectDriver::started() {
    if (m_active && m_transition.tempoMode != MixTransition::TempoMode::Off) {
        set(m_to, kPhaseSync, 1.0);
        set(m_to, kPhaseSync, 0.0);
    }
}

void MixEffectDriver::apply(double progress) {
    if (!m_active) {
        return;
    }
    const double p = std::clamp(progress, 0.0, 1.0);
    const MixTransition& t = m_transition;

    // Fader levels, relative to where the user left the faders.
    set(m_from, kVolume, original(m_from, kVolume) * t.evaluate(t.lane(MixLane::VolumeA), p));
    set(m_to, kVolume, original(m_to, kVolume) * t.evaluate(t.lane(MixLane::VolumeB), p));

    // EQ and filter lines only touch the knobs when they were edited.
    static const struct {
        MixLane lane;
        bool fromDeck;
        const char* item;
    } kEqLanes[] = {
            {MixLane::LowA, true, "parameter1"},
            {MixLane::MidA, true, "parameter2"},
            {MixLane::HighA, true, "parameter3"},
            {MixLane::LowB, false, "parameter1"},
            {MixLane::MidB, false, "parameter2"},
            {MixLane::HighB, false, "parameter3"},
    };
    for (const auto& eq : kEqLanes) {
        if (!t.isLaneActive(eq.lane)) {
            continue;
        }
        const QString group = eqGroup(eq.fromDeck ? m_from : m_to);
        const QString item = QString::fromLatin1(eq.item);
        set(group, item, original(group, item) * eqFactor(t.evaluate(t.lane(eq.lane), p)));
    }
    if (t.isLaneActive(MixLane::FilterA)) {
        set(quickEffectGroup(m_from), kFilterItem, t.evaluate(t.lane(MixLane::FilterA), p));
    }
    if (t.isLaneActive(MixLane::FilterB)) {
        set(quickEffectGroup(m_to), kFilterItem, t.evaluate(t.lane(MixLane::FilterB), p));
    }

    // Tempo: both decks follow the same curve so they stay beatmatched.
    if (t.tempoMode != MixTransition::TempoMode::Off && m_startBpm > 0 && m_targetBpm > 0 &&
            std::fabs(m_targetBpm - m_startBpm) > 0.01) {
        const double bpm = m_startBpm +
                (m_targetBpm - m_startBpm) * t.evaluate(t.lane(MixLane::Tempo), p);
        set(m_from, kBpm, bpm);
        set(m_to, kBpm, bpm);
    }
}

void MixEffectDriver::finish() {
    if (!m_active) {
        m_controls.clear();
        return;
    }
    m_active = false;
    const MixTransition& t = m_transition;
    // Without a ramp the tempo switches once A is out.
    if (t.tempoMode != MixTransition::TempoMode::Off && m_targetBpm > 0 &&
            std::fabs(m_targetBpm - m_startBpm) > 0.01) {
        set(m_to, kBpm, m_targetBpm);
    }
    for (auto it = m_controls.begin(); it != m_controls.end(); ++it) {
        if (it->first.endsWith(kBpm) || it->first.endsWith(kPhaseSync)) {
            continue;
        }
        it->second.pControl->set(it->second.original);
    }
    m_controls.clear();
    kLogger.debug() << "Finished transition" << m_from << "->" << m_to;
}
