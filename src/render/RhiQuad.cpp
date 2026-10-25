#include "render/RhiQuad.h"

#include <QFile>
#include <QImage>
#include <QMatrix4x4>
#include <QVector4D>
#include <QtMath>
#include <cstring>

namespace {

constexpr quint32 kUniformSize = 304;
constexpr quint32 kMeshBufferBytes = 16 * 1024 * 1024; // mesh-warped slice vertices per frame // must match the std140 block in quad.vert / quad.frag
constexpr quint32 kMaxDrawsPerFrame = 8192; // cabinet layouts add a slice per cabinet

QShader loadShader(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning("Missing shader %s", qPrintable(path));
        return {};
    }
    return QShader::fromSerialized(f.readAll());
}

// Perspective-correct texturing of a quad from two triangles: each corner's weight comes from where
// the diagonals cross (q = (d + d_opposite) / d_opposite). Returns tl, tr, br, bl; 1s if degenerate.
QVector4D projectiveWeights(const QPointF &tl, const QPointF &tr, const QPointF &br, const QPointF &bl)
{
    const QPointF r = br - tl, s = bl - tr;
    const double denom = r.x() * s.y() - r.y() * s.x();
    if (qAbs(denom) < 1e-9)
        return QVector4D(1, 1, 1, 1);
    const QPointF d = tr - tl;
    const double t = (d.x() * s.y() - d.y() * s.x()) / denom;
    const double u = (d.x() * r.y() - d.y() * r.x()) / denom;
    if (t <= 0 || t >= 1 || u <= 0 || u >= 1) // not convex
        return QVector4D(1, 1, 1, 1);
    const QPointF c = tl + r * t;
    auto len = [](const QPointF &v) { return qSqrt(v.x() * v.x() + v.y() * v.y()); };
    const double d0 = len(tl - c), d1 = len(tr - c), d2 = len(br - c), d3 = len(bl - c);
    return QVector4D(float((d0 + d2) / d2), float((d1 + d3) / d3), float((d2 + d0) / d0), float((d3 + d1) / d1));
}

} // namespace

bool RhiQuad::init(QRhi *rhi)
{
    m_rhi = rhi;
    m_vs = loadShader(QStringLiteral(":/shaders/quad.vert.qsb"));
    m_fs = loadShader(QStringLiteral(":/shaders/quad.frag.qsb"));
    m_meshVs = loadShader(QStringLiteral(":/shaders/mesh.vert.qsb"));
    if (!m_vs.isValid() || !m_fs.isValid() || !m_meshVs.isValid())
        return false;

    const quint32 align = quint32(rhi->ubufAlignment());
    m_stride = (kUniformSize + align - 1) / align * align;
    m_ubuf = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, m_stride * kMaxDrawsPerFrame);
    m_sampler = rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    m_white = rhi->newTexture(QRhiTexture::RGBA8, QSize(4, 4));
    m_vbuf = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, kMeshBufferBytes);
    if (!m_ubuf->create() || !m_sampler->create() || !m_white->create() || !m_vbuf->create())
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
    delete m_vbuf;
    m_vbuf = nullptr;
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

QRhiGraphicsPipeline *RhiQuad::pipelineFor(QRhiRenderPassDescriptor *rp, Blend mode, bool mesh)
{
    const PipelineKey key{rp->serializedFormat(), mode, mesh};
    for (const auto &p : std::as_const(m_pipelines))
        if (p.first == key)
            return p.second;

    using P = QRhiGraphicsPipeline;
    QRhiGraphicsPipeline *ps = m_rhi->newGraphicsPipeline();
    P::TargetBlend blend;
    blend.enable = true;
    // Layer modes composite premultiplied colour onto the opaque composition and leave its alpha alone.
    blend.srcAlpha = P::Zero;
    blend.dstAlpha = P::One;
    switch (mode) {
    case Blend::Straight:
        blend.srcColor = P::SrcAlpha;
        blend.dstColor = P::OneMinusSrcAlpha;
        blend.srcAlpha = P::One;
        blend.dstAlpha = P::OneMinusSrcAlpha;
        break;
    case Blend::Normal:
        blend.srcColor = P::One;
        blend.dstColor = P::OneMinusSrcAlpha;
        break;
    case Blend::Add:
        blend.srcColor = P::One;
        blend.dstColor = P::One;
        break;
    case Blend::Screen: // src + dst * (1 - src)
        blend.srcColor = P::One;
        blend.dstColor = P::OneMinusSrcColor;
        break;
    case Blend::Multiply: // dst * (1 - a + a * src)
        blend.srcColor = P::DstColor;
        blend.dstColor = P::OneMinusSrcAlpha;
        break;
    case Blend::Lighten:
        blend.srcColor = P::One;
        blend.dstColor = P::One;
        blend.opColor = P::Max;
        break;
    case Blend::Darken:
        blend.srcColor = P::One;
        blend.dstColor = P::One;
        blend.opColor = P::Min;
        break;
    }
    ps->setTargetBlends({blend});
    ps->setFlags(QRhiGraphicsPipeline::UsesScissor);
    if (mesh) {
        ps->setTopology(QRhiGraphicsPipeline::Triangles);
        ps->setShaderStages({{QRhiShaderStage::Vertex, m_meshVs}, {QRhiShaderStage::Fragment, m_fs}});
        QRhiVertexInputLayout layout;
        layout.setBindings({{4 * sizeof(float)}});
        layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float4, 0}});
        ps->setVertexInputLayout(layout);
    } else {
        ps->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        ps->setShaderStages({{QRhiShaderStage::Vertex, m_vs}, {QRhiShaderStage::Fragment, m_fs}});
        ps->setVertexInputLayout({}); // positions come from the vertex index
    }
    ps->setShaderResourceBindings(m_layoutSrb);
    ps->setRenderPassDescriptor(rp);
    if (!ps->create()) {
        qWarning("RhiQuad: pipeline creation failed");
        delete ps;
        return nullptr;
    }
    m_pipelines.append({key, ps});
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
    QVector<QPair<quint32, quint32>> meshes(draws.size(), {0, 0}); // vertex buffer offset, vertex count
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
        const qint32 inputMode = p.blend == Blend::Straight ? 0 : (p.blend == Blend::Darken ? 2 : 1);
        const qint32 ints[] = {p.rotation & 3, p.flipH ? 1 : 0, p.flipV ? 1 : 0, p.texFlipY ? 1 : 0,
                               p.texture ? 1 : 0, inputMode, p.srcAxes ? 1 : 0, p.quad.size() == 4 ? 1 : 0};
        std::memcpy(data + 128, ints, sizeof(ints));
        const float grade[] = {p.gain[0], p.gain[1], p.gain[2], 1.0f, p.gamma, p.blackLevel, p.colorCorrect ? 1.0f : 0.0f, 0.0f};
        std::memcpy(data + 160, grade, sizeof(grade));
        const float axes[] = {float(p.srcU.x()), float(p.srcU.y()), float(p.srcV.x()), float(p.srcV.y())};
        std::memcpy(data + 192, axes, sizeof(axes));
        const float pivot[] = {float(p.pivot.x()), float(p.pivot.y()), float(qDegreesToRadians(p.dstAngle)), 0.0f};
        std::memcpy(data + 208, pivot, sizeof(pivot));
        if (p.quad.size() == 4) {
            // Strip order: top-left, top-right, bottom-left, bottom-right.
            const QPointF tl = p.quad[0], tr = p.quad[1], br = p.quad[2], bl = p.quad[3];
            const float corners[] = {float(tl.x()), float(tl.y()), float(tr.x()), float(tr.y()),
                                     float(bl.x()), float(bl.y()), float(br.x()), float(br.y())};
            std::memcpy(data + 224, corners, sizeof(corners));
            const QVector4D q = projectiveWeights(tl, tr, br, bl);
            const float weights[] = {q.x(), q.y(), q.w(), q.z()}; // tl, tr, bl, br
            std::memcpy(data + 256, weights, sizeof(weights));
        }
        if (p.edgeBlend) {
            const float blend[] = {p.edgeWidths[0], p.edgeWidths[1], p.edgeWidths[2], p.edgeWidths[3],
                                   qMax(0.1f, p.blendCurve), 1.0f / qMax(0.1f, p.blendGamma), 1.0f, 0.0f};
            std::memcpy(data + 272, blend, sizeof(blend));
        }
        if (!p.mesh.isEmpty()) {
            const quint32 bytes = quint32(p.mesh.size() * sizeof(QVector4D));
            if (m_vOffset + bytes <= kMeshBufferBytes) {
                u->updateDynamicBuffer(m_vbuf, m_vOffset, bytes, p.mesh.constData());
                meshes[offsets.size()] = {m_vOffset, quint32(p.mesh.size())};
                m_vOffset += bytes;
            } else {
                qWarning("RhiQuad: mesh vertex buffer full, mesh drawn unwarped");
            }
        }
        u->updateDynamicBuffer(m_ubuf, m_offset, kUniformSize, data);
        offsets.append(m_offset);
        m_offset += m_stride;
    }

    const QSize px = rt->pixelSize();
    cb->beginPass(rt, clear, {1.0f, 0}, u);
    {
        QRhiGraphicsPipeline *bound = nullptr;
        for (int i = 0; i < offsets.size(); ++i) {
            const Params &p = draws[i];
            const bool mesh = meshes[i].second > 0;
            QRhiGraphicsPipeline *ps = pipelineFor(rt->renderPassDescriptor(), p.blend, mesh);
            if (!ps)
                continue;
            if (ps != bound) {
                cb->setGraphicsPipeline(ps);
                cb->setViewport(QRhiViewport(0, 0, px.width(), px.height()));
                bound = ps;
            }
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
            if (mesh) {
                const QRhiCommandBuffer::VertexInput input(m_vbuf, meshes[i].first);
                cb->setVertexInput(0, 1, &input);
                cb->draw(meshes[i].second);
            } else {
                cb->draw(4);
            }
        }
    }
    cb->endPass(after);
}
