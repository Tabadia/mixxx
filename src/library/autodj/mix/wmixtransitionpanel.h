#pragma once

#include <QTimer>
#include <QWidget>
#include <memory>

#include "library/autodj/mix/mixtransition.h"
#include "library/autodj/mix/mixwaveformview.h"
#include "track/track_decl.h"

class ControlProxy;
class MixTransitionPlayer;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;

/// Skin widget (<MixTransitionPanel/>): edits the transition between the
/// tracks loaded on deck 1 and deck 2, right in the main window. Shows both
/// waveforms around the transition with editable automation lines (volume,
/// EQ, filter, tempo), the tempo target, and an Arm switch. Edits are saved
/// automatically per track pair.
class WMixTransitionPanel : public QWidget {
    Q_OBJECT
  public:
    WMixTransitionPanel(QWidget* pParent,
            MixTransitionStore* pStore,
            MixTransitionPlayer* pPlayer);
    ~WMixTransitionPanel() override;

  private slots:
    void slotTrackChanged(const QString& group, TrackPointer pNewTrack, TrackPointer pOldTrack);
    void slotEdited();
    void slotSave();
    void slotSwap();
    void slotReset();
    void slotProgress(int fromDeck, double progress);
    void slotRunningChanged(bool running);

  private:
    void reload(bool reframe);
    void updateControls();
    void updateInfo();
    void updateLaneButtons();
    MixTransition currentTransition() const;
    MixTransition suggestTransition() const;
    double deckBpm(int deck) const;
    double targetBpm(const MixTransition& transition) const;
    TrackPointer track(int deck) const;

    MixTransitionStore* m_pStore;
    MixTransitionPlayer* m_pPlayer;
    MixWaveformView* m_pView;
    QLabel* m_pTracks;
    QPushButton* m_pSwap;
    QButtonGroup* m_pLaneButtons;
    QComboBox* m_pTempoMode;
    QDoubleSpinBox* m_pCustomBpm;
    QCheckBox* m_pRamp;
    QCheckBox* m_pSnap;
    QPushButton* m_pArm;
    QPushButton* m_pReset;
    QLabel* m_pStatus;
    QTimer m_saveTimer;
    std::unique_ptr<ControlProxy> m_pBpm[2];

    int m_fromDeck; // 0 or 1
    bool m_manualDirection;
    bool m_updating;
    bool m_saved;
    TrackPointer m_pFromTrack;
    TrackPointer m_pToTrack;
};
