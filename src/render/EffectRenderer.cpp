#include "render/EffectRenderer.h"

#include "render/Effects.h"
#include "render/IsfRenderer.h"

#include <QDataStream>
#include <QFile>
#include <QImage>
#include <QMatrix4x4>
#include <QPainter>
#include <QRandomGenerator>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr quint32 kUniformSize = 128; // must match the std140 block in fx.vert / fx.frag
constexpr quint32 kMaxPassesPerFrame = 512;
constexpr int kTableWidth = 4096;
constexpr int kTableRows = 3; // texels per panel, stacked: order, centre + size, rotation

QShader loadShader(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning("Missing shader %s", qPrintable(path));
        return {};
    }
    return QShader::fromSerialized(f.readAll());
}

struct PanelEntry {
    QPointF centre;   // in the composition (after the wall's rotation)
    QSizeF size;
    double angle = 0; // the wall's rotation, degrees clockwise
    QTransform transform;
    QRectF local;     // the panel before rotation
    float order[4] = {0, 0, 0, 0}; // left-right, top-bottom, by number, random
};

// Every fitted panel of every wall, in wall order and row-major within a wall (the panel map numbering).
QVector<PanelEntry> collectPanels(const Project &p)
{
    QVector<PanelEntry> panels;
    for (const LedWall &w : p.walls) {
        const QTransform t = w.transform();
        for (int r = 0; r < w.rows; ++r)
            for (int c = 0; c < w.cols; ++c)
                if (w.panelEnabled(c, r)) {
                    const QRectF local(w.panelRect(c, r));
                    panels.append({t.map(local.center()), local.size(), w.angle, t, local, {}});
                }
    }
    const int n = panels.size();
    if (n == 0)
        return panels;

    // Left-right and top-bottom follow the panel centres, so panels in the same column (row) light
    // together, gaps in odd layouts keep their physical spacing and rotated walls sweep as they hang.
    // The span includes one panel, so the last column is not the same phase as the first.
    double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18, sumW = 0, sumH = 0;
    for (const PanelEntry &e : std::as_const(panels)) {
        minX = qMin(minX, e.centre.x());
        maxX = qMax(maxX, e.centre.x());
        minY = qMin(minY, e.centre.y());
        maxY = qMax(maxY, e.centre.y());
        sumW += e.size.width();
        sumH += e.size.height();
    }
    const double spanX = maxX - minX + sumW / n, spanY = maxY - minY + sumH / n;
    QVector<int> shuffled(n);
    for (int i = 0; i < n; ++i)
        shuffled[i] = i;
    QRandomGenerator rng(0x44480F5); // fixed: the same wall always gets the same random order
    for (int i = n - 1; i > 0; --i)
        std::swap(shuffled[i], shuffled[int(rng.bounded(i + 1))]);
    for (int i = 0; i < n; ++i) {
        PanelEntry &e = panels[i];
        e.order[0] = float((e.centre.x() - minX) / spanX);
        e.order[1] = float((e.centre.y() - minY) / spanY);
        e.order[2] = float(i) / float(n);
        e.order[3] = float(shuffled[i]) / float(n);
    }
    return panels;
}

} // namespace

bool EffectRenderer::init(QRhi *rhi)
{
    m_rhi = rhi;
    m_vs = loadShader(QStringLiteral(":/shaders/fx.vert.qsb"));
    m_fs = loadShader(QStringLiteral(":/shaders/fx.frag.qsb"));
    if (!m_vs.isValid() || !m_fs.isValid())
        return false;
    const quint32 align = quint32(rhi->ubufAlignment());
    m_stride = (kUniformSize + align - 1) / align * align;
    m_ubuf = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, m_stride * kMaxPassesPerFrame);
    m_linear = rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                               QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    m_nearest = rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                                QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    // Placeholders until a project with walls arrives: no panels anywhere.
    m_placeholderId = rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1));
    m_placeholderTable = rhi->newTexture(QRhiTexture::RGBA32F, QSize(1, kTableRows));
    if (!m_ubuf->create() || !m_linear->create() || !m_nearest->create() || !m_placeholderId->create()
        || !m_placeholderTable->create())
        return false;
    m_pendingUpload = rhi->nextResourceUpdateBatch();
    QImage none(1, 1, QImage::Format_RGBA8888);
    none.fill(Qt::transparent);
    m_pendingUpload->uploadTexture(m_placeholderId, none);
    const float zeros[4 * kTableRows] = {};
    m_pendingUpload->uploadTexture(m_placeholderTable,
                                   QRhiTextureUploadEntry(0, 0, QRhiTextureSubresourceUploadDescription(zeros, sizeof(zeros))));
    m_layoutSrb = newSrb(m_placeholderId, m_placeholderId, m_placeholderTable, m_placeholderId);
    return m_layoutSrb != nullptr;
}

void EffectRenderer::release()
{
    for (Chain &c : m_chains)
        releaseChain(c, false);
    m_chains.clear();
    if (m_pendingUpload) {
        m_pendingUpload->release();
        m_pendingUpload = nullptr;
    }
    qDeleteAll(m_srbs);
    m_srbs.clear();
    delete m_layoutSrb;
    m_layoutSrb = nullptr;
    delete m_pipeline;
    m_pipeline = nullptr;
    delete m_panelId;
    m_panelId = nullptr;
    delete m_panelTable;
    m_panelTable = nullptr;
    delete m_placeholderId;
    m_placeholderId = nullptr;
    delete m_placeholderTable;
    m_placeholderTable = nullptr;
    m_panelKey.clear();
    m_panelCount = 0;
    delete m_linear;
    m_linear = nullptr;
    delete m_nearest;
    m_nearest = nullptr;
    delete m_ubuf;
    m_ubuf = nullptr;
}

void EffectRenderer::clearSrbs()
{
    // Frames in flight may still use them.
    for (QRhiShaderResourceBindings *srb : std::as_const(m_srbs))
        srb->deleteLater();
    m_srbs.clear();
}

void EffectRenderer::forgetTexture(QRhiTexture *tex)
{
    if (m_isf)
        m_isf->forgetTexture(tex);
    for (auto it = m_srbs.begin(); it != m_srbs.end();) {
        if (it.key().first == tex || it.key().second == tex) {
            it.value()->deleteLater();
            it = m_srbs.erase(it);
        } else {
            ++it;
        }
    }
}

QRhiShaderResourceBindings *EffectRenderer::newSrb(QRhiTexture *tex, QRhiTexture *ids, QRhiTexture *table,
                                                   QRhiTexture *history)
{
    QRhiShaderResourceBindings *srb = m_rhi->newShaderResourceBindings();
    const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
    const auto frag = QRhiShaderResourceBinding::FragmentStage;
    srb->setBindings({
        QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(0, stages, m_ubuf, kUniformSize),
        QRhiShaderResourceBinding::sampledTexture(1, frag, tex, m_linear),
        QRhiShaderResourceBinding::sampledTexture(2, frag, ids, m_nearest),
        QRhiShaderResourceBinding::sampledTexture(3, frag, table, m_nearest),
        QRhiShaderResourceBinding::sampledTexture(4, frag, history, m_nearest),
    });
    if (!srb->create()) {
        delete srb;
        return nullptr;
    }
    return srb;
}

QRhiShaderResourceBindings *EffectRenderer::srbFor(QRhiTexture *tex, QRhiTexture *history)
{
    const auto key = qMakePair(tex, history);
    if (QRhiShaderResourceBindings *srb = m_srbs.value(key))
        return srb;
    QRhiShaderResourceBindings *srb = newSrb(tex, m_panelId ? m_panelId : m_placeholderId,
                                             m_panelTable ? m_panelTable : m_placeholderTable,
                                             history ? history : m_placeholderId);
    if (srb)
        m_srbs.insert(key, srb);
    return srb;
}

QRhiTexture *EffectRenderer::historyFor(Chain &c, int effect, const QSize &size, QRhiResourceUpdateBatch *u)
{
    QRhiTexture *&h = c.history[effect];
    if (h && h->pixelSize() == size)
        return h;
    if (h) {
        forgetTexture(h);
        h->deleteLater();
    }
    h = m_rhi->newTexture(QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
    if (!h->create()) {
        delete h;
        h = nullptr;
        return nullptr;
    }
    QImage clear(size, QImage::Format_RGBA8888); // trails start from nothing
    clear.fill(Qt::transparent);
    u->uploadTexture(h, clear);
    return h;
}

void EffectRenderer::syncPanels(const Project &project, const QSize &compSize)
{
    QByteArray key;
    {
        QDataStream ds(&key, QIODevice::WriteOnly);
        ds << compSize;
        for (const LedWall &w : project.walls) {
            ds << w.x << w.y << w.cols << w.rows << w.panelPxW << w.panelPxH << w.angle;
            for (const QPoint &d : w.disabledPanels)
                ds << d;
            ds << qint32(-1);
        }
    }
    if (key == m_panelKey)
        return;
    m_panelKey = key;

    const QVector<PanelEntry> panels = collectPanels(project);
    m_panelCount = panels.size();

    // Panel index + 1 at every composition pixel, 24 bits over R, G and B.
    QImage ids(compSize.expandedTo(QSize(1, 1)), QImage::Format_RGBA8888);
    ids.fill(Qt::transparent);
    {
        QPainter painter(&ids); // no antialiasing: ids must stay exact
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.setPen(Qt::NoPen);
        for (int i = 0; i < panels.size(); ++i) {
            const int v = i + 1;
            painter.setTransform(panels[i].transform);
            painter.fillRect(panels[i].local, QColor(v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, 255));
        }
    }
    const int width = qBound(1, int(panels.size()), kTableWidth);
    const int rows = qMax(1, int((panels.size() + width - 1) / width));
    QByteArray table(width * rows * kTableRows * 4 * int(sizeof(float)), 0);
    float *f = reinterpret_cast<float *>(table.data());
    for (int i = 0; i < panels.size(); ++i) {
        const PanelEntry &e = panels[i];
        const int x = i % width, y = (i / width) * kTableRows;
        float *order = f + (y * width + x) * 4;
        float *box = f + ((y + 1) * width + x) * 4;
        float *rot = f + ((y + 2) * width + x) * 4;
        std::memcpy(order, e.order, sizeof(e.order));
        box[0] = float(e.centre.x());
        box[1] = float(e.centre.y());
        box[2] = float(e.size.width());
        box[3] = float(e.size.height());
        rot[0] = float(qCos(qDegreesToRadians(e.angle)));
        rot[1] = float(qSin(qDegreesToRadians(e.angle)));
    }

    // New textures (sizes change); every bindings object refers to the old ones.
    clearSrbs();
    if (m_panelId)
        m_panelId->deleteLater();
    if (m_panelTable)
        m_panelTable->deleteLater();
    m_panelId = m_rhi->newTexture(QRhiTexture::RGBA8, ids.size());
    m_panelTable = m_rhi->newTexture(QRhiTexture::RGBA32F, QSize(width, rows * kTableRows));
    if (!m_panelId->create() || !m_panelTable->create()) {
        qWarning("EffectRenderer: could not create the panel textures");
        delete m_panelId;
        delete m_panelTable;
        m_panelId = m_panelTable = nullptr;
        m_panelCount = 0;
        return;
    }
    if (!m_pendingUpload)
        m_pendingUpload = m_rhi->nextResourceUpdateBatch();
    m_pendingUpload->uploadTexture(m_panelId, ids);
    m_pendingUpload->uploadTexture(m_panelTable,
                                   QRhiTextureUploadEntry(0, 0, QRhiTextureSubresourceUploadDescription(table)));
}

QRhiGraphicsPipeline *EffectRenderer::pipelineFor(QRhiRenderPassDescriptor *rp)
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
        qWarning("EffectRenderer: pipeline creation failed");
        delete m_pipeline;
        m_pipeline = nullptr;
        return nullptr;
    }
    m_pipelineFormat = rp->serializedFormat();
    return m_pipeline;
}

bool EffectRenderer::ensureChain(Chain &c, const QSize &size)
{
    if (c.tex[0] && c.size == size)
        return true;
    releaseChain(c, true);
    for (int i = 0; i < 2; ++i) {
        // Copied from by trails (history), so also a transfer source.
        c.tex[i] = m_rhi->newTexture(QRhiTexture::RGBA8, size, 1,
                                     QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
        if (!c.tex[i]->create()) {
            releaseChain(c, false);
            return false;
        }
        c.rt[i] = m_rhi->newTextureRenderTarget({QRhiColorAttachment(c.tex[i])});
        if (!c.rp)
            c.rp = c.rt[i]->newCompatibleRenderPassDescriptor();
        c.rt[i]->setRenderPassDescriptor(c.rp);
        c.rt[i]->create();
    }
    c.size = size;
    return true;
}

void EffectRenderer::releaseChain(Chain &c, bool deferred)
{
    for (int i = 0; i < 2; ++i) {
        if (c.tex[i])
            forgetTexture(c.tex[i]);
        if (deferred) {
            if (c.rt[i])
                c.rt[i]->deleteLater();
            if (c.tex[i])
                c.tex[i]->deleteLater();
        } else {
            delete c.rt[i];
            delete c.tex[i];
        }
    }
    for (QRhiTexture *h : std::as_const(c.history)) {
        forgetTexture(h);
        if (deferred)
            h->deleteLater();
        else
            delete h;
    }
    if (deferred && c.rp)
        c.rp->deleteLater();
    else
        delete c.rp;
    c = Chain();
}

void EffectRenderer::collectGarbage(qint64 now, qint64 idleMs)
{
    for (auto it = m_chains.begin(); it != m_chains.end();) {
        if (now - it->lastUsed > idleMs) {
            releaseChain(*it, true);
            it = m_chains.erase(it);
        } else {
            ++it;
        }
    }
}

QRhiTexture *EffectRenderer::apply(QRhiCommandBuffer *cb, const QString &key, QRhiTexture *input, const QSize &size,
                                   const QVector<EffectInstance> &chain, const QVector<Override> &overrides,
                                   double timeSec)
{
    struct Pass {
        const EffectDef *def;
        int index;
        int pass;
    };
    QVector<Pass> passes;
    bool usesPanels = false;
    for (int i = 0; i < chain.size(); ++i) {
        const EffectInstance &e = chain[i];
        const EffectDef *def = e.enabled ? effectDef(e.type) : nullptr;
        if (!def)
            continue;
        usesPanels = usesPanels || def->usesPanels;
        for (int pass = 0; pass < def->passes; ++pass)
            passes.append({def, i, pass});
    }
    if (passes.isEmpty() || !input)
        return input;

    Chain &c = m_chains[key];
    c.lastUsed = m_now;
    if (!ensureChain(c, size))
        return input;
    QRhiGraphicsPipeline *ps = pipelineFor(c.rp);
    if (!ps)
        return input;
    if (usesPanels && m_project)
        syncPanels(*m_project, size); // uploaded by the first pass below

    const QMatrix4x4 corr = m_rhi->clipSpaceCorrMatrix();
    const float flipY = m_rhi->isYUpInFramebuffer() ? 1.0f : 0.0f;
    const float time = float(std::fmod(timeSec, 3600.0));
    QRhiTexture *current = input;
    int ping = 0;
    for (const Pass &pass : std::as_const(passes)) {
        if (m_offset + m_stride > m_ubuf->size()) {
            if (!m_warnedFull)
                qWarning("EffectRenderer: more than %u effect passes in one frame, extra passes skipped",
                         kMaxPassesPerFrame);
            m_warnedFull = true;
            break;
        }
        if (pass.def->shaderMode < 0) { // ISF: its own shader and parameters
            const EffectInstance &e = chain[pass.index];
            QVector<double> values(pass.def->params.size());
            for (int k = 0; k < values.size(); ++k)
                values[k] = effectParam(e, k);
            for (const Override &o : overrides)
                if (o.effect == pass.index && o.param >= 0 && o.param < values.size())
                    values[o.param] = o.value;
            if (m_pendingUpload) { // the panel textures may be waiting: submit them now
                cb->resourceUpdate(m_pendingUpload);
                m_pendingUpload = nullptr;
            }
            if (m_isf && m_isf->render(cb, c.rt[ping], e.type, values, current, timeSec, true)) {
                current = c.tex[ping];
                ping ^= 1;
            }
            continue;
        }
        QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
        QRhiTexture *history = pass.def->usesHistory ? historyFor(c, pass.index, size, u) : nullptr;
        QRhiShaderResourceBindings *srb = srbFor(current, history);
        if (!srb) {
            u->release();
            break;
        }

        const EffectInstance &e = chain[pass.index];
        float params[8] = {};
        for (int p = 0; p < 8; ++p)
            params[p] = float(effectParam(e, p));
        for (const Override &o : overrides)
            if (o.effect == pass.index && o.param >= 0 && o.param < 8)
                params[o.param] = float(o.value);

        char data[kUniformSize];
        std::memset(data, 0, sizeof(data));
        std::memcpy(data, corr.constData(), 64);
        const float sizeVec[4] = {float(size.width()), float(size.height()), time, flipY};
        std::memcpy(data + 64, sizeVec, sizeof(sizeVec));
        const qint32 modeVec[4] = {pass.def->shaderMode, pass.pass, m_panelCount, m_panelCount > 0 ? 1 : 0};
        std::memcpy(data + 80, modeVec, sizeof(modeVec));
        std::memcpy(data + 96, params, sizeof(params));

        if (m_pendingUpload) {
            u->merge(m_pendingUpload);
            m_pendingUpload->release();
            m_pendingUpload = nullptr;
        }
        u->updateDynamicBuffer(m_ubuf, m_offset, kUniformSize, data);
        cb->beginPass(c.rt[ping], Qt::transparent, {1.0f, 0}, u);
        cb->setGraphicsPipeline(ps);
        cb->setViewport(QRhiViewport(0, 0, size.width(), size.height()));
        const QRhiCommandBuffer::DynamicOffset off(0, m_offset);
        cb->setShaderResources(srb, 1, &off);
        cb->draw(4);
        cb->endPass();
        m_offset += m_stride;
        if (history) { // this output is next frame's history
            QRhiResourceUpdateBatch *copy = m_rhi->nextResourceUpdateBatch();
            copy->copyTexture(history, c.tex[ping]);
            cb->resourceUpdate(copy);
        }

        current = c.tex[ping];
        ping ^= 1;
    }
    return current;
}
