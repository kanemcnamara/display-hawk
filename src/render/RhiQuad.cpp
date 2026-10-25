#include "render/RhiQuad.h"

#include <QFile>
#include <QImage>
#include <QMatrix4x4>
#include <cstring>

namespace {

constexpr quint32 kUniformSize = 160; // must match the std140 block in quad.vert / quad.frag
constexpr quint32 kMaxDrawsPerFrame = 1024;

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

bool RhiQuad::init(QRhi *rhi)
{
    m_rhi = rhi;
    m_vs = loadShader(QStringLiteral(":/shaders/quad.vert.qsb"));
    m_fs = loadShader(QStringLiteral(":/shaders/quad.frag.qsb"));
    if (!m_vs.isValid() || !m_fs.isValid())
        return false;

    const quint32 align = quint32(rhi->ubufAlignment());
    m_stride = (kUniformSize + align - 1) / align * align;
    m_ubuf = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, m_stride * kMaxDrawsPerFrame);
    m_sampler = rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    m_white = rhi->newTexture(QRhiTexture::RGBA8, QSize(4, 4));
    if (!m_ubuf->create() || !m_sampler->create() || !m_white->create())
        return false;
    m_layoutSrb = srbFor(m_white);
    return m_layoutSrb != nullptr;
}

void RhiQuad::release()
{
    for (auto &p : m_pipelines)
        delete p.second;
    m_pipelines.clear();
    qDeleteAll(m_srbs);
    m_srbs.clear();
    m_layoutSrb = nullptr;
    delete m_white;
    m_white = nullptr;
    delete m_sampler;
    m_sampler = nullptr;
    delete m_ubuf;
    m_ubuf = nullptr;
    m_whiteUploaded = false;
}

void RhiQuad::forgetTexture(QRhiTexture *tex)
{
    if (QRhiShaderResourceBindings *srb = m_srbs.take(tex))
        delete srb;
}

QRhiShaderResourceBindings *RhiQuad::srbFor(QRhiTexture *tex)
{
    if (QRhiShaderResourceBindings *srb = m_srbs.value(tex))
        return srb;
    QRhiShaderResourceBindings *srb = m_rhi->newShaderResourceBindings();
    srb->setBindings({
        QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, m_ubuf,
            kUniformSize),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, tex, m_sampler),
    });
    if (!srb->create()) {
        delete srb;
        return nullptr;
    }
    m_srbs.insert(tex, srb);
    return srb;
}

QRhiGraphicsPipeline *RhiQuad::pipelineFor(QRhiRenderPassDescriptor *rp)
{
    const QVector<quint32> format = rp->serializedFormat();
    for (const auto &p : std::as_const(m_pipelines))
        if (p.first == format)
            return p.second;

    QRhiGraphicsPipeline *ps = m_rhi->newGraphicsPipeline();
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    ps->setTargetBlends({blend});
    ps->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    ps->setFlags(QRhiGraphicsPipeline::UsesScissor);
    ps->setShaderStages({{QRhiShaderStage::Vertex, m_vs}, {QRhiShaderStage::Fragment, m_fs}});
    ps->setVertexInputLayout({}); // positions come from the vertex index
    ps->setShaderResourceBindings(m_layoutSrb);
    ps->setRenderPassDescriptor(rp);
    if (!ps->create()) {
        qWarning("RhiQuad: pipeline creation failed");
        delete ps;
        return nullptr;
    }
    m_pipelines.append({format, ps});
    return ps;
}

void RhiQuad::recordPass(QRhiCommandBuffer *cb, QRhiRenderTarget *rt, const QColor &clear,
                         const QVector<Params> &draws, QRhiResourceUpdateBatch *before,
                         QRhiResourceUpdateBatch *after)
{
    QRhiResourceUpdateBatch *u = before ? before : m_rhi->nextResourceUpdateBatch();
    if (!m_whiteUploaded) {
        QImage white(4, 4, QImage::Format_RGBA8888);
        white.fill(Qt::white);
        u->uploadTexture(m_white, white);
        m_whiteUploaded = true;
    }

    const QMatrix4x4 corr = m_rhi->clipSpaceCorrMatrix();
    QVector<quint32> offsets;
    offsets.reserve(draws.size());
    for (const Params &p : draws) {
        if (m_offset + m_stride > m_ubuf->size()) {
            if (!m_warnedFull)
                qWarning("RhiQuad: more than %u draws in one frame, extra draws skipped", kMaxDrawsPerFrame);
            m_warnedFull = true;
            break;
        }
        char data[kUniformSize];
        std::memset(data, 0, sizeof(data));
        float *fp = reinterpret_cast<float *>(data);
        std::memcpy(fp, corr.constData(), 64);
        const float vecs[] = {float(p.dst.x()),    float(p.dst.y()),    float(p.dst.width()),    float(p.dst.height()),
                              float(p.src.x()),    float(p.src.y()),    float(p.src.width()),    float(p.src.height()),
                              p.color.redF(),      p.color.greenF(),    p.color.blueF(),         p.color.alphaF(),
                              float(p.target.width()), float(p.target.height()), p.opacity, p.brightness};
        std::memcpy(fp + 16, vecs, sizeof(vecs));
        const qint32 ints[] = {p.rotation & 3, p.flipH ? 1 : 0, p.flipV ? 1 : 0, p.texFlipY ? 1 : 0,
                               p.texture ? 1 : 0, 0, 0, 0};
        std::memcpy(data + 128, ints, sizeof(ints));
        u->updateDynamicBuffer(m_ubuf, m_offset, kUniformSize, data);
        offsets.append(m_offset);
        m_offset += m_stride;
    }

    const QSize px = rt->pixelSize();
    cb->beginPass(rt, clear, {1.0f, 0}, u);
    QRhiGraphicsPipeline *ps = pipelineFor(rt->renderPassDescriptor());
    if (ps) {
        cb->setGraphicsPipeline(ps);
        cb->setViewport(QRhiViewport(0, 0, px.width(), px.height()));
        for (int i = 0; i < offsets.size(); ++i) {
            const Params &p = draws[i];
            QRhiShaderResourceBindings *srb = srbFor(p.texture ? p.texture : m_white);
            if (!srb)
                continue;
            // QRhi scissors use a bottom-left origin on every backend.
            QRect clip = p.clip.isEmpty() ? QRect(QPoint(), px) : p.clip.intersected(QRect(QPoint(), px));
            if (clip.isEmpty())
                continue;
            cb->setScissor(QRhiScissor(clip.x(), px.height() - (clip.y() + clip.height()), clip.width(), clip.height()));
            const QRhiCommandBuffer::DynamicOffset off(0, offsets[i]);
            cb->setShaderResources(srb, 1, &off);
            cb->draw(4);
        }
    }
    cb->endPass(after);
}
