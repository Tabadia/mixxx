#pragma once

#include <QFutureWatcher>
#include <QVector>
#include <QWidget>

#include "library/autodj/mix/mixtransition.h"
#include "track/track_decl.h"

/// Two-lane waveform editor for a Mix transition.
///
/// The horizontal axis is "mix time": 0 is the moment the transition starts.
/// The top lane shows the outgoing track (A), the bottom lane the incoming
/// track (B), both positioned so that what plays together is drawn stacked.
///  - drag the top waveform to move where A mixes out
///  - drag the bottom waveform to move where B comes in
///  - drag the "end" handle to change the overlap length
///  - drag the ruler to pan, scroll to zoom
class MixWaveformView : public QWidget {
    Q_OBJECT
  public:
    explicit MixWaveformView(QWidget* pParent = nullptr);
    ~MixWaveformView() override;

    void setTracks(const TrackPointer& pFrom, const TrackPointer& pTo);
    void setTransition(const MixTransition& transition);
    const MixTransition& transition() const {
        return m_transition;
    }
    void setSnapToBeats(bool snap);
    /// Mark the playback position during a preview (mix time, NaN to hide).
    void setPlayhead(double mixTime);
    void frameTransition();

    /// Snaps a time of the given lane (0 = A, 1 = B) to its closest beat.
    double snapToBeat(int lane, double seconds) const;
    /// Beat length of track A in seconds, 0 if unknown.
    double beatLength() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  signals:
    void transitionEdited();

  protected:
    void paintEvent(QPaintEvent* pEvent) override;
    void mousePressEvent(QMouseEvent* pEvent) override;
    void mouseMoveEvent(QMouseEvent* pEvent) override;
    void mouseReleaseEvent(QMouseEvent* pEvent) override;
    void wheelEvent(QWheelEvent* pEvent) override;

  private:
    struct Lane {
        TrackPointer pTrack;
        QVector<float> envelope;
        bool loading = false;
        QFutureWatcher<QVector<float>>* pWatcher = nullptr;
    };
    enum class Drag {
        None,
        LaneA,
        LaneB,
        Length,
        Pan,
    };

    void loadEnvelope(int lane);
    QRectF laneRect(int lane) const;
    QRectF rulerRect() const;
    double xForMixTime(double mixTime) const;
    double mixTimeForX(double x) const;
    double laneTime(int lane, double mixTime) const;
    double envelopeAt(int lane, double trackSeconds) const;
    void paintLane(QPainter* pPainter, int lane);
    void paintBeats(QPainter* pPainter, int lane);
    void paintRuler(QPainter* pPainter);
    void paintOverlap(QPainter* pPainter);
    void applySnap();

    Lane m_lanes[2];
    MixTransition m_transition;
    MixTransition m_dragStartTransition;
    double m_viewStart;
    double m_viewSpan;
    double m_dragStartViewStart;
    bool m_snap;
    double m_playhead;
    Drag m_drag;
    QPointF m_dragStartPos;
};
