#include "render/RenderThread.h"

#include "render/Patterns.h"
#include "ui/OutputWindow.h"

#include <QJsonDocument>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <rhi/qrhi_platform.h>

namespace {

constexpr int kIdleFrameMs = 16;    // pacing when no output is live (no vsync to wait on)
constexpr int kPreviewIntervalMs = 33; // editor previews refresh at ~30 fps

QRectF scaledRect(const QSizeF &src, const QRectF &target, ScaleMode mode)
{
    if (src.isEmpty())
        return target;
    QSizeF s = src;
    switch (mode) {
    case ScaleMode::Stretch: return target;
    case ScaleMode::Native: break;
    case ScaleMode::Fit: s = src.scaled(target.size(), Qt::KeepAspectRatio); break;
    case ScaleMode::Fill: s = src.scaled(target.size(), Qt::KeepAspectRatioByExpanding); break;
    }
    return QRectF(target.center().x() - s.width() / 2, target.center().y() - s.height() / 2, s.width(),
                  s.height());
}

} // namespace

RenderThread::RenderThread(QRhi::Implementation backend, QOffscreenSurface *fallbackSurface, QObject *parent)
    : QThread(parent)
    , m_backend(backend)
    , m_fallbackSurface(fallbackSurface)
{
}

// ---------------------------------------------------------------- GUI-thread API

bool RenderThread::waitForInit(QString *error)
{
    m_initDone.acquire();
    if (!m_initOk && error)
        *error = m_initError;
    return m_initOk;
}

void RenderThread::requestStop()
{
    QMutexLocker lock(&m_mutex);
    m_stop = true;
}

void RenderThread::setProject(const Project &project)
{
    QMutexLocker lock(&m_mutex);
    m_sharedProject = project;
    m_projectDirty = true;
}

void RenderThread::setVideoFrame(const QVideoFrame &frame)
{
    QMutexLocker lock(&m_mutex);
    m_sharedFrame = frame;
    m_sharedImage = QImage();
    m_mediaDirty = true;
}

void RenderThread::setStillImage(const QImage &image)
{
    QMutexLocker lock(&m_mutex);
    m_sharedFrame = QVideoFrame();
    m_sharedImage = image;
    m_mediaDirty = true;
}

void RenderThread::setPreview(int view, const PreviewRequest &request)
{
    if (view < 0 || view > 1)
        return;
    QMutexLocker lock(&m_mutex);
    m_sharedPreview[view] = request;
}

void RenderThread::addOutput(OutputWindow *window)
{
    QMutexLocker lock(&m_mutex);
    m_toAdd.append(window);
}

void RenderThread::removeOutput(OutputWindow *window)
{
    QMutexLocker lock(&m_mutex);
    if (m_toAdd.removeAll(window) > 0)
        return; // never reached the render thread
    if (!isRunning())
        return;
    m_toRemove.append(window);
    while (m_toRemove.contains(window)) {
        if (!m_removed.wait(&m_mutex, 3000)) {
            qWarning("Render thread did not release an output within 3 s");
            m_toRemove.removeAll(window);
            break;
        }
    }
}

// ---------------------------------------------------------------- render thread

bool RenderThread::createRhi()
{
    QRhi::Flags flags;
    switch (m_backend) {
#if defined(Q_OS_WIN)
    case QRhi::D3D11: {
        QRhiD3D11InitParams params;
        m_rhi = QRhi::create(QRhi::D3D11, &params, flags);
        break;
    }
    case QRhi::D3D12: {
        QRhiD3D12InitParams params;
        m_rhi = QRhi::create(QRhi::D3D12, &params, flags);
        break;
    }
#endif
#if QT_CONFIG(metal)
    case QRhi::Metal: {
        QRhiMetalInitParams params;
        m_rhi = QRhi::create(QRhi::Metal, &params, flags);
        break;
    }
#endif
#if QT_CONFIG(opengl)
    case QRhi::OpenGLES2: {
        QRhiGles2InitParams params;
        params.fallbackSurface = m_fallbackSurface;
        params.format = m_fallbackSurface ? m_fallbackSurface->format() : QSurfaceFormat::defaultFormat();
        m_rhi = QRhi::create(QRhi::OpenGLES2, &params, flags);
        break;
    }
#endif
    default:
        break;
    }
    return m_rhi != nullptr;
}

void RenderThread::run()
{
    if (!createRhi()) {
        m_initError = tr("Could not initialise the graphics backend.");
        m_initDone.release();
        return;
    }
    if (!m_quad.init(m_rhi)) {
        m_initError = tr("Could not load shaders.");
        delete m_rhi;
        m_rhi = nullptr;
        m_initDone.release();
        return;
    }
    qInfo("Render thread: %s on %s", m_rhi->backendName(), m_rhi->driverInfo().deviceName.constData());
    m_initOk = true;
    m_initDone.release();

    QElapsedTimer fpsClock;
    fpsClock.start();
    int frames = 0;
    m_previewClock[0].start();
    m_previewClock[1].start();

    for (;;) {
        {
            QMutexLocker lock(&m_mutex);
            if (m_stop)
                break;
        }
        syncState();
        QElapsedTimer frameClock;
        frameClock.start();

        QVector<Output *> active;
        for (Output &o : m_outputs)
            if (ensureSwapChain(o))
                active.append(&o);

        bool compositionDone = false;
        if (active.isEmpty()) {
            QRhiCommandBuffer *cb = nullptr;
            if (m_rhi->beginOffscreenFrame(&cb) == QRhi::FrameOpSuccess) {
                m_quad.newFrame();
                renderComposition(cb);
                renderPreviews(cb);
                m_rhi->endOffscreenFrame();
                compositionDone = true;
            }
        } else {
            // The first live output paces everything: beginFrame() waits for its vsync.
            for (Output *o : active) {
                const QRhi::FrameOpResult r = m_rhi->beginFrame(o->swapChain);
                if (r == QRhi::FrameOpSwapChainOutOfDate) {
                    o->swapChain->createOrResize();
                    continue;
                }
                if (r != QRhi::FrameOpSuccess) {
                    if (r == QRhi::FrameOpDeviceLost)
                        qWarning("Graphics device lost");
                    continue;
                }
                QRhiCommandBuffer *cb = o->swapChain->currentFrameCommandBuffer();
                m_quad.newFrame();
                if (!compositionDone) {
                    renderComposition(cb);
                    renderPreviews(cb);
                    compositionDone = true;
                }
                renderOutput(cb, *o);
                m_rhi->endFrame(o->swapChain);
            }
        }
        deliverReadbacks();

        // No vsync wait happened (no outputs, minimised windows, or vsync forced off by the driver).
        const qint64 elapsed = frameClock.elapsed();
        if (active.isEmpty() || elapsed < 2)
            QThread::msleep(quint64(qMax<qint64>(1, kIdleFrameMs - elapsed)));

        if (compositionDone)
            ++frames;
        if (fpsClock.elapsed() >= 1000) {
            emit statsUpdated(qRound(frames * 1000.0 / fpsClock.elapsed()), m_comp.size);
            frames = 0;
            fpsClock.restart();
        }
    }

    m_rhi->finish();
    for (Output &o : m_outputs)
        releaseOutput(o);
    m_outputs.clear();
    {
        QMutexLocker lock(&m_mutex);
        m_toRemove.clear();
        m_removed.wakeAll();
    }
    releaseTarget(m_comp);
    releaseTarget(m_previewTarget[0]);
    releaseTarget(m_previewTarget[1]);
    delete m_mediaTex;
    delete m_patternTex;
    delete m_overlayTex;
    m_mediaTex = m_patternTex = m_overlayTex = nullptr;
    m_quad.release();
    delete m_rhi;
    m_rhi = nullptr;
}

void RenderThread::syncState()
{
    QMutexLocker lock(&m_mutex);
    if (m_projectDirty) {
        m_project = m_sharedProject;
        m_projectDirty = false;
    }
    if (m_mediaDirty) {
        m_videoFrame = m_sharedFrame;
        m_stillImage = m_sharedImage;
        m_newMedia = true;
        m_mediaDirty = false;
    }
    m_preview[0] = m_sharedPreview[0];
    m_preview[1] = m_sharedPreview[1];

    for (OutputWindow *w : std::as_const(m_toAdd))
        m_outputs.append({w});
    m_toAdd.clear();
    if (!m_toRemove.isEmpty()) {
        for (OutputWindow *w : std::as_const(m_toRemove)) {
            for (int i = 0; i < m_outputs.size(); ++i) {
                if (m_outputs[i].window == w) {
                    releaseOutput(m_outputs[i]);
                    m_outputs.removeAt(i);
                    break;
                }
            }
        }
        m_toRemove.clear();
        m_removed.wakeAll();
    }
}

bool RenderThread::ensureSwapChain(Output &o)
{
    if (!o.window->isRenderable())
        return false;
    if (!o.swapChain) {
        o.swapChain = m_rhi->newSwapChain();
        o.swapChain->setWindow(o.window);
        o.rp = o.swapChain->newCompatibleRenderPassDescriptor();
        o.swapChain->setRenderPassDescriptor(o.rp);
        if (!o.swapChain->createOrResize()) {
            qWarning("Could not create a swapchain for output window");
            releaseOutput(o);
            return false;
        }
        return true;
    }
    const QSize surface = o.swapChain->surfacePixelSize();
    if (surface.isEmpty())
        return false;
    if (o.swapChain->currentPixelSize() != surface && !o.swapChain->createOrResize())
        return false;
    return true;
}

void RenderThread::releaseOutput(Output &o)
{
    if (!o.swapChain)
        return;
    m_rhi->finish(); // nothing in flight may still target this window
    delete o.swapChain;
    delete o.rp;
    o.swapChain = nullptr;
    o.rp = nullptr;
#if QT_CONFIG(opengl)
    // The GL context can stay current on the window surface after its swapchain is gone. The GUI
    // thread deletes the window next, so move the context back to the offscreen fallback surface.
    if (m_backend == QRhi::OpenGLES2 && m_fallbackSurface) {
        if (auto *h = static_cast<const QRhiGles2NativeHandles *>(m_rhi->nativeHandles()); h && h->context)
            h->context->makeCurrent(m_fallbackSurface);
    }
#endif
}

bool RenderThread::ensureTarget(Target &t, const QSize &size, bool readable)
{
    if (t.tex && t.size == size)
        return true;
    if (t.tex) {
        m_quad.forgetTexture(t.tex);
        // Still referenced by command buffers in flight; QRhi frees them once those complete.
        t.rt->deleteLater();
        t.rp->deleteLater();
        t.tex->deleteLater();
        t = Target();
    }
    QRhiTexture::Flags flags = QRhiTexture::RenderTarget;
    if (readable)
        flags |= QRhiTexture::UsedAsTransferSource;
    t.tex = m_rhi->newTexture(QRhiTexture::RGBA8, size, 1, flags);
    if (!t.tex->create()) {
        delete t.tex;
        t = Target();
        return false;
    }
    t.rt = m_rhi->newTextureRenderTarget({QRhiColorAttachment(t.tex)});
    t.rp = t.rt->newCompatibleRenderPassDescriptor();
    t.rt->setRenderPassDescriptor(t.rp);
    t.rt->create();
    t.size = size;
    return true;
}

void RenderThread::releaseTarget(Target &t)
{
    delete t.rt;
    delete t.rp;
    delete t.tex;
    t = Target();
}

void RenderThread::uploadImage(QRhiResourceUpdateBatch *u, QRhiTexture *&tex, const QImage &image)
{
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888); // straight alpha, matches the blend
    if (!tex || tex->pixelSize() != rgba.size()) {
        if (tex) {
            m_quad.forgetTexture(tex);
            tex->deleteLater();
        }
        tex = m_rhi->newTexture(QRhiTexture::RGBA8, rgba.size());
        if (!tex->create()) {
            delete tex;
            tex = nullptr;
            return;
        }
    }
    u->uploadTexture(tex, rgba);
}

QByteArray RenderThread::patternKey() const
{
    const Project &p = m_project;
    const Content &c = p.content;
    QJsonObject o{{"pattern", int(c.pattern)},
                  {"spacing", c.gridSpacing},
                  {"solid", c.solidColor.name(QColor::HexArgb)}};
    const QRect t = c.pattern == TestPattern::PanelMap ? QRect(QPoint(), p.compSize()) : p.contentTarget();
    o["w"] = t.width();
    o["h"] = t.height();
    if (c.pattern == TestPattern::PanelMap)
        o["walls"] = p.toJson()["walls"];
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QByteArray RenderThread::overlayKey() const
{
    const Project &p = m_project;
    QJsonObject o{{"w", p.compWidth}, {"h", p.compHeight}, {"walls", p.toJson()["walls"]}};
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

void RenderThread::renderComposition(QRhiCommandBuffer *cb)
{
    const Project &p = m_project;
    const int maxSize = m_rhi->resourceLimit(QRhi::TextureSizeMax);
    const QSize compSize = p.compSize().boundedTo(QSize(maxSize, maxSize));
    if (!ensureTarget(m_comp, compSize, false))
        return;

    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    const Content &c = p.content;
    const QRect target = p.contentTarget();
    QVector<RhiQuad::Params> draws;

    if (m_newMedia) {
        m_newMedia = false;
        const QImage img = m_videoFrame.isValid() ? m_videoFrame.toImage() : m_stillImage;
        m_hasMedia = !img.isNull();
        if (m_hasMedia)
            uploadImage(u, m_mediaTex, img);
    }

    RhiQuad::Params content;
    content.target = compSize;
    content.opacity = float(c.opacity);
    if (c.source == ContentSource::Media) {
        if (m_hasMedia && m_mediaTex) {
            content.texture = m_mediaTex;
            content.dst = scaledRect(m_mediaTex->pixelSize(), target, c.scale);
            content.clip = target;
            draws.append(content);
        }
    } else {
        const QByteArray key = patternKey();
        if (key != m_patternKey || !m_patternTex) {
            const QSize size = c.pattern == TestPattern::PanelMap ? p.compSize() : target.size();
            uploadImage(u, m_patternTex, Patterns::generate(p, size));
            m_patternKey = key;
        }
        if (m_patternTex) {
            content.texture = m_patternTex;
            if (c.pattern == TestPattern::PanelMap) {
                content.dst = QRectF(QPointF(), p.compSize());
            } else {
                content.dst = target;
                content.clip = target;
            }
            draws.append(content);
        }
    }

    if (p.panelOverlay && !p.walls.isEmpty()) {
        const QByteArray key = overlayKey();
        if (key != m_overlayKey || !m_overlayTex) {
            uploadImage(u, m_overlayTex, Patterns::panelMap(p, true));
            m_overlayKey = key;
        }
        if (m_overlayTex) {
            RhiQuad::Params ov;
            ov.target = compSize;
            ov.dst = QRectF(QPointF(), compSize);
            ov.texture = m_overlayTex;
            ov.opacity = float(p.overlayOpacity);
            draws.append(ov);
        }
    }

    m_quad.recordPass(cb, m_comp.rt, p.background, draws, u);
}

QVector<RhiQuad::Params> RenderThread::screenDraws(const Screen &screen, const QSizeF &target,
                                                   const QRectF &screenRect) const
{
    QVector<RhiQuad::Params> draws;
    if (!m_comp.tex || screen.width <= 0 || screen.height <= 0)
        return draws;
    const double sx = screenRect.width() / screen.width;
    const double sy = screenRect.height() / screen.height;
    const double cw = m_comp.size.width(), ch = m_comp.size.height();
    for (const Slice &sl : screen.slices) {
        if (!sl.enabled || sl.input.isEmpty() || sl.output.isEmpty())
            continue;
        RhiQuad::Params qp;
        qp.target = target;
        qp.dst = QRectF(screenRect.x() + sl.output.x() * sx, screenRect.y() + sl.output.y() * sy,
                        sl.output.width() * sx, sl.output.height() * sy);
        qp.src = QRectF(sl.input.x() / cw, sl.input.y() / ch, sl.input.width() / cw, sl.input.height() / ch);
        qp.rotation = sl.rotation;
        qp.flipH = sl.flipH;
        qp.flipV = sl.flipV;
        qp.texFlipY = m_rhi->isYUpInFramebuffer();
        qp.texture = m_comp.tex;
        qp.brightness = float(sl.brightness);
        draws.append(qp);
    }
    return draws;
}

void RenderThread::renderOutput(QRhiCommandBuffer *cb, Output &o)
{
    const int index = o.window->screenIndex();
    QVector<RhiQuad::Params> draws;
    const QSize px = o.swapChain->currentPixelSize();
    if (index >= 0 && index < m_project.screens.size())
        draws = screenDraws(m_project.screens[index], px, QRectF(QPointF(), px));
    m_quad.recordPass(cb, o.swapChain->currentFrameRenderTarget(), Qt::black, draws);
}

void RenderThread::renderPreviews(QRhiCommandBuffer *cb)
{
    for (int v = 0; v < 2; ++v) {
        const PreviewRequest &req = m_preview[v];
        if (!req.enabled || req.pixelSize.isEmpty() || req.region.isEmpty() || m_readbackPending[v])
            continue;
        if (m_previewClock[v].elapsed() < kPreviewIntervalMs)
            continue;
        const QSize px = req.pixelSize.boundedTo(QSize(4096, 4096));
        if (!ensureTarget(m_previewTarget[v], px, true))
            continue;

        QVector<RhiQuad::Params> draws;
        if (!req.outputMode) {
            if (!m_comp.tex)
                continue;
            RhiQuad::Params qp;
            qp.target = px;
            qp.dst = QRectF(QPointF(), px);
            const double cw = m_comp.size.width(), ch = m_comp.size.height();
            qp.src = QRectF(req.region.x() / cw, req.region.y() / ch, req.region.width() / cw,
                            req.region.height() / ch);
            qp.texFlipY = m_rhi->isYUpInFramebuffer();
            qp.texture = m_comp.tex;
            draws.append(qp);
        } else if (req.screen >= 0 && req.screen < m_project.screens.size()) {
            const Screen &s = m_project.screens[req.screen];
            const double sx = px.width() / req.region.width();
            const double sy = px.height() / req.region.height();
            const QRectF screenRect(-req.region.x() * sx, -req.region.y() * sy, s.width * sx, s.height * sy);
            draws = screenDraws(s, px, screenRect);
        } else {
            continue;
        }

        QRhiResourceUpdateBatch *after = m_rhi->nextResourceUpdateBatch();
        m_readback[v] = QRhiReadbackResult();
        m_readback[v].completed = [this, v] { m_readbackDone[v] = true; };
        after->readBackTexture(QRhiReadbackDescription(m_previewTarget[v].tex), &m_readback[v]);
        m_quad.recordPass(cb, m_previewTarget[v].rt, Qt::black, draws, nullptr, after);
        m_readbackPending[v] = true;
        m_readbackRequest[v] = req;
        m_previewClock[v].restart();
    }
}

void RenderThread::deliverReadbacks()
{
    for (int v = 0; v < 2; ++v) {
        if (!m_readbackDone[v])
            continue;
        m_readbackDone[v] = false;
        m_readbackPending[v] = false;
        const QRhiReadbackResult &rb = m_readback[v];
        QImage img(reinterpret_cast<const uchar *>(rb.data.constData()), rb.pixelSize.width(),
                   rb.pixelSize.height(), QImage::Format_RGBA8888);
        // Rendered textures are stored bottom-up on Y-up backends (OpenGL).
        const QImage out = m_rhi->isYUpInFramebuffer() ? img.mirrored() : img.copy();
        const PreviewRequest &req = m_readbackRequest[v];
        emit previewReady(v, out, req.region, req.outputMode, req.screen);
    }
}
