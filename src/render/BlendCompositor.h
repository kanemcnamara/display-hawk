#pragma once

#include "model/Project.h"

#include <QHash>
#include <rhi/qrhi.h>

// Mixes a premultiplied layer onto what is below it with a shader blend mode (blend.frag): for
// the modes the GPU's fixed-function blender cannot do, and the mask modes. Render thread only.
class BlendCompositor
{
public:
    bool init(QRhi *rhi);
    void release();
    void newFrame() { m_offset = 0; }

    // One full-target pass into `out` (every pixel written): base below, layer on top.
    void blend(QRhiCommandBuffer *cb, QRhiTextureRenderTarget *out, QRhiTexture *base, QRhiTexture *layer,
               BlendMode mode, float opacity, bool opaqueBase);
    void forgetTexture(QRhiTexture *tex);

private:
    QRhiShaderResourceBindings *srbFor(QRhiTexture *base, QRhiTexture *layer);
    QRhiGraphicsPipeline *pipelineFor(QRhiRenderPassDescriptor *rp);

    QRhi *m_rhi = nullptr;
    QShader m_vs, m_fs;
    QRhiBuffer *m_ubuf = nullptr;
    QRhiSampler *m_sampler = nullptr;
    QRhiTexture *m_dummy = nullptr;
    QRhiShaderResourceBindings *m_layoutSrb = nullptr;
    QHash<QPair<QRhiTexture *, QRhiTexture *>, QRhiShaderResourceBindings *> m_srbs;
    QRhiGraphicsPipeline *m_pipeline = nullptr;
    QVector<quint32> m_pipelineFormat;
    quint32 m_stride = 0;
    quint32 m_offset = 0;
};
