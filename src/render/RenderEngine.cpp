#include "render/RenderEngine.h"

#include "media/MediaSource.h"
#include "model/Document.h"
#include "render/RenderThread.h"

#include <QOffscreenSurface>
#include <QTimer>
#include <algorithm>
#include <rhi/qrhi_platform.h>

namespace {

struct BackendChoice {
    QRhi::Implementation impl;
    QSurface::SurfaceType surface;
    const char *name;
};

// Backends to try in order: the native API first, OpenGL as the fallback.
// DISPLAYHAWK_RHI=d3d11|d3d12|metal|opengl moves that backend to the front.
QVector<BackendChoice> backendCandidates()
{
    const BackendChoice gl{QRhi::OpenGLES2, QSurface::OpenGLSurface, "OpenGL"};
    QVector<BackendChoice> list;
#if defined(Q_OS_WIN)
    list << BackendChoice{QRhi::D3D11, QSurface::Direct3DSurface, "Direct3D 11"} << gl;
    const BackendChoice d3d12{QRhi::D3D12, QSurface::Direct3DSurface, "Direct3D 12"};
#elif QT_CONFIG(metal)
    list << BackendChoice{QRhi::Metal, QSurface::MetalSurface, "Metal"} << gl;
#else
    list << gl;
#endif

    const QByteArray env = qgetenv("DISPLAYHAWK_RHI").toLower();
    auto prefer = [&list](const BackendChoice &b) {
        list.erase(std::remove_if(list.begin(), list.end(), [&b](const BackendChoice &c) { return c.impl == b.impl; }),
                   list.end());
        list.prepend(b);
    };
    if (env == "opengl" || env == "gl")
        prefer(gl);
#if defined(Q_OS_WIN)
    else if (env == "d3d12")
        prefer(d3d12);
#endif
    return list;
}

} // namespace

RenderEngine::RenderEngine(Document *doc, QObject *parent)
    : QObject(parent)
    , m_doc(doc)
{
    connect(m_doc, &Document::changed, this, [this] {
        pushProject();
        syncMedia();
    });
    connect(m_doc, &Document::clipTriggered, this, [this](const QString &clipId) {
        // Re-triggering a playing clip restarts it, like a cue.
        if (MediaSource *m = m_media.value(clipId))
            m->restart();
    });
}

void RenderEngine::syncMedia()
{
    if (!m_thread)
        return;
    QHash<QString, const Clip *> wanted;
    int longestTransition = 0;
    for (const Layer &l : m_doc->project().layers) {
        longestTransition = qMax(longestTransition, l.transitionMs);
        const Clip *c = l.activeClip();
        if (c && !c->mediaKey().isEmpty())
            wanted.insert(c->id, c);
    }

    for (auto it = wanted.cbegin(); it != wanted.cend(); ++it) {
        const QString id = it.key();
        const Clip *clip = it.value();
        MediaSource *m = m_media.value(id);
        if (!m) {
            m = new MediaSource(this);
            // Frames may arrive on a decoder thread; the render thread setters are thread-safe.
            connect(m, &MediaSource::videoFrame, this, [this, id](const QVideoFrame &f) {
                if (m_thread)
                    m_thread->setClipFrame(id, f);
            }, Qt::DirectConnection);
            connect(m, &MediaSource::hapFrame, this, [this, id](const HapFrame &f) {
                if (m_thread)
                    m_thread->setClipHap(id, f);
            }, Qt::DirectConnection);
            connect(m, &MediaSource::stillImage, this, [this, id](const QImage &img) {
                if (m_thread)
                    m_thread->setClipImage(id, img);
            }, Qt::DirectConnection);
            connect(m, &MediaSource::stateChanged, this, &RenderEngine::mediaStateChanged);
            m_media.insert(id, m);
        }
        if (m_mediaPaths.value(id) != clip->mediaKey()) {
            m_mediaPaths.insert(id, clip->mediaKey());
            m->open(clip->mediaKey());
        }
        m->setLoop(clip->loop);
    }

    // Keep decoders of clips that just stopped alive long enough for their crossfade to finish.
    for (const QString &id : m_media.keys()) {
        if (wanted.contains(id))
            continue;
        QTimer::singleShot(longestTransition + 500, this, [this, id] {
            bool stillWanted = false;
            for (const Layer &l : m_doc->project().layers)
                if (const Clip *c = l.activeClip(); c && c->id == id && !c->mediaKey().isEmpty())
                    stillWanted = true;
            if (!stillWanted)
                releaseMedia(id);
        });
    }
    emit mediaStateChanged();
}

void RenderEngine::releaseMedia(const QString &clipId)
{
    MediaSource *m = m_media.take(clipId);
    if (!m)
        return;
    m_mediaPaths.remove(clipId);
    m->disconnect(this);
    m->close();
    m->deleteLater();
    if (m_thread)
        m_thread->dropClipMedia(clipId);
    emit mediaStateChanged();
}

RenderEngine::~RenderEngine()
{
    stop();
}

bool RenderEngine::start(QString *error)
{
    QStringList failures;
    for (const BackendChoice &b : backendCandidates()) {
        m_backendName = QString::fromLatin1(b.name);
        m_surfaceType = b.surface;
#if QT_CONFIG(opengl)
        if (b.impl == QRhi::OpenGLES2) {
            QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
            fmt.setVersion(3, 3);
            fmt.setProfile(QSurfaceFormat::CoreProfile);
            fmt.setSwapInterval(1);
            QSurfaceFormat::setDefaultFormat(fmt);
            // Must be created on the GUI thread.
            m_fallbackSurface = QRhiGles2InitParams::newFallbackSurface(fmt);
        }
#endif
        m_thread = new RenderThread(b.impl, m_fallbackSurface);
        connect(m_thread, &RenderThread::previewReady, this, &RenderEngine::previewReady);
        connect(m_thread, &RenderThread::statsUpdated, this, &RenderEngine::statsUpdated);
        pushProject();
        m_thread->start(QThread::HighPriority);
        QString reason;
        if (m_thread->waitForInit(&reason)) {
            if (!failures.isEmpty())
                qWarning("Using %s after: %s", b.name, qPrintable(failures.join(QStringLiteral("; "))));
            syncMedia();
            return true;
        }
        failures << QStringLiteral("%1: %2").arg(m_backendName, reason);
        qWarning("%s failed to start: %s", b.name, qPrintable(reason));
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
        delete m_fallbackSurface;
        m_fallbackSurface = nullptr;
    }
    if (error)
        *error = failures.join(QLatin1Char('\n'));
    return false;
}

void RenderEngine::stop()
{
    if (!m_thread)
        return;
    for (const QString &id : m_media.keys())
        releaseMedia(id);
    m_thread->requestStop();
    m_thread->wait();
    delete m_thread;
    m_thread = nullptr;
    delete m_fallbackSurface;
    m_fallbackSurface = nullptr;
}

void RenderEngine::addOutput(OutputWindow *window)
{
    if (m_thread)
        m_thread->addOutput(window);
}

void RenderEngine::removeOutput(OutputWindow *window)
{
    if (m_thread)
        m_thread->removeOutput(window);
}

void RenderEngine::setPreview(int view, const PreviewRequest &request)
{
    if (m_thread)
        m_thread->setPreview(view, request);
}

void RenderEngine::setNdiOutputs(const QVector<QPair<int, QString>> &outputs)
{
    if (m_thread)
        m_thread->setNdiOutputs(outputs);
}

void RenderEngine::pushProject()
{
    if (m_thread)
        m_thread->setProject(m_doc->project());
}
