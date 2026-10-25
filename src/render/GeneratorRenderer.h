#pragma once

#include "model/Project.h"

#include <QHash>

#include <functional>
#include <rhi/qrhi.h>

// Draws generator clips (shaders/gen.frag) into a texture of their own each frame. Render thread only.
class GeneratorRenderer
{
public:
    bool init(QRhi *rhi);
    void release();
    void newFrame() { m_offset = 0; }

    // Renders the clip's generator at `size` and returns the texture (straight alpha, rendered:
    // sample it Y-flipped on Y-up backends); nullptr for an unknown generator.
    QRhiTexture *render(QRhiCommandBuffer *cb, const Clip &clip, const QSize &size, double timeSec, qint64 now);
    void collectGarbage(qint64 now, qint64 idleMs);
    // Called before a texture it handed out is destroyed (others cache bindings by texture).
    std::function<void(QRhiTexture *)> onTextureRelease;

private:
    struct Target {
        QRhiTexture *tex = nullptr;
        QRhiTextureRenderTarget *rt = nullptr;
        QRhiRenderPassDescriptor *rp = nullptr;
        qint64 lastUsed = 0;
    };
    void releaseTarget(Target &t, bool deferred);

    QRhi *m_rhi = nullptr;
    QShader m_vs, m_fs;
    QRhiBuffer *m_ubuf = nullptr;
    QRhiShaderResourceBindings *m_layoutSrb = nullptr;
    QRhiGraphicsPipeline *m_pipeline = nullptr;
    QVector<quint32> m_pipelineFormat;
    QHash<QString, Target> m_targets; // by clip id
    quint32 m_stride = 0;
    quint32 m_offset = 0;
};
