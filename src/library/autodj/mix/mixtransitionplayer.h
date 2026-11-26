#pragma once

#include <QObject>
#include <QTimer>
#include <memory>

#include "library/autodj/mix/mixtransition.h"
#include "track/track_decl.h"

class ControlProxy;

/// Fires saved transitions while DJing by hand: when the track on one deck
/// (A) is playing, the paired track (B) is loaded on the other deck and A
/// crosses the transition's trigger point, B starts and the automation runs.
/// Works on decks 1 and 2 and stands back while Auto DJ is enabled, which
/// runs saved transitions itself.
class MixTransitionPlayer : public QObject {
    Q_OBJECT
  public:
    MixTransitionPlayer(MixTransitionStore* pStore, QObject* pParent = nullptr);
    ~MixTransitionPlayer() override;

    bool isRunning() const {
        return m_running;
    }
    /// Deck index (0 or 1) of the outgoing deck while running, else -1.
    int fromDeck() const {
        return m_running ? m_from : -1;
    }

  signals:
    /// progress 0..1 while a transition runs; emitted on every tick.
    void progressChanged(int fromDeck, double progress);
    void runningChanged(bool running);

  private slots:
    void slotTick();

  private:
    struct Deck {
        std::unique_ptr<ControlProxy> pPlay;
        std::unique_ptr<ControlProxy> pPlayPos;
        std::unique_ptr<ControlProxy> pDuration;
        std::unique_ptr<ControlProxy> pTrackLoaded;
        double lastSeconds = -1.0;
    };
    double seconds(int deck) const;
    bool isPlaying(int deck) const;
    void start(int from, const MixTransition& transition, const TrackPointer& pTo);
    void stop();

    MixTransitionStore* m_pStore;
    Deck m_decks[2];
    std::unique_ptr<ControlProxy> m_pAutoDJEnabled;
    std::unique_ptr<ControlProxy> m_pCrossfader;
    QTimer m_timer;
    MixEffectDriver m_driver;
    MixTransition m_transition;
    bool m_running;
    int m_from;
};
