#include "render/RenderEngine.h"

#include "media/MediaSource.h"
#include "model/Document.h"
#include "render/RenderThread.h"

#include <QOffscreenSurface>
#include <QAudio>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QSettings>
#include <QTimer>
#include <algorithm>
#include <rhi/qrhi_platform.h>

namespace {

constexpr int kVolumeTickMs = 15;
constexpr int kClickFreeMs = 40; // even "instant" level changes ramp this long, so they never click

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
        pushClipPreview(); // picks up edits to the previewed clip
    });
    m_audioDeviceId = QSettings().value(QStringLiteral("audio/device")).toByteArray();
    m_volumeTimer = new QTimer(this);
    m_volumeTimer->setInterval(kVolumeTickMs);
    m_volumeTimer->setTimerType(Qt::PreciseTimer);
    connect(m_volumeTimer, &QTimer::timeout, this, &RenderEngine::stepVolumes);
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
        // The previewed clip's media runs too (muted below unless it is also playing).
        for (const Clip &pc : l.clips)
            if (!m_previewClip.isEmpty() && pc.id == m_previewClip && !pc.mediaKey().isEmpty())
                wanted.insert(pc.id, &pc);
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
            // Beat sync needs the movie's length, known once it has loaded.
            connect(m, &MediaSource::stateChanged, this, [this] { QTimer::singleShot(0, this, &RenderEngine::syncMedia); });
            connect(m, &MediaSource::looped, this, [this, id] { emit clipFinished(id, false); });
            connect(m, &MediaSource::ended, this, [this, id] { emit clipFinished(id, true); });
            for (const QAudioDevice &d : QMediaDevices::audioOutputs())
                if (d.id() == m_audioDeviceId)
                    m->setAudioDevice(d);
            m_media.insert(id, m);
        }
        if (m_mediaPaths.value(id) != clip->mediaKey()) {
            m_mediaPaths.insert(id, clip->mediaKey());
            m->open(clip->mediaKey());
        }
        PlaybackParams params;
        params.speed = clip->speed;
        const double length = m->durationSeconds() * qMax(0.0, clip->outPoint - clip->inPoint);
        if (clip->beatSync > 0 && length > 0.0) // in to out lasts exactly beatSync beats
            params.speed *= length / (clip->beatSync * 60.0 / qMax(1.0, m_doc->project().bpm));
        params.reverse = clip->reverse;
        params.mode = clip->playMode;
        params.in = clip->inPoint;
        params.out = clip->outPoint;
        m->setPlayback(params);
    }

    // Audio levels: clip x layer x master, faded over the layer's crossfade when clips come and go.
    const Project &p = m_doc->project();
    bool anySolo = false;
    for (const Layer &l : p.layers)
        anySolo = anySolo || (l.solo && !l.bypass);
    const double master = p.audioMuted ? 0.0 : p.audioMaster;
    for (int li = 0; li < p.layers.size(); ++li) {
        const Layer &l = p.layers[li];
        const Clip *c = l.activeClip();
        if (!c || !wanted.contains(c->id))
            continue;
        const bool audible = !l.bypass && (!anySolo || l.solo);
        const double target = audible ? c->volume * l.volume * master : 0.0;
        const bool starting = !m_volumes.contains(c->id) || m_volumes[c->id].target == 0.0;
        m_mediaLayer.insert(c->id, li);
        rampTo(c->id, qMin(1.0, target), starting && l.transitionMs > 0 ? l.transitionMs : kClickFreeMs);
    }

    // Keep decoders of clips that just stopped alive long enough for their crossfade to finish.
    for (const QString &id : m_media.keys()) {
        if (wanted.contains(id))
            continue;
        const int li = m_mediaLayer.value(id, -1);
        const int fade = li >= 0 && li < p.layers.size() ? p.layers[li].transitionMs : 0;
        rampTo(id, 0.0, qMax(kClickFreeMs, fade));
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
    m_volumes.remove(clipId);
    m_mediaLayer.remove(clipId);
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
        m_thread->setIdentifySlices(m_identifySlices);
        connect(m_thread, &RenderThread::previewReady, this, &RenderEngine::previewReady);
        connect(m_thread, &RenderThread::statsUpdated, this, &RenderEngine::statsUpdated);
        connect(m_thread, &RenderThread::clipPreviewReady, this, &RenderEngine::clipPreviewReady);
        connect(m_thread, &RenderThread::recordFrame, this, &RenderEngine::recordFrame);
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

void RenderEngine::rampTo(const QString &clipId, double target, int ms)
{
    Ramp &r = m_volumes[clipId];
    if (qFuzzyCompare(r.target + 1.0, target + 1.0) && m_volumeTimer->isActive())
        return;
    r.target = target;
    r.step = qMax(0.0005, qAbs(target - r.current) * kVolumeTickMs / qMax(1, ms));
    if (!m_volumeTimer->isActive())
        m_volumeTimer->start();
    stepVolumes();
}

void RenderEngine::stepVolumes()
{
    bool moving = false;
    for (auto it = m_volumes.begin(); it != m_volumes.end(); ++it) {
        Ramp &r = it.value();
        if (r.current != r.target) {
            r.current = r.current < r.target ? qMin(r.target, r.current + r.step) : qMax(r.target, r.current - r.step);
            moving = moving || r.current != r.target;
        }
        // Sliders are perceptual; QAudioOutput wants linear gain.
        if (MediaSource *m = m_media.value(it.key()))
            m->setVolume(QAudio::convertVolume(r.current, QAudio::LogarithmicVolumeScale, QAudio::LinearVolumeScale));
    }
    if (!moving)
        m_volumeTimer->stop();
}

void RenderEngine::setAudioDevice(const QByteArray &deviceId)
{
    m_audioDeviceId = deviceId;
    QSettings().setValue(QStringLiteral("audio/device"), deviceId);
    QAudioDevice device = QMediaDevices::defaultAudioOutput();
    for (const QAudioDevice &d : QMediaDevices::audioOutputs())
        if (d.id() == deviceId)
            device = d;
    for (MediaSource *m : std::as_const(m_media))
        m->setAudioDevice(device);
}

void RenderEngine::setNdiOutputs(const QVector<QPair<int, QString>> &outputs)
{
    if (m_thread)
        m_thread->setNdiOutputs(outputs);
}

void RenderEngine::setPreviewClip(const QString &clipId, const QSize &size)
{
    if (clipId == m_previewClip && size == m_previewSize)
        return;
    m_previewClip = clipId;
    m_previewSize = size;
    syncMedia(); // opens (or releases) its media
    pushClipPreview();
}

void RenderEngine::pushClipPreview()
{
    if (!m_thread)
        return;
    const Project &p = m_doc->project();
    for (const Layer &l : p.layers)
        for (const Clip &c : l.clips)
            if (!m_previewClip.isEmpty() && c.id == m_previewClip) {
                const QRect area = p.targetRect(l.targetWall); // the monitor keeps the area's shape
                m_thread->setClipPreview(c, area, area.size().scaled(m_previewSize, Qt::KeepAspectRatio).expandedTo(QSize(16, 16)));
                return;
            }
    m_thread->setClipPreview(Clip(), QRect(), QSize());
}

void RenderEngine::setRecording(bool on, int fps)
{
    if (m_thread)
        m_thread->setRecording(on, fps);
}

void RenderEngine::setIdentifySlices(bool on)
{
    m_identifySlices = on;
    if (m_thread)
        m_thread->setIdentifySlices(on);
}

void RenderEngine::setShareOutputs(const QVector<QPair<int, QString>> &outputs)
{
    if (m_thread)
        m_thread->setShareOutputs(outputs);
}

void RenderEngine::setModulation(double master, const QVector<double> &layerOpacity,
                                 const QVector<EffectParamMod> &effects)
{
    if (m_thread)
        m_thread->setModulation(master, layerOpacity, effects);
}

void RenderEngine::pushProject()
{
    if (m_thread)
        m_thread->setProject(m_doc->project());
}
