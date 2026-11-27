#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>
#include <map>
#include <memory>
#include <optional>

#include "preferences/usersettings.h"
#include "track/track_decl.h"
#include "track/trackid.h"

class ControlProxy;

/// One editable point of an automation line. x is the progress through the
/// transition (0 = start, 1 = end), y is the normalized value (0..1).
struct MixPoint {
    double x;
    double y;
};

/// The automation lines a transition can carry.
enum class MixLane {
    VolumeA = 0, // outgoing deck fader, y = level
    VolumeB,     // incoming deck fader
    LowA,        // EQ bands, y = 0 kill, 0.5 unchanged, 1 boost (+6 dB)
    MidA,
    HighA,
    LowB,
    MidB,
    HighB,
    FilterA, // quick-effect filter, y = knob (0.5 off, <0.5 low-pass, >0.5 high-pass)
    FilterB,
    Tempo, // y = 0 start BPM .. 1 target BPM (shared by both decks)
    Count,
};
constexpr int kMixLaneCount = static_cast<int>(MixLane::Count);

/// A saved, hand-tuned transition between two specific tracks ("Mix").
///
/// Times are in seconds of track time (independent of the deck's tempo).
/// The transition starts when track A ("from") reaches `fromTrigger`; at that
/// moment track B ("to") starts playing at `toStart`, and the two overlap for
/// `length` seconds while the automation lines run.
struct MixTransition {
    enum class TempoMode {
        Off = 0, // leave both decks' tempo alone
        KeepA,   // B plays at A's tempo
        UseB,    // end up at B's own tempo
        Custom,  // end up at customBpm
    };

    double fromTrigger = 0.0;
    double toStart = 0.0;
    double length = 8.0;
    TempoMode tempoMode = TempoMode::KeepA;
    double customBpm = 0.0;
    // Ramp the tempo across the transition; when off it changes at the end,
    // once A is out. An edited Tempo line overrides both.
    bool tempoRamp = true;
    // Only lanes the user edited are stored; others use defaultLane().
    QHash<int, QVector<MixPoint>> lanes;

    QVector<MixPoint> lane(MixLane lane) const;
    void setLane(MixLane lane, const QVector<MixPoint>& points);
    /// True when the lane is stored and differs from its default.
    bool isLaneActive(MixLane lane) const;
    QVector<MixPoint> defaultLane(MixLane lane) const;

    QJsonObject toJson() const;
    static std::optional<MixTransition> fromJson(const QJsonObject& obj);

    /// Linear interpolation along sorted points, clamped at the ends.
    static double evaluate(const QVector<MixPoint>& points, double x);
    static QString laneName(MixLane lane);
    /// True when two BPMs are close enough (incl. half/double time) to blend.
    static bool bpmsCompatible(double bpmA, double bpmB, double tolerance = 0.06);
};

/// Persistent storage of transitions, keyed by (from track, to track).
/// Saved as JSON next to the library database.
class MixTransitionStore : public QObject {
    Q_OBJECT
  public:
    explicit MixTransitionStore(UserSettingsPointer pConfig, QObject* pParent = nullptr);

    std::optional<MixTransition> get(TrackId fromId, TrackId toId) const;
    bool contains(TrackId fromId, TrackId toId) const;
    void set(const TrackPointer& pFrom, const TrackPointer& pTo, const MixTransition& transition);
    void remove(TrackId fromId, TrackId toId);

    /// Master switch ("Arm" / "Mix"): when off, saved transitions are not
    /// triggered, neither by hand nor by Auto DJ.
    bool isEnabled() const;
    void setEnabled(bool enabled);
    /// True when saved transitions should fire: the "Transition" master
    /// switch ([Mix],show) is on and the transitions are armed. When false,
    /// mixing (by hand and Auto DJ) behaves like stock Mixxx.
    bool isActive() const;

  signals:
    void transitionsChanged();
    void enabledChanged(bool enabled);

  private:
    static QString key(TrackId fromId, TrackId toId);
    void load();
    void save() const;

    UserSettingsPointer m_pConfig;
    QString m_filePath;
    QHash<QString, QJsonObject> m_entries;
};

/// Drives a running transition on two decks: fader levels, EQ, filter and
/// tempo along the automation lines. Original EQ / filter / fader values are
/// captured on begin() and restored on finish(), so the user's settings
/// survive. Tempo is left at the target.
class MixEffectDriver {
  public:
    MixEffectDriver();
    ~MixEffectDriver();

    void begin(const QString& fromGroup,
            const QString& toGroup,
            const MixTransition& transition,
            double toTrackFileBpm);
    /// Call right after the incoming deck started playing (aligns beats).
    void started();
    /// Applies automation for `progress` (0..1).
    void apply(double progress);
    void finish();
    bool isActive() const {
        return m_active;
    }
    double startBpm() const {
        return m_startBpm;
    }
    double targetBpm() const {
        return m_targetBpm;
    }

  private:
    struct Saved {
        std::unique_ptr<ControlProxy> pControl;
        double original;
    };
    ControlProxy* control(const QString& group, const QString& item);
    void set(const QString& group, const QString& item, double value);
    double get(const QString& group, const QString& item);
    double original(const QString& group, const QString& item);

    bool m_active;
    QString m_from;
    QString m_to;
    MixTransition m_transition;
    double m_startBpm;
    double m_targetBpm;
    std::map<QString, Saved> m_controls;
};
