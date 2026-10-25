#include "render/IsfRenderer.h"

#include "render/Isf.h"

#include <QDateTime>
#include <QImage>
#include <QMatrix4x4>
#include <cmath>
#include <cstring>
#include <rhi/qshaderbaker.h>

namespace {

constexpr int kMaxInputs = 32;
constexpr quint32 kUniformSize = 64 + 3 * 16 + kMaxInputs * 16; // must match Isf.cpp's block
constexpr quint32 kMaxPassesPerFrame = 128;

QShader bake(const QString &source, QShader::Stage stage, QString *error)
{
    QShaderBaker baker;
    baker.setGeneratedShaderVariants({QShader::StandardShader});
    baker.setGeneratedShaders({{QShader::SpirvShader, QShaderVersion(100)},
                               {QShader::GlslShader, QShaderVersion(330)},
                               {QShader::HlslShader, QShaderVersion(50)},
                               {QShader::MslShader, QShaderVersion(12)}});
    baker.setSourceString(source.toUtf8(), stage);
    const QShader s = baker.bake();
    if (!s.isValid() && error)
        *error = baker.errorMessage();
    return s;
}

} // namespace

bool IsfRenderer::init(QRhi *rhi)
{
    m_rhi = rhi;
    const quint32 align = quint32(rhi->ubufAlignment());
    m_stride = (kUniformSize + align - 1) / align * align;
    m_ubuf = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, m_stride * kMaxPassesPerFrame);
    m_sampler = rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    m_blank = rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1));
    return m_ubuf->create() && m_sampler->create() && m_blank->create();
}

void IsfRenderer::release()
{
    qDeleteAll(m_srbs);
    m_srbs.clear();
    for (Program &p : m_programs) {
        delete p.pipeline;
        delete p.layout;
    }
    m_programs.clear();
    delete m_blank;
    m_blank = nullptr;
    delete m_sampler;
    m_sampler = nullptr;
    delete m_ubuf;
    m_ubuf = nullptr;
    m_blankUploaded = false;
}

void IsfRenderer::forgetTexture(QRhiTexture *tex)
{
    for (auto it = m_srbs.begin(); it != m_srbs.end();) {
        if (it.key().second == tex) {
            it.value()->deleteLater();
            it = m_srbs.erase(it);
        } else {
            ++it;
        }
    }
}

IsfRenderer::Program &IsfRenderer::program(const QString &id)
{
    Program &p = m_programs[id];
    if (p.tried)
        return p;
    p.tried = true;
    const Isf::Info *info = Isf::info(id);
    if (!info) {
        p.error = QStringLiteral("unknown ISF shader");
        return p;
    }
    QString err;
    p.vs = bake(info->vertexSource, QShader::VertexStage, &err);
    if (p.vs.isValid())
        p.fs = bake(info->fragmentSource, QShader::FragmentStage, &err);
    if (!p.vs.isValid() || !p.fs.isValid()) {
        p.error = err;
        qWarning("ISF %s does not compile:\n%s", qPrintable(info->path), qPrintable(err));
    }
    return p;
}

QRhiShaderResourceBindings *IsfRenderer::srbFor(Program &p, const QString &id, QRhiTexture *input)
{
    const auto key = qMakePair(id, input);
    if (QRhiShaderResourceBindings *srb = m_srbs.value(key))
        return srb;
    const Isf::Info *info = Isf::info(id);
    QVector<QRhiShaderResourceBinding> bindings;
    bindings << QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
        0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, m_ubuf, kUniformSize);
    for (const Isf::Input &in : info->inputs) {
        if (in.type != Isf::Input::Image)
            continue;
        // inputImage is the effect's input; other image inputs are not connected yet (blank).
        QRhiTexture *tex = in.name == QLatin1String("inputImage") && input ? input : m_blank;
        bindings << QRhiShaderResourceBinding::sampledTexture(in.binding, QRhiShaderResourceBinding::FragmentStage, tex, m_sampler);
    }
    QRhiShaderResourceBindings *srb = m_rhi->newShaderResourceBindings();
    srb->setBindings(bindings.cbegin(), bindings.cend());
    if (!srb->create()) {
        delete srb;
        return nullptr;
    }
    m_srbs.insert(key, srb);
    if (!p.layout) { // the pipeline's layout: same shape, kept for its lifetime
        p.layout = m_rhi->newShaderResourceBindings();
        p.layout->setBindings(bindings.cbegin(), bindings.cend());
        p.layout->create();
    }
    return srb;
}

bool IsfRenderer::render(QRhiCommandBuffer *cb, QRhiTextureRenderTarget *out, const QString &id,
                         const QVector<double> &params, QRhiTexture *input, double timeSec, bool premultiplyOutput)
{
    Program &p = program(id);
    const Isf::Info *info = Isf::info(id);
    if (!p.error.isEmpty() || !info || m_offset + m_stride > m_ubuf->size())
        return false;
    QRhiShaderResourceBindings *srb = srbFor(p, id, input);
    if (!srb)
        return false;
    QRhiRenderPassDescriptor *rp = out->renderPassDescriptor();
    if (!p.pipeline || p.format != rp->serializedFormat()) {
        delete p.pipeline;
        p.pipeline = m_rhi->newGraphicsPipeline();
        p.pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        p.pipeline->setShaderStages({{QRhiShaderStage::Vertex, p.vs}, {QRhiShaderStage::Fragment, p.fs}});
        p.pipeline->setVertexInputLayout({});
        p.pipeline->setShaderResourceBindings(p.layout);
        p.pipeline->setRenderPassDescriptor(rp);
        if (!p.pipeline->create()) {
            delete p.pipeline;
            p.pipeline = nullptr;
            p.error = QStringLiteral("pipeline creation failed");
            return false;
        }
        p.format = rp->serializedFormat();
    }

    const QSize size = out->pixelSize();
    char data[kUniformSize];
    std::memset(data, 0, sizeof(data));
    float *f = reinterpret_cast<float *>(data);
    std::memcpy(f, m_rhi->clipSpaceCorrMatrix().constData(), 64);
    const double time = std::fmod(timeSec, 86400.0);
    f[16] = float(size.width());
    f[17] = float(size.height());
    f[18] = float(time);
    f[19] = float(p.lastTime < 0 ? 0.0 : qMax(0.0, time - p.lastTime));
    f[20] = float(p.frames++);
    f[21] = m_rhi->isYUpInFramebuffer() ? 1.0f : 0.0f;
    f[22] = premultiplyOutput ? 1.0f : 0.0f;
    const QDateTime now = QDateTime::currentDateTime();
    f[24] = float(now.date().year());
    f[25] = float(now.date().month());
    f[26] = float(now.date().day());
    f[27] = float(now.time().msecsSinceStartOfDay() / 1000.0);
    p.lastTime = time;
    // Inputs: each in its own vec4, from the parameter values.
    for (int i = 0; i < info->inputs.size() && i < kMaxInputs; ++i) {
        const Isf::Input &in = info->inputs[i];
        float *slot = f + 28 + i * 4;
        auto param = [&](int k, double fallback) {
            const int idx = in.firstParam + k;
            return float(idx >= 0 && idx < params.size() ? params[idx] : fallback);
        };
        switch (in.type) {
        case Isf::Input::Float:
        case Isf::Input::Bool:
        case Isf::Input::Event:
            slot[0] = param(0, in.def.x());
            break;
        case Isf::Input::Long: { // the parameter is the choice's index
            const int index = qRound(param(0, 0));
            slot[0] = in.values.isEmpty() ? float(index) : float(in.values.value(index, in.values.value(0)));
            break;
        }
        case Isf::Input::Color:
            for (int k = 0; k < 4; ++k)
                slot[k] = param(k, in.def[k]);
            break;
        case Isf::Input::Point2D:
            slot[0] = param(0, in.def.x());
            slot[1] = param(1, in.def.y());
            break;
        case Isf::Input::Image:
            break;
        }
    }

    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    if (!m_blankUploaded) {
        QImage blank(1, 1, QImage::Format_RGBA8888);
        blank.fill(Qt::transparent);
        u->uploadTexture(m_blank, blank);
        m_blankUploaded = true;
    }
    u->updateDynamicBuffer(m_ubuf, m_offset, kUniformSize, data);
    cb->beginPass(out, Qt::transparent, {1.0f, 0}, u);
    cb->setGraphicsPipeline(p.pipeline);
    cb->setViewport(QRhiViewport(0, 0, size.width(), size.height()));
    const QRhiCommandBuffer::DynamicOffset off(0, m_offset);
    cb->setShaderResources(srb, 1, &off);
    cb->draw(4);
    cb->endPass();
    m_offset += m_stride;
    return true;
}
