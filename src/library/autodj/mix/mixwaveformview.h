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
/// track (B), positioned so that what plays together is drawn stacked.
///
/// Outside the overlap region:
///  - drag the top waveform to move where A mixes out
///  - drag the bottom waveform to move where B comes in
///  - drag the ruler to pan, scroll to zoom
/// The white handle at the end of the overlap changes its length.
/// Inside the overlap region the selected automation lines are edited:
/// click to add a point, drag to move it, double/right-click to delete it.
/// Alt + drag moves the waveforms there too.
class MixWaveformView : public QWidget {
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

    explicit MixWaveformView(QWidget* pParent = nullptr);
    ~MixWaveformView() override;

    void setTracks(const TrackPointer& pFrom, const TrackPointer& pTo);
    void setTransition(const MixTransition& transition);
    const MixTransition& transition() const {
        return m_transition;
    }
    void setLaneGroup(LaneGroup group);
    LaneGroup laneGroup() const {
        return m_group;
    }
    void setSnapToBeats(bool snap);
    /// BPM shown at the bottom / top of the tempo line.
    void setTempoRange(double startBpm, double targetBpm);
    /// Mark the playback position (mix time, NaN to hide).
    void setPlayhead(double mixTime);
    void frameTransition();
    void setPlaceholderText(const QString& text);

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
    void mouseDoubleClickEvent(QMouseEvent* pEvent) override;
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
        Point,
    };
    struct PointHit {
        MixLane lane = MixLane::Count;
        int index = -1;
    };

    void loadEnvelope(int lane);
    QRectF laneRect(int lane) const;
    QRectF rulerRect() const;
    QRectF curveRect(MixLane lane) const;
    double xForMixTime(double mixTime) const;
    double mixTimeForX(double x) const;
    double laneTime(int lane, double mixTime) const;
    double envelopeAt(int lane, double trackSeconds) const;
    double overlapLength() const;
    QPointF pointPos(MixLane lane, const MixPoint& point) const;
    MixPoint pointFromPos(MixLane lane, const QPointF& pos) const;
    QVector<MixLane> editableLanes() const;
    PointHit hitPoint(const QPointF& pos) const;
    MixLane laneAt(const QPointF& pos) const;
    bool inOverlap(const QPointF& pos) const;
    void paintLane(QPainter* pPainter, int lane);
    void paintBeats(QPainter* pPainter, int lane);
    void paintRuler(QPainter* pPainter);
    void paintOverlap(QPainter* pPainter);
    void paintLine(QPainter* pPainter, MixLane lane, bool editable);
    void applySnap();

    Lane m_lanes[2];
    MixTransition m_transition;
    MixTransition m_dragStartTransition;
    LaneGroup m_group;
    double m_viewStart;
    double m_viewSpan;
    double m_dragStartViewStart;
    bool m_snap;
    double m_playhead;
    double m_startBpm;
    double m_targetBpm;
    QString m_placeholder;
    Drag m_drag;
    PointHit m_dragPoint;
    QPointF m_dragStartPos;
};
