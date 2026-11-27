#pragma once

#include <QColor>
#include <QPointF>
#include <vector>

#include "shaders/unicolorshader.h"
#include "util/class.h"
#include "waveform/renderers/allshader/waveformrenderer.h"

class QDomNode;
class SkinContext;

namespace allshader {
class WaveformRenderMixTransition;
}

/// Draws the saved/edited transition on the main waveform: the region where
/// this deck mixes out (deck A) or in (deck B), and its automation lines
/// (volume, EQ, filter, tempo) with handles while editing. Data comes from
/// MixTransitionEditor.
class allshader::WaveformRenderMixTransition final : public allshader::WaveformRenderer {
  public:
    explicit WaveformRenderMixTransition(WaveformWidgetRenderer* waveformWidget);

    void setup(const QDomNode& node, const SkinContext& context) override {
        Q_UNUSED(node);
        Q_UNUSED(context);
    }
    void initializeGL() override;
    void paintGL() override;

  private:
    void fillRect(const QRectF& rect, const QColor& color);
    void drawPolyline(const std::vector<QPointF>& points, float thickness, const QColor& color);

    mixxx::UnicolorShader m_shader;
    std::vector<float> m_vertices;

    DISALLOW_COPY_AND_ASSIGN(WaveformRenderMixTransition);
};
