#include "waveform/widgets/allshader/simplewaveformwidget.h"

#include "waveform/renderers/allshader/waveformrenderbackground.h"
#include "waveform/renderers/allshader/waveformrenderbeat.h"
#include "waveform/renderers/allshader/waveformrendererendoftrack.h"
#include "waveform/renderers/allshader/waveformrendererpreroll.h"
#include "waveform/renderers/allshader/waveformrenderersimple.h"
#include "waveform/renderers/allshader/waveformrendermark.h"
#include "waveform/renderers/allshader/waveformrendermarkrange.h"
#include "waveform/renderers/allshader/waveformrendermixtransition.h"
#include "waveform/widgets/allshader/moc_simplewaveformwidget.cpp"

namespace allshader {

SimpleWaveformWidget::SimpleWaveformWidget(const QString& group, QWidget* parent)
        : WaveformWidget(group, parent) {
    addRenderer<WaveformRenderBackground>();
    addRenderer<WaveformRendererEndOfTrack>();
    addRenderer<WaveformRendererPreroll>();
    addRenderer<WaveformRenderMarkRange>();
    addRenderer<WaveformRendererSimple>();
    addRenderer<WaveformRenderBeat>();
    addRenderer<WaveformRenderMark>();
    addRenderer<WaveformRenderMixTransition>();

    m_initSuccess = init();
}

void SimpleWaveformWidget::castToQWidget() {
    m_widget = this;
}

void SimpleWaveformWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
}

} // namespace allshader
