#include "render/YuvConverter.h"

#include <QFile>
#include <QMatrix4x4>
#include <cstring>

namespace {

constexpr quint32 kUniformSize = 144; // mat4 clipCorr, mat4 colorMatrix, ivec4 mode

struct PlaneLayout {
    int count = 0;
    QRhiTexture::Format format[3] = {};
    QSize size[3];
    int sourcePlane[3] = {0, 1, 2}; // which frame plane feeds each texture
    int mode = 0;                   // 0 = semi-planar UV, 1 = planar, 2 = semi-planar VU
};

bool layoutFor(QVideoFrameFormat::PixelFormat format, const QSize &s, PlaneLayout &l)
{
    const QSize half((s.width() + 1) / 2, (s.height() + 1) / 2);
    const QSize halfWidth((s.width() + 1) / 2, s.height());
    using R = QRhiTexture;
    switch (format) {
    case QVideoFrameFormat::Format_NV12:
    case QVideoFrameFormat::Format_NV21:
        l = {2, {R::R8, R::RG8}, {s, half}, {0, 1, 2}, format == QVideoFrameFormat::Format_NV21 ? 2 : 0};
        return true;
    case QVideoFrameFormat::Format_P010:
    case QVideoFrameFormat::Format_P016:
        // 10/16-bit samples in the high bits of 16-bit words; normalized they line up with 8-bit maths.
        l = {2, {R::R16, R::RG16}, {s, half}, {0, 1, 2}, 0};
        return true;
    case QVideoFrameFormat::Format_YUV420P:
        l = {3, {R::R8, R::R8, R::R8}, {s, half, half}, {0, 1, 2}, 1};
        return true;
    case QVideoFrameFormat::Format_YV12: // V before U
        l = {3, {R::R8, R::R8, R::R8}, {s, half, half}, {0, 2, 1}, 1};
        return true;
    case QVideoFrameFormat::Format_YUV422P:
        l = {3, {R::R8, R::R8, R::R8}, {s, halfWidth, halfWidth}, {0, 1, 2}, 1};
        return true;
    default:
        return false;
    }
}

// YUV -> RGB for the frame's colour space and range, as a matrix applied to (Y, U, V, 1).
QMatrix4x4 colorMatrix(const QVideoFrameFormat &f)
{
    QVideoFrameFormat::ColorSpace cs = f.colorSpace();
    if (cs == QVideoFrameFormat::ColorSpace_Undefined)
        cs = f.frameHeight() >= 720 ? QVideoFrameFormat::ColorSpace_BT709 : QVideoFrameFormat::ColorSpace_BT601;
    double kr = 0.2126, kb = 0.0722; // BT.709
    if (cs == QVideoFrameFormat::ColorSpace_BT601) {
        kr = 0.299;
        kb = 0.114;
    } else if (cs == QVideoFrameFormat::ColorSpace_BT2020) {
        kr = 0.2627;
        kb = 0.0593;
    }
    const double kg = 1.0 - kr - kb;

    // Video ("limited") range puts black at 16 and white at 235 (chroma 16..240).
    const bool full = f.colorRange() == QVideoFrameFormat::ColorRange_Full;
    const double ys = full ? 1.0 : 255.0 / 219.0;
    const double yo = full ? 0.0 : -16.0 / 255.0 * ys;
    const double cs_ = full ? 1.0 : 255.0 / 224.0;
    const double co = 128.0 / 255.0;

    const double rv = 2.0 * (1.0 - kr) * cs_;
    const double gu = -2.0 * kb * (1.0 - kb) / kg * cs_;
    const double gv = -2.0 * kr * (1.0 - kr) / kg * cs_;
    const double bu = 2.0 * (1.0 - kb) * cs_;
    return QMatrix4x4(float(ys), 0.0f, float(rv), float(yo - rv * co),
                      float(ys), float(gu), float(gv), float(yo - (gu + gv) * co),
                      float(ys), float(bu), 0.0f, float(yo - bu * co),
                      0.0f, 0.0f, 0.0f, 1.0f);
}

// CPU fallback for backends without BC4 (Qt's OpenGL backend): expands the blocks to 8-bit values.
QByteArray decodeBc4(const QByteArray &blocks, const QSize &padded)
{
    QByteArray out(qsizetype(padded.width()) * padded.height(), Qt::Uninitialized);
    const int bw = padded.width() / 4, bh = padded.height() / 4;
    if (blocks.size() < qsizetype(bw) * bh * 8)
        return {};
    const auto *in = reinterpret_cast<const unsigned char *>(blocks.constData());
    auto *dst = reinterpret_cast<unsigned char *>(out.data());
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx, in += 8) {
            const int a0 = in[0], a1 = in[1];
            int pal[8] = {a0, a1};
            if (a0 > a1) {
                for (int k = 1; k < 7; ++k)
                    pal[k + 1] = ((7 - k) * a0 + k * a1) / 7;
            } else {
                for (int k = 1; k < 5; ++k)
                    pal[k + 1] = ((5 - k) * a0 + k * a1) / 5;
                pal[6] = 0;
                pal[7] = 255;
            }
            quint64 bits = 0;
            for (int i = 0; i < 6; ++i)
                bits |= quint64(in[2 + i]) << (8 * i);
            for (int i = 0; i < 16; ++i)
                dst[(by * 4 + i / 4) * padded.width() + bx * 4 + i % 4] = uchar(pal[(bits >> (3 * i)) & 7]);
        }
    }
    return out;
}

QShader loadShader(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QShader::fromSerialized(f.readAll()) : QShader();
}

} // namespace

bool YuvConverter::supports(QVideoFrameFormat::PixelFormat format)
{
    PlaneLayout l;
    return layoutFor(format, QSize(2, 2), l);
}

bool YuvConverter::init(QRhi *rhi)
{
    m_rhi = rhi;
    m_vs = loadShader(QStringLiteral(":/shaders/yuv.vert.qsb"));
    m_fs = loadShader(QStringLiteral(":/shaders/yuv.frag.qsb"));
    m_sampler = rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    return m_vs.isValid() && m_fs.isValid() && m_sampler->create();
}

void YuvConverter::release()
{
    delete m_pipeline;
    m_pipeline = nullptr;
    delete m_sampler;
    m_sampler = nullptr;
}

void YuvConverter::releaseTarget(Target &t, bool deferred)
{
    QRhiResource *resources[] = {t.planes[0], t.planes[1], t.planes[2], t.rgba, t.rt, t.rp, t.srb, t.ubuf};
    for (QRhiResource *r : resources) {
        if (!r)
            continue;
        if (deferred)
            r->deleteLater(); // may still be referenced by frames in flight
        else
            delete r;
    }
    t = Target();
}

bool YuvConverter::ensureTarget(Target &t, const QVideoFrameFormat &format)
{
    const QSize size = format.frameSize();
    if (t.rgba && t.size == size && t.format == format.pixelFormat() && t.hapKind == HapFrame::None)
        return true;
    releaseTarget(t, true);

    PlaneLayout l;
    if (!layoutFor(format.pixelFormat(), size, l))
        return false;
    for (int i = 0; i < l.count; ++i) {
        t.planes[i] = m_rhi->newTexture(l.format[i], l.size[i]);
        if (!t.planes[i]->create())
            return false;
    }
    if (!createOutput(t, size))
        return false;
    t.format = format.pixelFormat();
    return true;
}

bool YuvConverter::ensureHapTarget(Target &t, const HapFrame &frame)
{
    const QSize padded = frame.paddedSize();
    if (t.rgba && t.size == padded && t.hapKind == frame.kind)
        return true;
    releaseTarget(t, true);

    t.planes[0] = m_rhi->newTexture(QRhiTexture::BC3, padded);
    if (!t.planes[0]->create())
        return false;
    if (frame.kind == HapFrame::YCoCgAlpha) {
        const bool bc4 = m_rhi->isTextureFormatSupported(QRhiTexture::BC4);
        t.planes[1] = m_rhi->newTexture(bc4 ? QRhiTexture::BC4 : QRhiTexture::R8, padded);
        if (!t.planes[1]->create())
            return false;
    }
    if (!createOutput(t, padded))
        return false;
    t.hapKind = frame.kind;
    return true;
}

// The RGBA render target, its uniforms and the bindings to whatever input textures exist.
bool YuvConverter::createOutput(Target &t, const QSize &size)
{
    t.rgba = m_rhi->newTexture(QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget);
    if (!t.rgba->create())
        return false;
    t.rt = m_rhi->newTextureRenderTarget({QRhiColorAttachment(t.rgba)});
    t.rp = t.rt->newCompatibleRenderPassDescriptor();
    t.rt->setRenderPassDescriptor(t.rp);
    t.rt->create();
    t.ubuf = m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUniformSize);
    t.ubuf->create();
    // Unused inputs are bound to the first texture so the layout stays the same for every mode.
    QRhiTexture *u = t.planes[1] ? t.planes[1] : t.planes[0];
    QRhiTexture *v = t.planes[2] ? t.planes[2] : u;
    t.srb = m_rhi->newShaderResourceBindings();
    t.srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, t.ubuf),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, t.planes[0], m_sampler),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, u, m_sampler),
        QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage, v, m_sampler),
    });
    t.srb->create();
    t.size = size;
    return true;
}

QRhiGraphicsPipeline *YuvConverter::pipelineFor(QRhiRenderPassDescriptor *rp, QRhiShaderResourceBindings *srb)
{
    // Every target is RGBA8 with the same binding layout, so one pipeline serves them all.
    if (m_pipeline && m_pipelineFormat == rp->serializedFormat())
        return m_pipeline;
    delete m_pipeline;
    m_pipeline = m_rhi->newGraphicsPipeline();
    m_pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    m_pipeline->setShaderStages({{QRhiShaderStage::Vertex, m_vs}, {QRhiShaderStage::Fragment, m_fs}});
    m_pipeline->setVertexInputLayout({});
    m_pipeline->setShaderResourceBindings(srb); // only its layout matters
    m_pipeline->setRenderPassDescriptor(rp);
    if (!m_pipeline->create()) {
        qWarning("YuvConverter: pipeline creation failed");
        delete m_pipeline;
        m_pipeline = nullptr;
        return nullptr;
    }
    m_pipelineFormat = rp->serializedFormat();
    return m_pipeline;
}

void YuvConverter::runPass(QRhiCommandBuffer *cb, Target &t, QRhiResourceUpdateBatch *u, const QMatrix4x4 &matrix,
                           int mode)
{
    char data[kUniformSize];
    std::memset(data, 0, sizeof(data));
    std::memcpy(data, m_rhi->clipSpaceCorrMatrix().constData(), 64);
    std::memcpy(data + 64, matrix.constData(), 64);
    const qint32 modes[4] = {mode, 0, 0, 0};
    std::memcpy(data + 128, modes, sizeof(modes));
    u->updateDynamicBuffer(t.ubuf, 0, kUniformSize, data);

    QRhiGraphicsPipeline *ps = pipelineFor(t.rp, t.srb);
    cb->beginPass(t.rt, Qt::black, {1.0f, 0}, u);
    if (ps) {
        cb->setGraphicsPipeline(ps);
        cb->setViewport(QRhiViewport(0, 0, t.size.width(), t.size.height()));
        cb->setShaderResources(t.srb);
        cb->draw(4);
    }
    cb->endPass();
}

bool YuvConverter::convert(QRhiCommandBuffer *cb, const QVideoFrame &frame, Target &t)
{
    QVideoFrame f = frame;
    if (!f.map(QVideoFrame::ReadOnly))
        return false;
    const QVideoFrameFormat format = f.surfaceFormat();
    PlaneLayout l;
    if (!layoutFor(format.pixelFormat(), format.frameSize(), l) || !ensureTarget(t, format)) {
        f.unmap();
        return false;
    }

    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    const bool strideOk = m_rhi->isFeatureSupported(QRhi::ImageDataStride);
    for (int i = 0; i < l.count; ++i) {
        const int sp = l.sourcePlane[i];
        const int stride = f.bytesPerLine(sp);
        const QSize ps = l.size[i];
        const int bpp = (l.format[i] == QRhiTexture::R8 ? 1 : l.format[i] == QRhiTexture::RG16 ? 4 : 2);
        const int rowBytes = ps.width() * bpp;
        const char *bits = reinterpret_cast<const char *>(f.bits(sp));
        QRhiTextureSubresourceUploadDescription desc;
        if (stride == rowBytes || strideOk) {
            desc = QRhiTextureSubresourceUploadDescription(QByteArray(bits, f.mappedBytes(sp)));
            if (stride != rowBytes)
                desc.setDataStride(quint32(stride));
        } else {
            // Repack padded rows for backends that cannot take a custom stride.
            QByteArray packed(rowBytes * ps.height(), Qt::Uninitialized);
            for (int y = 0; y < ps.height(); ++y)
                std::memcpy(packed.data() + y * rowBytes, bits + y * stride, size_t(rowBytes));
            desc = QRhiTextureSubresourceUploadDescription(packed);
        }
        u->uploadTexture(t.planes[i], QRhiTextureUploadDescription({0, 0, desc}));
    }
    f.unmap();
    runPass(cb, t, u, colorMatrix(format), l.mode);
    return true;
}

bool YuvConverter::convertHap(QRhiCommandBuffer *cb, const HapFrame &frame, Target &t)
{
    if ((frame.kind != HapFrame::YCoCg && frame.kind != HapFrame::YCoCgAlpha) || !ensureHapTarget(t, frame))
        return false;
    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    u->uploadTexture(t.planes[0], QRhiTextureUploadDescription(
                                      {0, 0, QRhiTextureSubresourceUploadDescription(frame.color)}));
    if (frame.kind == HapFrame::YCoCgAlpha) {
        const QByteArray alpha =
            t.planes[1]->format() == QRhiTexture::BC4 ? frame.alpha : decodeBc4(frame.alpha, t.size);
        u->uploadTexture(t.planes[1], QRhiTextureUploadDescription(
                                          {0, 0, QRhiTextureSubresourceUploadDescription(alpha)}));
    }
    runPass(cb, t, u, QMatrix4x4(), frame.kind == HapFrame::YCoCgAlpha ? 4 : 3);
    return true;
}