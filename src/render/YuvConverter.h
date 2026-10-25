#pragma once

#include <QVideoFrame>
#include <rhi/qrhi.h>

// Converts decoded YUV video frames to RGBA on the GPU: the planes are uploaded as they come from
// the decoder (much less data than RGBA) and a shader applies the frame's colour matrix and range.
// Render-thread only.
class YuvConverter
{
public:
    // GPU resources for one clip's video: its planes and the RGBA texture they are converted into.
    struct Target {
        QRhiTexture *planes[3] = {};
        QRhiTexture *rgba = nullptr; // sample this; it is a render target (flip Y on Y-up backends)
        QRhiTextureRenderTarget *rt = nullptr;
        QRhiRenderPassDescriptor *rp = nullptr;
        QRhiShaderResourceBindings *srb = nullptr;
        QRhiBuffer *ubuf = nullptr;
        QSize size;
        QVideoFrameFormat::PixelFormat format = QVideoFrameFormat::Format_Invalid;
    };

    static bool supports(QVideoFrameFormat::PixelFormat format);

    bool init(QRhi *rhi);
    void release();

    // Uploads the frame's planes and records a conversion pass into target.rgba (recreating
    // resources when the size or format changes). Call outside any other render pass.
    bool convert(QRhiCommandBuffer *cb, const QVideoFrame &frame, Target &target);
    void releaseTarget(Target &target, bool deferred);

private:
    bool ensureTarget(Target &t, const QVideoFrameFormat &format);
    QRhiGraphicsPipeline *pipelineFor(QRhiRenderPassDescriptor *rp, QRhiShaderResourceBindings *srb);

    QRhi *m_rhi = nullptr;
    QShader m_vs, m_fs;
    QRhiSampler *m_sampler = nullptr;
    QRhiGraphicsPipeline *m_pipeline = nullptr;
    QVector<quint32> m_pipelineFormat;
};
