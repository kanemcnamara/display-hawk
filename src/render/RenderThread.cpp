#include "render/RenderThread.h"

#include "media/Ndi.h"
#include "render/Patterns.h"
#include "ui/OutputWindow.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>
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

void RenderThread::setClipFrame(const QString &clipId, const QVideoFrame &frame)
{
    QMutexLocker lock(&m_mutex);
    m_sharedMedia[clipId] = {frame, QImage(), false, HapFrame()};
}

void RenderThread::setClipImage(const QString &clipId, const QImage &image)
{
    QMutexLocker lock(&m_mutex);
    m_sharedMedia[clipId] = {QVideoFrame(), image, false, HapFrame()};
}

void RenderThread::setClipHap(const QString &clipId, const HapFrame &frame)
{
    QMutexLocker lock(&m_mutex);
    m_sharedMedia[clipId] = {QVideoFrame(), QImage(), false, frame};
}

void RenderThread::dropClipMedia(const QString &clipId)
{
    QMutexLocker lock(&m_mutex);
    m_sharedMedia[clipId] = {QVideoFrame(), QImage(), true, HapFrame()};
}

void RenderThread::setPreview(int view, const PreviewRequest &request)
{
    if (view < 0 || view > 1)
        return;
    QMutexLocker lock(&m_mutex);
    m_sharedPreview[view] = request;
}

void RenderThread::setNdiOutputs(const QVector<QPair<int, QString>> &outputs)
{
    QMutexLocker lock(&m_mutex);
    m_sharedNdi = outputs;
    m_ndiDirty = true;
}

void RenderThread::syncNdiOutputs()
{
    // Called on the render thread with the wanted list already copied into m_ndiWanted.
    for (int i = int(m_ndiOutputs.size()) - 1; i >= 0; --i) {
        NdiOutput *o = m_ndiOutputs[i];
        if (m_ndiWanted.contains({o->screen, o->name}))
            continue;
        if (o->pending)
            m_rhi->finish(); // completes the readback before its result object goes away
        delete o->sender;
        releaseTarget(o->target);
        delete o;
        m_ndiOutputs.removeAt(i);
    }
    for (const auto &w : std::as_const(m_ndiWanted)) {
        bool have = false;
        for (const NdiOutput *o : std::as_const(m_ndiOutputs))
            have = have || (o->screen == w.first && o->name == w.second);
        if (have)
            continue;
        auto *o = new NdiOutput;
        o->screen = w.first;
        o->name = w.second;
        o->sender = new NdiSender(w.second);
        if (!o->sender->isValid())
            qWarning("NDI output %s not started: %s", qPrintable(w.second), qPrintable(Ndi::runtimeStatus()));
        else
            qInfo("NDI output: %s", qPrintable(w.second));
        m_ndiOutputs.append(o);
    }
}

void RenderThread::renderNdiOutputs(QRhiCommandBuffer *cb)
{
    for (NdiOutput *o : std::as_const(m_ndiOutputs)) {
        if (o->pending || !o->sender->isValid() || o->screen < 0 || o->screen >= m_project.screens.size())
            continue;
        const Screen &s = m_project.screens[o->screen];
        const QSize size(s.width, s.height);
        if (!ensureTarget(o->target, size, true))
            continue;
        QRhiResourceUpdateBatch *after = m_rhi->nextResourceUpdateBatch();
        o->readback = QRhiReadbackResult();
        o->readback.completed = [o] { o->done = true; };
        after->readBackTexture(QRhiReadbackDescription(o->target.tex), &o->readback);
        m_quad.recordPass(cb, o->target.rt, Qt::black, screenDraws(s, size, QRectF(QPointF(), size)), nullptr, after);
        o->pending = true;
    }
}

void RenderThread::setModulation(double master, const QVector<double> &layerOpacity,
                                 const QVector<EffectParamMod> &effects)
{
    QMutexLocker lock(&m_mutex);
    m_sharedMasterMod = master;
    m_sharedLayerMod = layerOpacity;
    m_sharedEffectMod = effects;
}

void RenderThread::setIdentifySlices(bool on)
{
    QMutexLocker lock(&m_mutex);
    m_sharedIdentify = on;
}

void RenderThread::releaseScreenOverlays()
{
    for (ScreenOverlay &o : m_screenOverlays) {
        if (o.tex) {
            m_quad.forgetTexture(o.tex);
            o.tex->deleteLater();
        }
    }
    m_screenOverlays.clear();
}

void RenderThread::syncScreenOverlays(QRhiResourceUpdateBatch *u)
{
    if (!m_identify) {
        if (!m_screenOverlays.isEmpty())
            releaseScreenOverlays();
        return;
    }
    const Project &p = m_project;
    while (m_screenOverlays.size() > p.screens.size()) {
        ScreenOverlay o = m_screenOverlays.takeLast();
        if (o.tex) {
            m_quad.forgetTexture(o.tex);
            o.tex->deleteLater();
        }
    }
    m_screenOverlays.resize(p.screens.size());
    const QJsonArray screens = p.toJson()["screens"].toArray();
    for (int i = 0; i < p.screens.size(); ++i) {
        const QByteArray key = QJsonDocument(screens[i].toObject()).toJson(QJsonDocument::Compact);
        ScreenOverlay &o = m_screenOverlays[i];
        if (key != o.key || !o.tex) {
            uploadImage(u, o.tex, Patterns::outputOverlay(p, p.screens[i]));
            o.key = key;
        }
    }
}

void RenderThread::syncMaskOverlays(QRhiResourceUpdateBatch *u)
{
    const Project &p = m_project;
    while (m_maskOverlays.size() > p.screens.size()) {
        ScreenOverlay o = m_maskOverlays.takeLast();
        if (o.tex) {
            m_quad.forgetTexture(o.tex);
            o.tex->deleteLater();
        }
    }
    m_maskOverlays.resize(p.screens.size());
    for (int i = 0; i < p.screens.size(); ++i) {
        const Screen &s = p.screens[i];
        QJsonArray masks;
        for (const ProjectionMask &m : s.masks) {
            QJsonArray pts;
            for (const QPointF &pt : m.points)
                pts << pt.x() << pt.y();
            masks.append(QJsonArray{m.enabled, m.invert, m.feather, pts});
        }
        const QByteArray key = QJsonDocument(QJsonArray{s.width, s.height, masks}).toJson(QJsonDocument::Compact);
        ScreenOverlay &o = m_maskOverlays[i];
        if (key == o.key)
            continue;
        o.key = key;
        const QImage img = Patterns::maskOverlay(s);
        if (img.isNull()) {
            if (o.tex) {
                m_quad.forgetTexture(o.tex);
                o.tex->deleteLater();
                o.tex = nullptr;
            }
        } else {
            uploadImage(u, o.tex, img);
        }
    }
}

void RenderThread::setShareOutputs(const QVector<QPair<int, QString>> &outputs)
{
    QMutexLocker lock(&m_mutex);
    m_sharedShare = outputs;
    m_shareDirty = true;
}

void RenderThread::syncShareOutputs()
{
    for (int i = int(m_shareOutputs.size()) - 1; i >= 0; --i) {
        ShareOutput *o = m_shareOutputs[i];
        if (m_shareWanted.contains({o->screen, o->name}))
            continue;
        m_rhi->finish();
        delete o->sender;
        releaseTarget(o->target);
        delete o;
        m_shareOutputs.removeAt(i);
    }
    for (const auto &w : std::as_const(m_shareWanted)) {
        bool have = false;
        for (const ShareOutput *o : std::as_const(m_shareOutputs))
            have = have || (o->screen == w.first && o->name == w.second);
        if (!have)
            m_shareOutputs.append(new ShareOutput{w.first, w.second});
    }
}

void RenderThread::renderShareOutputs(QRhiCommandBuffer *cb)
{
    for (ShareOutput *o : std::as_const(m_shareOutputs)) {
        if (o->failed || o->screen < 0 || o->screen >= m_project.screens.size())
            continue;
        if (!o->sender) {
            o->sender = TextureSender::create(m_rhi, o->name);
            if (!o->sender) {
                o->failed = true; // logged once
                if (TextureShare::supportsBackend(m_rhi))
                    qWarning("%s output %s could not be created", qPrintable(TextureShare::technology()), qPrintable(o->name));
                else
                    qWarning("%s output %s not started: needs the %s backend",
                             qPrintable(TextureShare::technology().isEmpty() ? QStringLiteral("Texture sharing") : TextureShare::technology()),
                             qPrintable(o->name), TextureShare::technology() == QLatin1String("Syphon") ? "Metal" : "Direct3D 11");
                continue;
            }
            qInfo("%s output: %s", qPrintable(TextureShare::technology()), qPrintable(o->name));
        }
        const Screen &s = m_project.screens[o->screen];
        const QSize size(s.width, s.height);
        if (!ensureTarget(o->target, size, false, o->sender->format()))
            continue;
        m_quad.recordPass(cb, o->target.rt, Qt::black, screenDraws(s, size, QRectF(QPointF(), size)));
        o->sender->publish(cb, o->target.tex);
    }
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
    if (!m_quad.init(m_rhi) || !m_yuv.init(m_rhi) || !m_fx.init(m_rhi)) {
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
    m_clock.start();
    m_perfLog = qEnvironmentVariableIsSet("DISPLAYHAWK_PERF");

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
                m_fx.newFrame();
                renderComposition(cb);
                renderPreviews(cb);
                renderNdiOutputs(cb);
                renderShareOutputs(cb);
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
                m_fx.newFrame();
                if (!compositionDone) {
                    renderComposition(cb);
                    renderPreviews(cb);
                    renderNdiOutputs(cb);
                    renderShareOutputs(cb);
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
            if (m_perfLog && m_perfFrames > 0)
                qInfo("perf: %d video frames/s (%d on GPU path), %.2f ms each to prepare", m_perfFrames,
                      m_perfGpuFrames, m_perfNs / 1e6 / m_perfFrames);
            m_perfNs = 0;
            m_perfFrames = m_perfGpuFrames = 0;
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
    m_ndiWanted.clear();
    syncNdiOutputs();
    m_shareWanted.clear();
    syncShareOutputs();
    for (LayerState &ls : m_layers)
        m_fx.forgetTexture(ls.target.tex);
    m_fx.forgetTexture(m_comp.tex);
    m_fx.release();
    releaseTarget(m_comp);
    releaseTarget(m_compFinal);
    m_compOut = nullptr;
    releaseTarget(m_previewTarget[0]);
    releaseTarget(m_previewTarget[1]);
    for (ClipTex &ct : m_clipTex)
        releaseClipTex(ct, false);
    m_clipTex.clear();
    for (LayerState &ls : m_layers)
        releaseTarget(ls.target);
    m_layers.clear();
    for (ClipTarget &ct : m_clipTargets)
        releaseTarget(ct.target);
    m_clipTargets.clear();
    delete m_overlayTex;
    m_overlayTex = nullptr;
    releaseScreenOverlays();
    for (ScreenOverlay &o : m_maskOverlays)
        delete o.tex;
    m_maskOverlays.clear();
    delete m_maskTex;
    m_maskTex = nullptr;
    m_yuv.release();
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
    for (auto it = m_sharedMedia.cbegin(); it != m_sharedMedia.cend(); ++it) {
        ClipTex &ct = m_clipTex[it.key()];
        if (it->remove) {
            ct.hasMedia = false;
            ct.newMedia = false;
            ct.frame = QVideoFrame();
            ct.image = QImage();
            ct.hap = HapFrame();
        } else {
            ct.frame = it->frame;
            ct.image = it->image;
            ct.hap = it->hap;
            ct.newMedia = true;
        }
    }
    m_sharedMedia.clear();
    m_preview[0] = m_sharedPreview[0];
    m_preview[1] = m_sharedPreview[1];
    m_masterMod = m_sharedMasterMod;
    m_layerMod = m_sharedLayerMod;
    m_effectMod = m_sharedEffectMod;
    m_identify = m_sharedIdentify;
    const bool ndiChanged = m_ndiDirty;
    if (m_ndiDirty) {
        m_ndiWanted = m_sharedNdi;
        m_ndiDirty = false;
    }
    if (m_shareDirty) {
        m_shareWanted = m_sharedShare;
        m_shareDirty = false;
        syncShareOutputs();
    }

    if (ndiChanged) {
        lock.unlock(); // creating NDI senders can take a moment; do not hold the GUI thread
        syncNdiOutputs();
        lock.relock();
    }
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

bool RenderThread::ensureTarget(Target &t, const QSize &size, bool readable, QRhiTexture::Format format)
{
    if (t.tex && t.size == size && t.format == format)
        return true;
    if (t.tex) {
        m_quad.forgetTexture(t.tex);
        m_fx.forgetTexture(t.tex);
        // Still referenced by command buffers in flight; QRhi frees them once those complete.
        t.rt->deleteLater();
        t.rp->deleteLater();
        t.tex->deleteLater();
        t = Target();
    }
    QRhiTexture::Flags flags = QRhiTexture::RenderTarget;
    if (readable)
        flags |= QRhiTexture::UsedAsTransferSource;
    t.tex = m_rhi->newTexture(format, size, 1, flags);
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
    t.format = format;
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

QByteArray RenderThread::patternKey(const Clip &c) const
{
    const Project &p = m_project;
    QJsonObject o{{"pattern", int(c.pattern)},
                  {"spacing", c.gridSpacing},
                  {"solid", c.solidColor.name(QColor::HexArgb)},
                  {"w", p.compWidth},
                  {"h", p.compHeight}};
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

void RenderThread::releaseClipTex(ClipTex &ct, bool deferred)
{
    if (ct.tex) {
        m_quad.forgetTexture(ct.tex);
        if (deferred)
            ct.tex->deleteLater();
        else
            delete ct.tex;
        ct.tex = nullptr;
    }
    if (ct.hapTex) {
        m_quad.forgetTexture(ct.hapTex);
        if (deferred)
            ct.hapTex->deleteLater();
        else
            delete ct.hapTex;
        ct.hapTex = nullptr;
    }
    if (ct.yuv.rgba)
        m_quad.forgetTexture(ct.yuv.rgba);
    m_yuv.releaseTarget(ct.yuv, deferred);
    if (ct.share) {
        if (deferred)
            m_rhi->finish(); // its wrapped texture may be used by frames in flight
        delete ct.share;
        ct.share = nullptr;
    }
}

void RenderThread::appendClipDraw(QRhiCommandBuffer *cb, QVector<RhiQuad::Params> &draws,
                                  QRhiResourceUpdateBatch *u, const Clip &clip, const QRect &target,
                                  const QSize &compSize, float opacity)
{
    ClipTex &ct = m_clipTex[clip.id];
    ct.lastUsed = m_clock.elapsed();

    RhiQuad::Params qp;
    qp.target = compSize;
    qp.opacity = opacity;
    qp.clip = target;

    if (clip.source == ContentSource::Share) {
        // Spout / Syphon: the frame is already a GPU texture on our device.
        if (!ct.share && !ct.shareFailed) {
            ct.share = TextureReceiver::create(m_rhi, clip.inputId);
            ct.shareFailed = !ct.share;
            if (ct.shareFailed)
                qWarning("Cannot receive %s here (needs the %s backend)", qPrintable(clip.inputId),
                         TextureShare::technology() == QLatin1String("Syphon") ? "Metal" : "Direct3D 11");
        }
        QRhiTexture *tex = ct.share ? ct.share->receive(cb) : nullptr;
        if (!tex)
            return;
        qp.texture = tex;
        qp.dst = scaledRect(tex->pixelSize(), target, clip.scale);
        draws.append(qp);
        return;
    }
    if (clip.source != ContentSource::TestPattern) { // files and live inputs
        if (ct.newMedia) {
            ct.newMedia = false;
            QElapsedTimer prep;
            prep.start();
            QRhiTexture *oldRgba = ct.yuv.rgba;
            static const bool gpuYuv = !qEnvironmentVariableIsSet("DISPLAYHAWK_NO_GPU_YUV");
            ct.useYuv = ct.useHap = false;
            ct.srcScale = QSizeF(1.0, 1.0);
            if (ct.hap.isValid()) {
                // HAP: the blocks are already a GPU texture format, so they are uploaded untouched.
                const HapFrame &h = ct.hap;
                const QSize padded = h.paddedSize();
                ct.mediaSize = h.size;
                ct.srcScale = QSizeF(double(h.size.width()) / padded.width(), double(h.size.height()) / padded.height());
                if (h.kind == HapFrame::YCoCg || h.kind == HapFrame::YCoCgAlpha) {
                    ct.useYuv = m_yuv.convertHap(cb, h, ct.yuv);
                } else {
                    const QRhiTexture::Format format = h.kind == HapFrame::BC1   ? QRhiTexture::BC1
                                                       : h.kind == HapFrame::BC3 ? QRhiTexture::BC3
                                                                                 : QRhiTexture::BC7;
                    if (!m_rhi->isTextureFormatSupported(format)) {
                        static bool warned = false;
                        if (!warned)
                            qWarning("This GPU/backend cannot sample compressed format %d; HAP clip skipped", int(format));
                        warned = true;
                    } else {
                        if (!ct.hapTex || ct.hapTex->pixelSize() != padded || ct.hapTex->format() != format) {
                            if (ct.hapTex) {
                                m_quad.forgetTexture(ct.hapTex);
                                ct.hapTex->deleteLater();
                            }
                            ct.hapTex = m_rhi->newTexture(format, padded);
                            if (!ct.hapTex->create()) {
                                delete ct.hapTex;
                                ct.hapTex = nullptr;
                            }
                        }
                        if (ct.hapTex) {
                            u->uploadTexture(ct.hapTex, QRhiTextureUploadDescription(
                                                            {0, 0, QRhiTextureSubresourceUploadDescription(h.color)}));
                            ct.useHap = true;
                        }
                    }
                }
                ct.hasMedia = ct.useHap || ct.useYuv;
                m_perfNs += prep.nsecsElapsed();
                ++m_perfFrames;
                ++m_perfGpuFrames;
            } else if (gpuYuv && ct.frame.isValid() && YuvConverter::supports(ct.frame.pixelFormat())
                       && m_yuv.convert(cb, ct.frame, ct.yuv)) {
                // Planes uploaded as decoded; colour conversion ran on the GPU.
                ct.useYuv = true;
                ct.hasMedia = true;
                ct.mediaSize = ct.yuv.size;
                m_perfNs += prep.nsecsElapsed();
                ++m_perfFrames;
                ++m_perfGpuFrames;
            } else {
                const QImage img = ct.frame.isValid() ? ct.frame.toImage() : ct.image;
                ct.hasMedia = !img.isNull();
                ct.mediaSize = img.size();
                if (ct.hasMedia)
                    uploadImage(u, ct.tex, img);
                if (ct.frame.isValid()) {
                    m_perfNs += prep.nsecsElapsed();
                    ++m_perfFrames;
                }
            }
            if (oldRgba && oldRgba != ct.yuv.rgba)
                m_quad.forgetTexture(oldRgba);
            ct.frame = QVideoFrame();
            ct.image = QImage();
            ct.hap = HapFrame();
            ct.patternKey.clear();
        }
        QRhiTexture *tex = ct.useHap ? ct.hapTex : ct.useYuv ? ct.yuv.rgba : ct.tex;
        if (!ct.hasMedia || !tex)
            return;
        qp.texture = tex;
        qp.src = QRectF(0, 0, ct.srcScale.width(), ct.srcScale.height());
        qp.texFlipY = ct.useYuv && m_rhi->isYUpInFramebuffer(); // rendered, not uploaded
        qp.dst = scaledRect(ct.mediaSize.isEmpty() ? tex->pixelSize() : ct.mediaSize, target, clip.scale);
    } else {
        if (clip.pattern == TestPattern::MovingBars) {
            // A vertical bar sweeps across in 2 s and a horizontal bar down in 3 s, over black.
            // Tearing, scan-line or sync faults on LED processors show up as broken or doubled bars.
            const double t = m_clock.elapsed() / 1000.0;
            const int barW = qMax(4, target.width() / 64);
            const int barH = qMax(4, target.height() / 36);
            qp.color = Qt::black;
            qp.dst = target;
            draws.append(qp);
            qp.color = Qt::white;
            qp.dst = QRectF(target.x() + std::fmod(t / 2.0, 1.0) * (target.width() - barW), target.y(), barW,
                            target.height());
            draws.append(qp);
            qp.dst = QRectF(target.x(), target.y() + std::fmod(t / 3.0, 1.0) * (target.height() - barH),
                            target.width(), barH);
            draws.append(qp);
            return;
        }
        // Patterns are drawn for the target area; the panel map always covers the whole composition.
        const bool panelMap = clip.pattern == TestPattern::PanelMap;
        const QSize size = panelMap ? m_project.compSize() : target.size();
        QByteArray key = patternKey(clip);
        key += QByteArray::number(size.width()) + 'x' + QByteArray::number(size.height());
        if (key != ct.patternKey || !ct.tex) {
            uploadImage(u, ct.tex, Patterns::generate(m_project, clip, size));
            ct.patternKey = key;
            ct.hasMedia = false;
            ct.useYuv = ct.useHap = false;
            ct.srcScale = QSizeF(1.0, 1.0);
        }
        if (!ct.tex)
            return;
        qp.texture = ct.tex;
        qp.dst = panelMap ? QRectF(QPointF(), m_project.compSize()) : QRectF(target);
        if (panelMap)
            qp.clip = QRect();
    }
    draws.append(qp);
}

void RenderThread::renderComposition(QRhiCommandBuffer *cb)
{
    const Project &p = m_project;
    const qint64 now = m_clock.elapsed();
    const int maxSize = m_rhi->resourceLimit(QRhi::TextureSizeMax);
    const QSize compSize = p.compSize().boundedTo(QSize(maxSize, maxSize));
    if (!ensureTarget(m_comp, compSize, false)) {
        m_compOut = nullptr;
        return;
    }
    const double timeSec = now / 1000.0;
    m_fx.setFrame(now, &p);

    bool anySolo = false;
    for (const Layer &l : p.layers)
        anySolo = anySolo || (l.solo && !l.bypass);

    // Each layer renders its clip(s) into its own texture (premultiplied), then is blended into the
    // composition with its opacity and blend mode, bottom layer first.
    QVector<RhiQuad::Params> compDraws;
    for (const Layer &layer : p.layers) {
        LayerState &st = m_layers[layer.id];
        st.lastUsed = now;
        const Clip *active = layer.activeClip();
        const QString activeId = active ? active->id : QString();
        if (activeId != st.current.id) {
            // Crossfade from the outgoing clip unless this layer cuts.
            st.previous = layer.transitionMs > 0 ? st.current : Clip();
            st.current = active ? *active : Clip();
            st.since = now;
            st.duration = layer.transitionMs;
        } else if (active) {
            st.current = *active; // pick up edits to the playing clip
        }
        float mix = 1.0f; // transition progress
        if (!st.previous.isEmpty()) {
            mix = st.duration > 0 ? float(now - st.since) / float(st.duration) : 1.0f;
            if (mix >= 1.0f) {
                st.previous = Clip();
                mix = 1.0f;
            }
        }

        // Audio-reactive modulation scales the fader value; the fader itself is untouched.
        const int layerIndex = int(&layer - p.layers.constData());
        const double opacity = layer.opacity * (layerIndex < m_layerMod.size() ? m_layerMod[layerIndex] : 1.0);
        if (layer.bypass || (anySolo && !layer.solo) || opacity <= 0.0)
            continue;
        if (st.current.isEmpty() && st.previous.isEmpty())
            continue;
        if (!ensureTarget(st.target, compSize, false))
            continue;

        QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
        QVector<RhiQuad::Params> draws;
        const QRect target = p.targetRect(layer.targetWall);
        // The transition style decides where the outgoing and incoming clips are while it runs.
        Placement out, in;
        const double w = target.width(), h = target.height(), m = mix;
        const QRectF t(target);
        switch (st.previous.isEmpty() ? TransitionStyle::Crossfade : layer.transition) {
        case TransitionStyle::Crossfade: in.opacity = mix; break;
        case TransitionStyle::WipeLeft: in.scissor = QRectF(t.right() - w * m, t.top(), w * m, h); break;
        case TransitionStyle::WipeRight: in.scissor = QRectF(t.left(), t.top(), w * m, h); break;
        case TransitionStyle::WipeUp: in.scissor = QRectF(t.left(), t.bottom() - h * m, w, h * m); break;
        case TransitionStyle::WipeDown: in.scissor = QRectF(t.left(), t.top(), w, h * m); break;
        case TransitionStyle::PushLeft: in.offset = {w * (1 - m), 0}; out.offset = {-w * m, 0}; break;
        case TransitionStyle::PushRight: in.offset = {-w * (1 - m), 0}; out.offset = {w * m, 0}; break;
        case TransitionStyle::PushUp: in.offset = {0, h * (1 - m)}; out.offset = {0, -h * m}; break;
        case TransitionStyle::PushDown: in.offset = {0, -h * (1 - m)}; out.offset = {0, h * m}; break;
        case TransitionStyle::Zoom: in.zoom = qMax(0.001, m); in.opacity = mix; break;
        }
        if (!st.previous.isEmpty())
            placeClip(cb, draws, u, st.previous, target, compSize, out, timeSec);
        if (!st.current.isEmpty())
            placeClip(cb, draws, u, st.current, target, compSize, in, timeSec);
        m_quad.recordPass(cb, st.target.rt, Qt::transparent, draws, u);
        QRhiTexture *layerTex = m_fx.apply(cb, layer.id, st.target.tex, compSize, layer.effects,
                                           effectOverrides(layerIndex), timeSec);

        RhiQuad::Params lp;
        lp.target = compSize;
        lp.dst = QRectF(QPointF(), compSize);
        lp.texture = layerTex;
        lp.texFlipY = m_rhi->isYUpInFramebuffer();
        lp.opacity = float(opacity);
        lp.blend = RhiQuad::fromLayer(layer.blend);
        // Clips were fitted into the wall as if it were upright; the layer turns with the wall.
        if (layer.targetWall >= 0 && layer.targetWall < p.walls.size() && p.walls[layer.targetWall].rotated()) {
            const LedWall &w = p.walls[layer.targetWall];
            lp.dstAngle = w.angle;
            lp.pivot = QRectF(w.rect()).center();
        }
        compDraws.append(lp);
    }

    // Composition effects run on the layers over the background, before the panel mask, the
    // overlay and the master fader, which then go on in a second pass.
    bool compEffects = false;
    for (const EffectInstance &e : p.compEffects)
        compEffects = compEffects || e.enabled;
    QRhiTexture *finalInput = nullptr;
    if (compEffects) {
        m_quad.recordPass(cb, m_comp.rt, p.background, compDraws);
        compDraws.clear();
        finalInput = m_fx.apply(cb, QStringLiteral("#composition"), m_comp.tex, compSize, p.compEffects,
                                effectOverrides(-1), timeSec);
        RhiQuad::Params cp;
        cp.target = compSize;
        cp.dst = QRectF(QPointF(), compSize);
        cp.texture = finalInput;
        cp.texFlipY = m_rhi->isYUpInFramebuffer();
        cp.blend = RhiQuad::Blend::Normal;
        compDraws.append(cp);
    } else if (m_compFinal.tex) {
        releaseTarget(m_compFinal); // not needed while there are no composition effects
    }

    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    syncScreenOverlays(u); // uploaded with the composition pass, before any output is drawn
    syncMaskOverlays(u);
    if (p.maskToPanels && !p.walls.isEmpty()) {
        // Multiply by white-where-there-are-panels: what the real wall can show, nothing else.
        const QByteArray key = overlayKey();
        if (key != m_maskKey || !m_maskTex) {
            uploadImage(u, m_maskTex, Patterns::panelMask(p));
            m_maskKey = key;
        }
        if (m_maskTex) {
            RhiQuad::Params mask;
            mask.target = compSize;
            mask.dst = QRectF(QPointF(), compSize);
            mask.texture = m_maskTex;
            mask.blend = RhiQuad::Blend::Multiply;
            compDraws.append(mask);
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
            compDraws.append(ov);
        }
    }

    // Master fader and blackout dim everything, including the background.
    const double master = p.blackout ? 0.0 : qBound(0.0, p.master * m_masterMod, 1.0);
    if (master < 1.0) {
        RhiQuad::Params dim;
        dim.target = compSize;
        dim.dst = QRectF(QPointF(), compSize);
        dim.color = Qt::black;
        dim.opacity = float(1.0 - master);
        compDraws.append(dim);
    }

    if (compEffects && ensureTarget(m_compFinal, compSize, false)) {
        m_quad.recordPass(cb, m_compFinal.rt, Qt::black, compDraws, u);
        m_compOut = m_compFinal.tex;
    } else {
        m_quad.recordPass(cb, m_comp.rt, p.background, compDraws, u);
        m_compOut = m_comp.tex;
    }

    if (now - m_lastGc > 1000)
        collectGarbage(now);
}

void RenderThread::transformDraws(QVector<RhiQuad::Params> &draws, int from, const QPointF &centre, double scale,
                                  const QPointF &offset, double rotation, const QRectF &scissor)
{
    for (int i = from; i < draws.size(); ++i) {
        RhiQuad::Params &qp = draws[i];
        // Scale about the centre, then shift; a rotation turns about the moved centre.
        const QPointF tl = centre + (qp.dst.topLeft() - centre) * scale + offset;
        qp.dst = QRectF(tl, qp.dst.size() * scale);
        if (qp.dstAngle != 0.0)
            qp.pivot = centre + (qp.pivot - centre) * scale + offset;
        if (rotation != 0.0) {
            qp.dstAngle += rotation;
            qp.pivot = centre + offset;
        }
        if (!scissor.isNull()) { // a wipe; zero width at its start
            qp.clip = qp.clip.isEmpty() ? scissor.toRect() : qp.clip.intersected(scissor.toRect());
            if (qp.clip.isEmpty())
                qp.clip = QRect(-1, -1, 1, 1); // fully wiped: draws nothing (an empty clip means "no scissor")
        }
    }
}

void RenderThread::placeClip(QRhiCommandBuffer *cb, QVector<RhiQuad::Params> &draws, QRhiResourceUpdateBatch *u,
                             const Clip &clip, const QRect &target, const QSize &compSize, const Placement &placement,
                             double timeSec)
{
    const QPointF centre = QRectF(target).center();
    bool effects = false;
    for (const EffectInstance &e : clip.effects)
        effects = effects || e.enabled;

    if (!effects) {
        // Straight into the layer: the clip's own transform, then the transition's.
        const int from = int(draws.size());
        appendClipDraw(cb, draws, u, clip, target, compSize, float(clip.opacity) * placement.opacity);
        transformDraws(draws, from, centre, clip.scaleBy, QPointF(clip.posX, clip.posY), clip.rotation, {});
        transformDraws(draws, from, centre, placement.zoom, placement.offset, 0.0, placement.scissor);
        return;
    }

    // With effects: the clip renders transformed into its own texture, the effects run on that,
    // and the result goes into the layer with the transition's placement.
    ClipTarget &ct = m_clipTargets[clip.id];
    ct.lastUsed = m_clock.elapsed();
    if (!ensureTarget(ct.target, compSize, false))
        return;
    QVector<RhiQuad::Params> own;
    QRhiResourceUpdateBatch *ou = m_rhi->nextResourceUpdateBatch();
    appendClipDraw(cb, own, ou, clip, target, compSize, float(clip.opacity));
    transformDraws(own, 0, centre, clip.scaleBy, QPointF(clip.posX, clip.posY), clip.rotation, {});
    m_quad.recordPass(cb, ct.target.rt, Qt::transparent, own, ou);
    QRhiTexture *tex = m_fx.apply(cb, QStringLiteral("clip:") + clip.id, ct.target.tex, compSize, clip.effects, {}, timeSec);

    RhiQuad::Params qp;
    qp.target = compSize;
    qp.dst = QRectF(QPointF(), compSize);
    qp.texture = tex;
    qp.texFlipY = m_rhi->isYUpInFramebuffer();
    qp.blend = RhiQuad::Blend::Normal; // premultiplied
    qp.opacity = placement.opacity;
    qp.clip = target;
    draws.append(qp);
    transformDraws(draws, int(draws.size()) - 1, centre, placement.zoom, placement.offset, 0.0, placement.scissor);
}

void RenderThread::collectGarbage(qint64 now)
{
    // Free textures of clips and layers that have not been drawn for a while.
    constexpr qint64 kIdleMs = 5000;
    m_lastGc = now;
    m_fx.collectGarbage(now, kIdleMs);
    for (auto it = m_clipTargets.begin(); it != m_clipTargets.end();) {
        if (now - it->lastUsed > kIdleMs) {
            if (it->target.tex) {
                m_quad.forgetTexture(it->target.tex);
                m_fx.forgetTexture(it->target.tex);
                it->target.rt->deleteLater();
                it->target.rp->deleteLater();
                it->target.tex->deleteLater();
            }
            it = m_clipTargets.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = m_clipTex.begin(); it != m_clipTex.end();) {
        if (now - it->lastUsed > kIdleMs && !it->newMedia) {
            releaseClipTex(*it, true);
            it = m_clipTex.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = m_layers.begin(); it != m_layers.end();) {
        if (now - it->lastUsed > kIdleMs) {
            if (it->target.tex) {
                m_quad.forgetTexture(it->target.tex);
                m_fx.forgetTexture(it->target.tex);
                it->target.rt->deleteLater();
                it->target.rp->deleteLater();
                it->target.tex->deleteLater();
            }
            it = m_layers.erase(it);
        } else {
            ++it;
        }
    }
}

QVector<EffectRenderer::Override> RenderThread::effectOverrides(int layer) const
{
    QVector<EffectRenderer::Override> out;
    for (const EffectParamMod &m : m_effectMod)
        if (m.layer == layer)
            out.append({m.effect, m.param, m.value});
    return out;
}

// A mesh-warped slice as a triangle list fine enough for the curve to look smooth.
static QVector<QVector4D> tessellate(const Slice &sl, const QPointF &origin, double sx, double sy)
{
    const int perCellX = qBound(2, 64 / (sl.meshW - 1), 16), perCellY = qBound(2, 64 / (sl.meshH - 1), 16);
    const int nx = (sl.meshW - 1) * perCellX, ny = (sl.meshH - 1) * perCellY;
    QVector<QPointF> grid((nx + 1) * (ny + 1));
    for (int y = 0; y <= ny; ++y)
        for (int x = 0; x <= nx; ++x) {
            const QPointF pt = sl.outputPoint(double(x) / nx, double(y) / ny);
            grid[y * (nx + 1) + x] = QPointF(origin.x() + pt.x() * sx, origin.y() + pt.y() * sy);
        }
    QVector<QVector4D> tris;
    tris.reserve(nx * ny * 6);
    auto vertex = [&](int x, int y) {
        const QPointF &pt = grid[y * (nx + 1) + x];
        return QVector4D(float(pt.x()), float(pt.y()), float(double(x) / nx), float(double(y) / ny));
    };
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x) {
            const QVector4D a = vertex(x, y), b = vertex(x + 1, y), c = vertex(x, y + 1), d = vertex(x + 1, y + 1);
            tris << a << b << c << b << d << c;
        }
    return tris;
}

QVector<RhiQuad::Params> RenderThread::screenDraws(const Screen &screen, const QSizeF &target,
                                                   const QRectF &screenRect) const
{
    QVector<RhiQuad::Params> draws;
    if (!m_compOut || screen.width <= 0 || screen.height <= 0)
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
        if (sl.angle != 0.0) {
            // The input rect turned clockwise about its centre: origin and edge vectors.
            const QPolygonF q = sl.inputPolygon();
            qp.srcAxes = true;
            qp.src.moveTopLeft(QPointF(q[0].x() / cw, q[0].y() / ch));
            qp.srcU = QPointF((q[1].x() - q[0].x()) / cw, (q[1].y() - q[0].y()) / ch);
            qp.srcV = QPointF((q[3].x() - q[0].x()) / cw, (q[3].y() - q[0].y()) / ch);
        }
        if (sl.isMeshed()) {
            qp.mesh = tessellate(sl, screenRect.topLeft(), sx, sy);
        } else if (sl.isWarped()) {
            for (const QPointF &pt : sl.warp)
                qp.quad << QPointF(screenRect.x() + pt.x() * sx, screenRect.y() + pt.y() * sy);
        }
        if (sl.hasBlend() && !sl.output.isEmpty()) {
            qp.edgeBlend = true;
            qp.edgeWidths[0] = float(sl.blendLeft / sl.output.width());
            qp.edgeWidths[1] = float(sl.blendRight / sl.output.width());
            qp.edgeWidths[2] = float(sl.blendTop / sl.output.height());
            qp.edgeWidths[3] = float(sl.blendBottom / sl.output.height());
            qp.blendCurve = float(sl.blendCurve);
            qp.blendGamma = float(sl.blendGamma);
        }
        qp.rotation = sl.rotation;
        qp.flipH = sl.flipH;
        qp.flipV = sl.flipV;
        qp.texFlipY = m_rhi->isYUpInFramebuffer();
        qp.texture = m_compOut;
        qp.brightness = float(sl.brightness);
        qp.colorCorrect = sl.hasColorCorrection();
        qp.gain[0] = float(sl.gainR);
        qp.gain[1] = float(sl.gainG);
        qp.gain[2] = float(sl.gainB);
        qp.gamma = float(sl.gamma);
        qp.blackLevel = float(sl.blackLevel);
        draws.append(qp);
    }
    const int index = int(&screen - m_project.screens.constData());
    if (index >= 0 && index < m_maskOverlays.size() && m_maskOverlays[index].tex) {
        RhiQuad::Params mask; // projection masks black out over every slice
        mask.target = target;
        mask.dst = screenRect;
        mask.texture = m_maskOverlays[index].tex;
        draws.append(mask);
    }
    if (m_identify && index >= 0 && index < m_screenOverlays.size() && m_screenOverlays[index].tex) {
        RhiQuad::Params ov;
        ov.target = target;
        ov.dst = screenRect;
        ov.texture = m_screenOverlays[index].tex;
        draws.append(ov);
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
            if (!m_compOut)
                continue;
            RhiQuad::Params qp;
            qp.target = px;
            qp.dst = QRectF(QPointF(), px);
            const double cw = m_comp.size.width(), ch = m_comp.size.height();
            qp.src = QRectF(req.region.x() / cw, req.region.y() / ch, req.region.width() / cw,
                            req.region.height() / ch);
            qp.texFlipY = m_rhi->isYUpInFramebuffer();
            qp.texture = m_compOut;
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
    for (NdiOutput *o : std::as_const(m_ndiOutputs)) {
        if (!o->done)
            continue;
        o->done = false;
        o->pending = false;
        const QRhiReadbackResult &rb = o->readback;
        const QImage img(reinterpret_cast<const uchar *>(rb.data.constData()), rb.pixelSize.width(),
                         rb.pixelSize.height(), QImage::Format_RGBA8888);
        // NDI wants the top row first; rendered textures are bottom-up on Y-up backends (OpenGL).
        o->sender->send(m_rhi->isYUpInFramebuffer() ? img.mirrored() : img.copy());
    }
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