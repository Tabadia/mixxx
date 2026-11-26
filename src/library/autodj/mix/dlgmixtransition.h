#pragma once

#include <QDialog>
#include <QTimer>
#include <memory>

#include "library/autodj/mix/mixtransition.h"
#include "track/track_decl.h"

class AutoDJProcessor;
class ControlProxy;
class MixWaveformView;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;

/// Editor for the Mix transition between two tracks, modeled on Spotify's
/// playlist "Mix" editor: pick a style, drag the two waveforms to choose where
/// the transition happens, adjust its length and curve, preview it, save it.
class DlgMixTransition : public QDialog {
    Q_OBJECT
  public:
    DlgMixTransition(QWidget* pParent,
            AutoDJProcessor* pProcessor,
            TrackPointer pFrom,
            TrackPointer pTo);
    ~DlgMixTransition() override;

  signals:
    void loadTrackToPlayer(TrackPointer pTrack, const QString& group, bool play);

  private slots:
    void slotSuggest();
    void slotSave();
    void slotRemove();
    void slotTogglePreview();
    void slotPreviewTick();

  private:
    enum class PreviewState {
        Idle,
        Loading,
        LeadIn,
        Mixing,
        Tail,
    };

    MixTransition suggestTransition() const;
    void updateFromView();
    void updateControls();
    void updateInfo();
    MixTransition currentTransition() const;
    void stopPreview(const QString& message = QString());
    void setCrossfaderToward(double t);
    bool deckHasTrack(int deck, const TrackPointer& pTrack) const;
    double deckPosition(int deck) const;

    AutoDJProcessor* m_pProcessor;
    MixTransitionStore* m_pStore;
    TrackPointer m_pFrom;
    TrackPointer m_pTo;
    bool m_updating;

    QLabel* m_pHeader;
    QLabel* m_pMatchInfo;
    MixWaveformView* m_pView;
    QComboBox* m_pStyle;
    QLabel* m_pStyleDescription;
    QComboBox* m_pCurve;
    QDoubleSpinBox* m_pLength;
    QLabel* m_pLengthBeats;
    QDoubleSpinBox* m_pTrigger;
    QDoubleSpinBox* m_pStart;
    QCheckBox* m_pSync;
    QCheckBox* m_pSnap;
    QPushButton* m_pPreview;
    QPushButton* m_pRemove;
    QLabel* m_pStatus;

    // Preview on decks 1 and 2
    QTimer m_previewTimer;
    PreviewState m_previewState;
    int m_previewTicks;
    MixTransition m_previewTransition;
    MixEffectDriver m_previewDriver;
    std::unique_ptr<ControlProxy> m_pPlay[2];
    std::unique_ptr<ControlProxy> m_pPlayPos[2];
    std::unique_ptr<ControlProxy> m_pDuration[2];
    std::unique_ptr<ControlProxy> m_pTrackLoaded[2];
    std::unique_ptr<ControlProxy> m_pCrossfader;
    std::unique_ptr<ControlProxy> m_pCrossfaderReverse;
};
