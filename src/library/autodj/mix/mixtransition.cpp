#include "library/autodj/mix/mixtransition.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QtMath>

#include "control/controlproxy.h"
#include "moc_mixtransition.cpp"
#include "track/track.h"
#include "util/logger.h"

namespace {

const mixxx::Logger kLogger("MixTransition");

const QString kFileName = QStringLiteral("mix_transitions.json");
const ConfigKey kMixEnabledKey(QStringLiteral("[Auto DJ]"), QStringLiteral("MixMode"));

// Quick effect "Filter" super knob: 0.5 = neutral, <0.5 low-pass, >0.5 high-pass
constexpr double kFilterNeutral = 0.5;
constexpr double kFilterHighPassMax = 0.92;
constexpr double kFilterLowPassMax = 0.1;

const QString kFilterItem = QStringLiteral("super1");
const QString kLowEqItem = QStringLiteral("parameter1");

QString quickEffectGroup(const QString& deckGroup) {
    return QStringLiteral("[QuickEffectRack1_%1]").arg(deckGroup);
}

QString eqGroup(const QString& deckGroup) {
    return QStringLiteral("[EqualizerRack1_%1_Effect1]").arg(deckGroup);
}

double clamp01(double v) {
    return qBound(0.0, v, 1.0);
}

double smoothstep(double edge0, double edge1, double x) {
    const double t = clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0 - 2.0 * t);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// MixTransition
// ---------------------------------------------------------------------------

QJsonObject MixTransition::toJson() const {
    QJsonObject obj;
    obj.insert(QStringLiteral("fromTrigger"), fromTrigger);
    obj.insert(QStringLiteral("toStart"), toStart);
    obj.insert(QStringLiteral("length"), length);
    obj.insert(QStringLiteral("style"), static_cast<int>(style));
    obj.insert(QStringLiteral("curve"), static_cast<int>(curve));
    obj.insert(QStringLiteral("syncTempo"), syncTempo);
    return obj;
}

std::optional<MixTransition> MixTransition::fromJson(const QJsonObject& obj) {
    if (!obj.contains(QStringLiteral("fromTrigger"))) {
        return std::nullopt;
    }
    MixTransition t;
    t.fromTrigger = qMax(0.0, obj.value(QStringLiteral("fromTrigger")).toDouble());
    t.toStart = qMax(0.0, obj.value(QStringLiteral("toStart")).toDouble());
    t.length = qBound(0.0, obj.value(QStringLiteral("length")).toDouble(8.0), 120.0);
    const int style = obj.value(QStringLiteral("style")).toInt();
    t.style = (style >= 0 && style < kStyleCount) ? static_cast<Style>(style) : Style::Auto;
    const int curve = obj.value(QStringLiteral("curve")).toInt();
    t.curve = (curve >= 0 && curve < kCurveCount) ? static_cast<Curve>(curve) : Curve::Smooth;
    t.syncTempo = obj.value(QStringLiteral("syncTempo")).toBool(true);
    return t;
}

bool MixTransition::bpmsCompatible(double bpmA, double bpmB, double tolerance) {
    if (bpmA <= 0 || bpmB <= 0) {
        return false;
    }
    // Accept half / double time as well.
    for (double factor : {1.0, 2.0, 0.5}) {
        if (qAbs(bpmA - bpmB * factor) / bpmA <= tolerance) {
            return true;
        }
    }
    return false;
}

MixTransition::Style MixTransition::effectiveStyle(double fromBpm, double toBpm) const {
    if (style != Style::Auto) {
        return style;
    }
    return bpmsCompatible(fromBpm, toBpm) ? Style::BassSwap : Style::Fade;
}

QString MixTransition::styleName(Style style) {
    switch (style) {
    case Style::Auto:
        return QObject::tr("Auto");
    case Style::Fade:
        return QObject::tr("Fade");
    case Style::Rise:
        return QObject::tr("Rise");
    case Style::FilterOut:
        return QObject::tr("Filter Out");
    case Style::BassSwap:
        return QObject::tr("Bass Swap");
    case Style::Cut:
        return QObject::tr("Cut");
    case Style::Overlap:
        return QObject::tr("Overlap");
    }
    return QString();
}

QString MixTransition::styleDescription(Style style) {
    switch (style) {
    case Style::Auto:
        return QObject::tr("Picks Bass Swap when the tempos match, otherwise Fade.");
    case Style::Fade:
        return QObject::tr("A smooth crossfade from one track to the next.");
    case Style::Rise:
        return QObject::tr(
                "The next track rises in through an opening filter while the "
                "current one sinks out muffled.");
    case Style::FilterOut:
        return QObject::tr(
                "The current track thins out through a high-pass sweep while "
                "the next fades in.");
    case Style::BassSwap:
        return QObject::tr(
                "Both play together with the next track's bass cut; halfway "
                "through, the basslines swap.");
    case Style::Cut:
        return QObject::tr("Switches instantly at the transition point.");
    case Style::Overlap:
        return QObject::tr(
                "Both tracks play at full volume during the overlap; at the end "
                "the current track stops and the next one keeps playing.");
    }
    return QString();
}

QString MixTransition::curveName(Curve curve) {
    switch (curve) {
    case Curve::Smooth:
        return QObject::tr("Smooth");
    case Curve::Linear:
        return QObject::tr("Linear");
    case Curve::Early:
        return QObject::tr("Early");
    case Curve::Late:
        return QObject::tr("Late");
    }
    return QString();
}

double MixTransition::applyCurve(Curve curve, double progress) {
    const double p = clamp01(progress);
    switch (curve) {
    case Curve::Smooth:
        return p * p * (3.0 - 2.0 * p);
    case Curve::Linear:
        return p;
    case Curve::Early:
        return std::sqrt(p);
    case Curve::Late:
        return p * p;
    }
    return p;
}

double MixTransition::crossfadePosition(Style effectiveStyle, Curve curve, double progress) {
    const double p = clamp01(progress);
    const double c = applyCurve(curve, p);
    switch (effectiveStyle) {
    case Style::BassSwap:
        // Bring the new track up by the swap point and only then let the old
        // one go, so both basslines never clash.
        return 0.5 * smoothstep(0.0, 0.45, c) + 0.5 * smoothstep(0.55, 1.0, p);
    case Style::Cut:
        return p >= 0.5 ? 1.0 : 0.0;
    case Style::Overlap:
        // Crossfader centered: both decks at full volume (default curve).
        return p >= 1.0 ? 1.0 : 0.5;
    case Style::Auto:
    case Style::Fade:
    case Style::Rise:
    case Style::FilterOut:
        break;
    }
    return c;
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
    m_pConfig->setValue(kMixEnabledKey, enabled);
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
    root.insert(QStringLiteral("version"), 1);
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
          m_style(MixTransition::Style::Fade) {
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

double MixEffectDriver::original(const QString& group, const QString& item) {
    if (!control(group, item)) {
        return 1.0;
    }
    return m_controls.at(group + item).original;
}

void MixEffectDriver::begin(const QString& fromGroup,
        const QString& toGroup,
        const MixTransition& transition,
        MixTransition::Style effectiveStyle) {
    finish();
    m_from = fromGroup;
    m_to = toGroup;
    m_transition = transition;
    m_style = effectiveStyle;
    m_active = true;

    if (transition.syncTempo) {
        // One-shot beat sync of the incoming deck to the playing one.
        set(m_to, QStringLiteral("beatsync"), 1.0);
        set(m_to, QStringLiteral("beatsync"), 0.0);
    }
    kLogger.debug() << "Begin" << MixTransition::styleName(m_style) << m_from
                    << "->" << m_to << "length" << transition.length;
    apply(0.0);
}

double MixEffectDriver::apply(double progress) {
    if (!m_active) {
        return clamp01(progress);
    }
    const double p = clamp01(progress);
    const double c = MixTransition::crossfadePosition(m_style, m_transition.curve, p);

    switch (m_style) {
    case MixTransition::Style::Rise: {
        // Incoming opens up from a thin high-passed sound to full range.
        const double open = smoothstep(0.0, 0.85, p);
        set(quickEffectGroup(m_to),
                kFilterItem,
                kFilterHighPassMax - (kFilterHighPassMax - kFilterNeutral) * open);
        // Outgoing gets muffled as it leaves.
        set(quickEffectGroup(m_from),
                kFilterItem,
                kFilterNeutral - (kFilterNeutral - kFilterLowPassMax) * smoothstep(0.3, 1.0, p));
        return c;
    }
    case MixTransition::Style::FilterOut:
        set(quickEffectGroup(m_from),
                kFilterItem,
                kFilterNeutral + (kFilterHighPassMax - kFilterNeutral) * smoothstep(0.0, 1.0, p));
        return c;
    case MixTransition::Style::BassSwap: {
        // Swap over a short window around the middle of the transition.
        const double swap = smoothstep(0.45, 0.55, p);
        set(eqGroup(m_from), kLowEqItem, original(eqGroup(m_from), kLowEqItem) * (1.0 - swap));
        set(eqGroup(m_to), kLowEqItem, original(eqGroup(m_to), kLowEqItem) * swap);
        return c;
    }
    case MixTransition::Style::Cut:
    case MixTransition::Style::Overlap:
    case MixTransition::Style::Auto: // resolved before begin()
    case MixTransition::Style::Fade:
        break;
    }
    return c;
}

void MixEffectDriver::finish() {
    if (!m_active) {
        m_controls.clear();
        return;
    }
    m_active = false;
    for (auto it = m_controls.begin(); it != m_controls.end(); ++it) {
        if (it->first.endsWith(QStringLiteral("beatsync"))) {
            continue;
        }
        it->second.pControl->set(it->second.original);
    }
    m_controls.clear();
    kLogger.debug() << "Finished transition" << m_from << "->" << m_to;
}
