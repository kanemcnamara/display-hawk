#include "render/BlendCompositor.h"

#include <QFile>
#include <QMatrix4x4>
#include <cstring>

namespace {

constexpr quint32 kUniformSize = 80; // must match blend.vert / blend.frag
constexpr quint32 kMaxPassesPerFrame = 256;

QShader loadShader(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning("Missing shader %s", qPrintable(path));
        return {};
    }
    return QShader::fromSerialized(f.readAll());
}

} // namespace

bool BlendCompositor::init(QRhi *rhi)
{
    m_rhi = rhi;
    m_vs = loadShader(QStringLiteral(":/shaders/blend.vert.qsb"));
    m_fs = loadShader(QStringLiteral(":/shaders/blend.frag.qsb"));
    if (!m_vs.isValid() || !m_fs.isValid())
        return false;
    const quint32 align = quint32(rhi->ubufAlignment());
    m_stride = (kUniformSize + align - 1) / align * align;
    m_ubuf = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, m_stride * kMaxPassesPerFrame);
    m_sampler = rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                                QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge); // same-size textures: 1:1
    m_dummy = rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1));
    if (!m_ubuf->create() || !m_sampler->create() || !m_dummy->create())
        return false;
    m_layoutSrb = m_rhi->newShaderResourceBindings();
    const auto frag = QRhiShaderResourceBinding::FragmentStage;
    m_layoutSrb->setBindings({
        QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(0, QRhiShaderResourceBinding::VertexStage | frag, m_ubuf, kUniformSize),
        QRhiShaderResourceBinding::sampledTexture(1, frag, m_dummy, m_sampler),
        QRhiShaderResourceBinding::sampledTexture(2, frag, m_dummy, m_sampler),
    });
    return m_layoutSrb->create();
}

void BlendCompositor::release()
{
    qDeleteAll(m_srbs);
    m_srbs.clear();
    delete m_layoutSrb;
    m_layoutSrb = nullptr;
    delete m_pipeline;
    m_pipeline = nullptr;
    delete m_dummy;
    m_dummy = nullptr;
    delete m_sampler;
    m_sampler = nullptr;
    delete m_ubuf;
    m_ubuf = nullptr;
}

void BlendCompositor::forgetTexture(QRhiTexture *tex)
{
    for (auto it = m_srbs.begin(); it != m_srbs.end();) {
        if (it.key().first == tex || it.key().second == tex) {
            it.value()->deleteLater();
            it = m_srbs.erase(it);
        } else {
            ++it;
        }
    }
}

QRhiShaderResourceBindings *BlendCompositor::srbFor(QRhiTexture *base, QRhiTexture *layer)
{
    const auto key = qMakePair(base, layer);
    if (QRhiShaderResourceBindings *srb = m_srbs.value(key))
        return srb;
    QRhiShaderResourceBindings *srb = m_rhi->newShaderResourceBindings();
    const auto frag = QRhiShaderResourceBinding::FragmentStage;
    srb->setBindings({
        QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(0, QRhiShaderResourceBinding::VertexStage | frag, m_ubuf, kUniformSize),
        QRhiShaderResourceBinding::sampledTexture(1, frag, base, m_sampler),
        QRhiShaderResourceBinding::sampledTexture(2, frag, layer, m_sampler),
    });
    if (!srb->create()) {
        delete srb;
        return nullptr;
    }
    m_srbs.insert(key, srb);
    return srb;
}

QRhiGraphicsPipeline *BlendCompositor::pipelineFor(QRhiRenderPassDescriptor *rp)
{
    if (m_pipeline && m_pipelineFormat == rp->serializedFormat())
        return m_pipeline;
    delete m_pipeline;
    m_pipeline = m_rhi->newGraphicsPipeline();
    m_pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    m_pipeline->setShaderStages({{QRhiShaderStage::Vertex, m_vs}, {QRhiShaderStage::Fragment, m_fs}});
    m_pipeline->setVertexInputLayout({});
    m_pipeline->setShaderResourceBindings(m_layoutSrb);
    m_pipeline->setRenderPassDescriptor(rp);
    if (!m_pipeline->create()) {
        qWarning("BlendCompositor: pipeline creation failed");
        delete m_pipeline;
        m_pipeline = nullptr;
        return nullptr;
    }
    m_pipelineFormat = rp->serializedFormat();
    return m_pipeline;
}

void BlendCompositor::blend(QRhiCommandBuffer *cb, QRhiTextureRenderTarget *out, QRhiTexture *base,
                            QRhiTexture *layer, BlendMode mode, float opacity, bool opaqueBase)
{
    if (m_offset + m_stride > m_ubuf->size()) {
        qWarning("BlendCompositor: too many blend passes in one frame");
        return;
    }
    QRhiGraphicsPipeline *ps = pipelineFor(out->renderPassDescriptor());
    QRhiShaderResourceBindings *srb = srbFor(base, layer);
    if (!ps || !srb)
        return;
    char data[kUniformSize];
    std::memset(data, 0, sizeof(data));
    std::memcpy(data, m_rhi->clipSpaceCorrMatrix().constData(), 64);
    const float params[4] = {opacity, float(int(mode)), m_rhi->isYUpInFramebuffer() ? 1.0f : 0.0f, opaqueBase ? 1.0f : 0.0f};
    std::memcpy(data + 64, params, sizeof(params));
    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    u->updateDynamicBuffer(m_ubuf, m_offset, kUniformSize, data);
    const QSize px = out->pixelSize();
    cb->beginPass(out, Qt::black, {1.0f, 0}, u);
    cb->setGraphicsPipeline(ps);
    cb->setViewport(QRhiViewport(0, 0, px.width(), px.height()));
    const QRhiCommandBuffer::DynamicOffset off(0, m_offset);
    cb->setShaderResources(srb, 1, &off);
    cb->draw(4);
    cb->endPass();
    m_offset += m_stride;
}
