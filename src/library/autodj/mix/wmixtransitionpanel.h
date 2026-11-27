#pragma once

#include <QWidget>

#include "library/autodj/mix/mixtransition.h"

class MixTransitionEditor;
class MixTransitionPlayer;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;

/// Skin widget (<MixTransitionPanel/>): a slim control bar for the
/// transition between the tracks on deck 1 and deck 2. The transition itself
/// is shown and edited on the main waveforms (see MixTransitionEditor and
/// WaveformRenderMixTransition); this bar sets the mix points from the
/// decks' positions, the length, tempo target, which line to edit, preview
/// and the Armed switch.
class WMixTransitionPanel : public QWidget {
    Q_OBJECT
  public:
    WMixTransitionPanel(QWidget* pParent,
            MixTransitionStore* pStore,
            MixTransitionEditor* pEditor,
            MixTransitionPlayer* pPlayer);

  private slots:
    void refresh();

  private:
    void applyTempoControls();

    MixTransitionStore* m_pStore;
    MixTransitionEditor* m_pEditor;
    MixTransitionPlayer* m_pPlayer;
    bool m_updating;

    QLabel* m_pTracks;
    QPushButton* m_pSwap;
    QPushButton* m_pEdit;
    QButtonGroup* m_pLaneButtons;
    QCheckBox* m_pSnap;
    QLabel* m_pStatus;
    QPushButton* m_pReset;
    QPushButton* m_pArm;
    QPushButton* m_pMixOut;
    QPushButton* m_pMixIn;
    QDoubleSpinBox* m_pLength;
    QComboBox* m_pTempoMode;
    QDoubleSpinBox* m_pCustomBpm;
    QCheckBox* m_pRamp;
    QPushButton* m_pPreview;
    QLabel* m_pHint;
};
