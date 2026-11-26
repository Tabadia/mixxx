#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <map>
#include <memory>
#include <optional>

#include "preferences/usersettings.h"
#include "track/track_decl.h"
#include "track/trackid.h"

class ControlProxy;

/// A saved, hand-tuned transition between two specific tracks ("Mix").
///
/// Times are in seconds of track time (independent of the deck's tempo).
/// The transition starts when track A ("from") reaches `fromTrigger`; at that
/// moment track B ("to") starts playing at `toStart`, and the two overlap for
/// `length` seconds while the style's volume / EQ / filter automation runs.
struct MixTransition {
    enum class Style {
        Auto = 0,  // pick based on BPM match: Bass Swap if close, else Fade
        Fade,      // plain crossfade along the chosen curve
        Rise,      // B rises in through a high-pass sweep, A dips out low-passed
        FilterOut, // A sweeps out through a high-pass filter, B fades in clean
        BassSwap,  // classic DJ blend: swap the low end halfway through
        Cut,       // hard cut at the trigger point (no overlap)
    };
    static constexpr int kStyleCount = 6;

    enum class Curve {
        Smooth = 0, // S-curve, gentle at both ends
        Linear,
        Early, // B comes in quickly, A lingers
        Late,  // B stays low until late, then takes over
    };
    static constexpr int kCurveCount = 4;

    double fromTrigger = 0.0;
    double toStart = 0.0;
    double length = 8.0;
    Style style = Style::Auto;
    Curve curve = Curve::Smooth;
    bool syncTempo = true;

    QJsonObject toJson() const;
    static std::optional<MixTransition> fromJson(const QJsonObject& obj);

    /// Resolves Style::Auto for the given pair of BPMs.
    Style effectiveStyle(double fromBpm, double toBpm) const;

    static QString styleName(Style style);
    static QString styleDescription(Style style);
    static QString curveName(Curve curve);
    /// Maps linear progress 0..1 through the curve.
    static double applyCurve(Curve curve, double progress);
    /// How far the mix has moved from the outgoing (0) to the incoming (1)
    /// track at linear progress `progress`, for an already resolved style.
    static double crossfadePosition(Style effectiveStyle, Curve curve, double progress);
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

    /// "Mix" master switch: when off, saved transitions are ignored and
    /// Auto DJ behaves exactly like stock Mixxx.
    bool isEnabled() const;
    void setEnabled(bool enabled);

  signals:
    void transitionsChanged();

  private:
    static QString key(TrackId fromId, TrackId toId);
    void load();
    void save() const;

    UserSettingsPointer m_pConfig;
    QString m_filePath;
    QHash<QString, QJsonObject> m_entries;
};

/// Drives the per-deck automation (crossfader shape, EQ, quick-effect filter) for a running transition. Original control values are captured on
/// begin() and restored on finish(), so the user's own EQ settings survive.
class MixEffectDriver {
  public:
    MixEffectDriver();
    ~MixEffectDriver();

    void begin(const QString& fromGroup,
            const QString& toGroup,
            const MixTransition& transition,
            MixTransition::Style effectiveStyle);
    /// Applies automation for `progress` (0..1) and returns how far the
    /// crossfader should be moved from the "from" side to the "to" side (0..1).
    double apply(double progress);
    void finish();
    bool isActive() const {
        return m_active;
    }

  private:
    struct Saved {
        std::unique_ptr<ControlProxy> pControl;
        double original;
    };
    ControlProxy* control(const QString& group, const QString& item);
    void set(const QString& group, const QString& item, double value);
    double original(const QString& group, const QString& item);

    bool m_active;
    QString m_from;
    QString m_to;
    MixTransition m_transition;
    MixTransition::Style m_style;
    std::map<QString, Saved> m_controls;
};
