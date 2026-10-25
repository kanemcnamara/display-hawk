#pragma once

#include <QColor>
#include <QHash>
#include <QRectF>
#include <QVector>
#include <rhi/qrhi.h>

#include "model/Project.h"

// Draws textured or solid quads with QRhi. Not thread-safe: owned and used by the render thread.
class RhiQuad
{
public:
    // Straight: textures/patterns with straight alpha over the target (the default).
    // The others composite a premultiplied layer texture with a layer blend mode.
    enum class Blend { Straight, Normal, Add, Screen, Multiply, Lighten, Darken };
    static Blend fromLayer(BlendMode m) { return Blend(int(m) + 1); }

    struct Params {
        QSizeF target;             // render target size in pixels
        QRectF dst;                // destination rect, top-left origin
        QRectF src = {0, 0, 1, 1}; // normalized source rect, top-left origin
        // Rotated source: when set, the source is src.topLeft() + s * srcU + t * srcV (normalized).
        bool srcAxes = false;
        QPointF srcU, srcV;
        int rotation = 0;          // quarter turns clockwise
        bool flipH = false;
        bool flipV = false;
        bool texFlipY = false;     // sampling a texture that was rendered on a Y-up backend
        QRhiTexture *texture = nullptr; // nullptr = solid colour
        QColor color = Qt::white;
        float opacity = 1.0f;
        float brightness = 1.0f;
        QRect clip;                // scissor in target pixels (top-left origin); empty = none
        Blend blend = Blend::Straight;
        // LED colour correction (slices): channel gains, gamma exponent, black level.
        bool colorCorrect = false;
        float gain[3] = {1.0f, 1.0f, 1.0f};
        float gamma = 1.0f;
        float blackLevel = 0.0f;
    };

    bool init(QRhi *rhi);
    void release();

    // Call once per QRhi frame, before recording any pass.
    void newFrame() { m_offset = 0; }

    // Records one render pass: clears rt, then draws in order. `before` and `after` are optional
    // update batches submitted at the start and end of the pass (uploads, readbacks).
    void recordPass(QRhiCommandBuffer *cb, QRhiRenderTarget *rt, const QColor &clear, const QVector<Params> &draws,
                    QRhiResourceUpdateBatch *before = nullptr, QRhiResourceUpdateBatch *after = nullptr);

    // Must be called before a texture that may have been used here is destroyed.
    void forgetTexture(QRhiTexture *tex);

private:
    QRhiGraphicsPipeline *pipelineFor(QRhiRenderPassDescriptor *rp, Blend blend);
    QRhiShaderResourceBindings *srbFor(QRhiTexture *tex);

    QRhi *m_rhi = nullptr;
    QShader m_vs, m_fs;
    QRhiBuffer *m_ubuf = nullptr;
    QRhiSampler *m_sampler = nullptr;
    QRhiTexture *m_white = nullptr;
    QRhiShaderResourceBindings *m_layoutSrb = nullptr;
    QHash<QRhiTexture *, QRhiShaderResourceBindings *> m_srbs;
    struct PipelineKey {
        QVector<quint32> format;
        Blend blend;
        bool operator==(const PipelineKey &o) const { return blend == o.blend && format == o.format; }
    };
    QVector<QPair<PipelineKey, QRhiGraphicsPipeline *>> m_pipelines;
    quint32 m_stride = 0;
    quint32 m_offset = 0;
    bool m_whiteUploaded = false;
    bool m_warnedFull = false;
};
