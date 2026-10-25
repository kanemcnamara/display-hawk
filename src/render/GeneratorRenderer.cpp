#include "render/GeneratorRenderer.h"

#include "render/Generators.h"

#include <QFile>
#include <QMatrix4x4>
#include <cmath>
#include <cstring>

namespace {

constexpr quint32 kUniformSize = 160; // must match gen.vert / gen.frag
constexpr quint32 kMaxPassesPerFrame = 128;

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

bool GeneratorRenderer::init(QRhi *rhi)
{
    m_rhi = rhi;
    m_vs = loadShader(QStringLiteral(":/shaders/gen.vert.qsb"));
    m_fs = loadShader(QStringLiteral(":/shaders/gen.frag.qsb"));
    if (!m_vs.isValid() || !m_fs.isValid())
        return false;
    const quint32 align = quint32(rhi->ubufAlignment());
    m_stride = (kUniformSize + align - 1) / align * align;
    m_ubuf = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, m_stride * kMaxPassesPerFrame);
    if (!m_ubuf->create())
        return false;
    m_layoutSrb = rhi->newShaderResourceBindings();
    m_layoutSrb->setBindings({QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
        0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, m_ubuf, kUniformSize)});
    return m_layoutSrb->create();
}

void GeneratorRenderer::release()
{
    for (Target &t : m_targets)
        releaseTarget(t, false);
    m_targets.clear();
    delete m_pipeline;
    m_pipeline = nullptr;
    delete m_layoutSrb;
    m_layoutSrb = nullptr;
    delete m_ubuf;
    m_ubuf = nullptr;
}

void GeneratorRenderer::releaseTarget(Target &t, bool deferred)
{
    // Every target shares the one uniform buffer; m_layoutSrb is the only bindings object needed.
    if (t.tex && onTextureRelease)
        onTextureRelease(t.tex);
    if (deferred) {
        if (t.rt) t.rt->deleteLater();
        if (t.rp) t.rp->deleteLater();
        if (t.tex) t.tex->deleteLater();
    } else {
        delete t.rt;
        delete t.rp;
        delete t.tex;
    }
    t = Target();
}

void GeneratorRenderer::collectGarbage(qint64 now, qint64 idleMs)
{
    for (auto it = m_targets.begin(); it != m_targets.end();) {
        if (now - it->lastUsed > idleMs) {
            releaseTarget(*it, true);
            it = m_targets.erase(it);
        } else {
            ++it;
        }
    }
}

QRhiTexture *GeneratorRenderer::render(QRhiCommandBuffer *cb, const Clip &clip, const QSize &size, double timeSec,
                                       qint64 now)
{
    const GeneratorDef *def = generatorDef(clip.generator);
    if (!def || size.isEmpty() || m_offset + m_stride > m_ubuf->size())
        return nullptr;
    Target &t = m_targets[clip.id];
    t.lastUsed = now;
    if (!t.tex || t.tex->pixelSize() != size) {
        releaseTarget(t, true);
        t.lastUsed = now;
        t.tex = m_rhi->newTexture(QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget);
        if (!t.tex->create()) {
            releaseTarget(t, false);
            return nullptr;
        }
        t.rt = m_rhi->newTextureRenderTarget({QRhiColorAttachment(t.tex)});
        t.rp = t.rt->newCompatibleRenderPassDescriptor();
        t.rt->setRenderPassDescriptor(t.rp);
        t.rt->create();
    }
    if (!m_pipeline || m_pipelineFormat != t.rp->serializedFormat()) {
        delete m_pipeline;
        m_pipeline = m_rhi->newGraphicsPipeline();
        m_pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        m_pipeline->setShaderStages({{QRhiShaderStage::Vertex, m_vs}, {QRhiShaderStage::Fragment, m_fs}});
        m_pipeline->setVertexInputLayout({});
        m_pipeline->setShaderResourceBindings(m_layoutSrb);
        m_pipeline->setRenderPassDescriptor(t.rp);
        if (!m_pipeline->create()) {
            delete m_pipeline;
            m_pipeline = nullptr;
            return nullptr;
        }
        m_pipelineFormat = t.rp->serializedFormat();
    }

    char data[kUniformSize];
    std::memset(data, 0, sizeof(data));
    std::memcpy(data, m_rhi->clipSpaceCorrMatrix().constData(), 64);
    const float sizeVec[4] = {float(size.width()), float(size.height()), float(std::fmod(timeSec, 3600.0)), 0.0f};
    std::memcpy(data + 64, sizeVec, sizeof(sizeVec));
    const qint32 mode[4] = {def->shaderMode, 0, 0, 0};
    std::memcpy(data + 80, mode, sizeof(mode));
    float params[8] = {};
    for (int i = 0; i < 8; ++i)
        params[i] = float(generatorParam(clip, i));
    std::memcpy(data + 96, params, sizeof(params));
    const float colours[8] = {clip.solidColor.redF(), clip.solidColor.greenF(), clip.solidColor.blueF(), clip.solidColor.alphaF(),
                              clip.color2.redF(),     clip.color2.greenF(),     clip.color2.blueF(),     clip.color2.alphaF()};
    std::memcpy(data + 128, colours, sizeof(colours));

    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    u->updateDynamicBuffer(m_ubuf, m_offset, kUniformSize, data);
    cb->beginPass(t.rt, Qt::transparent, {1.0f, 0}, u);
    cb->setGraphicsPipeline(m_pipeline);
    cb->setViewport(QRhiViewport(0, 0, size.width(), size.height()));
    const QRhiCommandBuffer::DynamicOffset off(0, m_offset);
    cb->setShaderResources(m_layoutSrb, 1, &off);
    cb->draw(4);
    cb->endPass();
    m_offset += m_stride;
    return t.tex;
}
