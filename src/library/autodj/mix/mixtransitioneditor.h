#pragma once

#include <QColor>
#include <QObject>
#include <QTimer>
#include <QVector>
#include <memory>

#include "library/autodj/mix/mixtransition.h"
#include "preferences/usersettings.h"
#include "track/track_decl.h"

class ControlObject;
class ControlProxy;
class ControlPushButton;

/// The transition being edited between the tracks on deck 1 and deck 2.
///
/// Shared by the transition control bar, the main waveforms (which draw the
/// mix-out / mix-in regions and automation lines, and let you edit them
/// with the mouse) and the controls "[Mix],edit_mode", "[Mix],preview",
/// "[ChannelN],mix_point". Edits are saved to the MixTransitionStore.
/// Lives in the GUI thread.
class MixTransitionEditor : public QObject {
    Q_OBJECT
  public:
    enum class LaneGroup {
        Volume = 0,
        Bass,
        Mid,
        Treble,
        Filter,
        Tempo,
    };
    enum class Role {
        None,
        From, // deck A, mixing out
        To,   // deck B, mixing in
    };

    /// What a waveform draws for its deck.
    struct Overlay {
        struct Line {
            QVector<MixPoint> points;
            QColor color;
            bool editable = false;
        };
        Role role = Role::None;
        double regionStart = 0.0; // seconds of track time
        double regionEnd = 0.0;
        QColor color;
        bool editMode = false;
        QVector<Line> lines;
    };

    MixTransitionEditor(UserSettingsPointer pConfig,
            MixTransitionStore* pStore,
            QObject* pParent = nullptr);
    ~MixTransitionEditor() override;

    /// The single instance (created by Library), or nullptr.
    static MixTransitionEditor* instance();

    bool hasPair() const {
        return m_pFrom && m_pTo;
    }
    int fromDeck() const {
        return m_fromDeck;
    }
    TrackPointer fromTrack() const {
        return m_pFrom;
    }
    TrackPointer toTrack() const {
        return m_pTo;
    }
    const MixTransition& transition() const {
        return m_transition;
    }
    bool isSaved() const {
        return m_saved;
    }
    bool isSaving() const {
        return m_saveTimer.isActive();
    }
    LaneGroup laneGroup() const {
        return m_group;
    }
    bool editMode() const;
    /// "[Mix],show" (toolbar "Transition"): the master switch. When off,
    /// nothing transition-related is drawn or editable and saved transitions
    /// don't fire (see MixTransitionStore::isActive()).
    bool isVisible() const;
    void setVisible(bool visible);
    bool snap() const {
        return m_snap;
    }
    double deckBpm(int deck) const;
    double startBpm() const {
        return deckBpm(m_fromDeck);
    }
    double targetBpm() const;

    void setTransition(const MixTransition& transition);
    void setLaneGroup(LaneGroup group);
    void setEditMode(bool on);
    void setSnap(bool on);
    void setLengthBeats(double beats);
    double lengthBeats() const;
    void setMixOutHere();
    void setMixInHere();
    void swap();
    void reset();
    void preview();

    Overlay overlay(const QString& group) const;

    // Mouse editing from the waveforms (positions in track seconds, values
    // 0..1 bottom to top; tolerances already converted from pixels).
    bool beginDrag(const QString& group,
            double seconds,
            double value,
            double secondsTolerance,
            double valueTolerance);
    void drag(const QString& group, double seconds, double value);
    void endDrag();
    bool removePoint(const QString& group,
            double seconds,
            double value,
            double secondsTolerance,
            double valueTolerance);

  signals:
    void changed();

  private slots:
    void slotTrackChanged(const QString& group, TrackPointer pNew, TrackPointer pOld);
    void save();

  private:
    enum class DragMode {
        None,
        Point,
        RegionStart,
        RegionEnd,
    };
    int deckIndex(const QString& group) const;
    Role role(const QString& group) const;
    double regionStart(Role role) const;
    QVector<MixLane> lanesForDeck(Role role) const;
    MixLane editableLane(Role role) const;
    double snapToBeat(const TrackPointer& pTrack, double seconds) const;
    void reload();
    void edited();
    MixTransition suggest() const;

    static MixTransitionEditor* s_pInstance;

    UserSettingsPointer m_pConfig;
    MixTransitionStore* m_pStore;
    QTimer m_saveTimer;
    int m_fromDeck;
    bool m_manualDirection;
    TrackPointer m_pFrom;
    TrackPointer m_pTo;
    MixTransition m_transition;
    bool m_saved;
    LaneGroup m_group;
    bool m_snap;

    DragMode m_dragMode;
    MixLane m_dragLane;
    int m_dragIndex;

    std::unique_ptr<ControlPushButton> m_pShow;
    std::unique_ptr<ControlPushButton> m_pEditMode;
    std::unique_ptr<ControlPushButton> m_pPreview;
    std::unique_ptr<ControlPushButton> m_pMixPoint[2];
    std::unique_ptr<ControlProxy> m_pPlay[2];
    std::unique_ptr<ControlProxy> m_pPlayPos[2];
    std::unique_ptr<ControlProxy> m_pDuration[2];
    std::unique_ptr<ControlProxy> m_pBpm[2];
    std::unique_ptr<ControlProxy> m_pAutoDJ;
};
