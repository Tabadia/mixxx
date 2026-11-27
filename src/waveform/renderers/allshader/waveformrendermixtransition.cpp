#include "waveform/renderers/allshader/waveformrendermixtransition.h"

#include <cmath>

#include "library/autodj/mix/mixtransitioneditor.h"
#include "track/track.h"
#include "waveform/renderers/allshader/matrixforwidgetgeometry.h"
#include "waveform/renderers/waveformwidgetrenderer.h"

namespace {
constexpr float kLineThickness = 2.0f;
constexpr float kEdgeThickness = 2.0f;
constexpr float kHandleSize = 7.0f;
// Lines use the middle part of the waveform's height.
constexpr double kMarginFraction = 0.08;
} // namespace

allshader::WaveformRenderMixTransition::WaveformRenderMixTransition(
        WaveformWidgetRenderer* waveformWidget)
        : WaveformRenderer(waveformWidget) {
}

void allshader::WaveformRenderMixTransition::initializeGL() {
    WaveformRenderer::initializeGL();
    m_shader.init();
}

void allshader::WaveformRenderMixTransition::fillRect(const QRectF& rect, const QColor& color) {
    const float x1 = static_cast<float>(rect.left());
    const float x2 = static_cast<float>(rect.right());
    const float y1 = static_cast<float>(rect.top());
    const float y2 = static_cast<float>(rect.bottom());
    const float posarray[] = {x1, y1, x2, y1, x1, y2, x2, y2};
    m_shader.setUniformValue(m_shader.colorLocation(), color);
    m_shader.setAttributeArray(m_shader.positionLocation(), GL_FLOAT, posarray, 2);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void allshader::WaveformRenderMixTransition::drawPolyline(
        const std::vector<QPointF>& points, float thickness, const QColor& color) {
    if (points.size() < 2) {
        return;
    }
    m_vertices.clear();
    m_vertices.reserve((points.size() - 1) * 12);
    const float half = thickness / 2.0f;
    for (size_t i = 1; i < points.size(); ++i) {
        const float x0 = static_cast<float>(points[i - 1].x());
        const float y0 = static_cast<float>(points[i - 1].y());
        const float x1 = static_cast<float>(points[i].x());
        const float y1 = static_cast<float>(points[i].y());
        float dx = x1 - x0;
        float dy = y1 - y0;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-3f) {
            continue;
        }
        // Normal of the segment, scaled to half the line thickness.
        const float nx = -dy / len * half;
        const float ny = dx / len * half;
        const float quad[] = {
                x0 + nx, y0 + ny, x1 + nx, y1 + ny, x0 - nx, y0 - ny, //
                x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny};
        m_vertices.insert(m_vertices.end(), std::begin(quad), std::end(quad));
    }
    if (m_vertices.empty()) {
        return;
    }
    m_shader.setUniformValue(m_shader.colorLocation(), color);
    m_shader.setAttributeArray(m_shader.positionLocation(), GL_FLOAT, m_vertices.data(), 2);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m_vertices.size() / 2));
}

void allshader::WaveformRenderMixTransition::paintGL() {
    MixTransitionEditor* pEditor = MixTransitionEditor::instance();
    if (!pEditor) {
        return;
    }
    const TrackPointer pTrack = m_waveformRenderer->getTrackInfo();
    if (!pTrack || pTrack->getSampleRate() <= 0) {
        return;
    }
    const MixTransitionEditor::Overlay overlay = pEditor->overlay(m_waveformRenderer->getGroup());
    if (overlay.role == MixTransitionEditor::Role::None) {
        return;
    }
    // Make sure the overlay belongs to the track this waveform shows.
    const TrackPointer pOverlayTrack = overlay.role == MixTransitionEditor::Role::From
            ? pEditor->fromTrack()
            : pEditor->toTrack();
    if (pOverlayTrack != pTrack) {
        return;
    }

    const double samplesPerSecond = pTrack->getSampleRate() * 2.0; // engine samples
    auto xForSeconds = [&](double seconds) {
        return m_waveformRenderer->transformSamplePositionInRendererWorld(
                seconds * samplesPerSecond);
    };
    const double length = m_waveformRenderer->getLength();
    const double breadth = m_waveformRenderer->getBreadth();
    const double x0 = xForSeconds(overlay.regionStart);
    const double x1 = xForSeconds(overlay.regionEnd);
    if (x1 < 0 || x0 > length) {
        return; // not on screen
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    m_shader.bind();
    m_shader.enableAttributeArray(m_shader.positionLocation());
    m_shader.setUniformValue(
            m_shader.matrixLocation(), matrixForWidgetGeometry(m_waveformRenderer, false));

    // Region tint and edges.
    QColor tint = overlay.color;
    tint.setAlphaF(overlay.editMode ? 0.16f : 0.10f);
    fillRect(QRectF(QPointF(x0, 0), QPointF(std::max(x1, x0 + 1), breadth)), tint);
    QColor edge = overlay.color;
    edge.setAlphaF(0.9f);
    fillRect(QRectF(x0 - kEdgeThickness / 2, 0, kEdgeThickness, breadth), edge);
    edge.setAlphaF(0.6f);
    fillRect(QRectF(x1 - kEdgeThickness / 2, 0, kEdgeThickness, breadth), edge);

    // Automation lines.
    const double top = breadth * kMarginFraction;
    const double span = breadth * (1.0 - 2 * kMarginFraction);
    const double width = x1 - x0;
    if (width > 2) {
        for (const auto& line : overlay.lines) {
            std::vector<QPointF> points;
            points.reserve(line.points.size());
            for (const MixPoint& p : line.points) {
                points.emplace_back(x0 + p.x * width, top + (1.0 - p.y) * span);
            }
            drawPolyline(points, line.editable ? kLineThickness + 0.5f : kLineThickness, line.color);
            if (line.editable) {
                for (const QPointF& p : points) {
                    fillRect(QRectF(p.x() - kHandleSize / 2 - 1,
                                     p.y() - kHandleSize / 2 - 1,
                                     kHandleSize + 2,
                                     kHandleSize + 2),
                            QColor(14, 15, 18, 230));
                    fillRect(QRectF(p.x() - kHandleSize / 2,
                                     p.y() - kHandleSize / 2,
                                     kHandleSize,
                                     kHandleSize),
                            line.color);
                }
            }
        }
    }

    m_shader.disableAttributeArray(m_shader.positionLocation());
    m_shader.release();
}
