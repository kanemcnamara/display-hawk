#include "media/MediaSource.h"

#include "media/HapPlayer.h"
#include "media/MovReader.h"
#include "media/Ndi.h"

#include <QAudioDevice>
#include <QAudioOutput>
#include <QCamera>
#include <QCoreApplication>
#include <QFileInfo>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QPermissions>
#include <QImageReader>
#include <QMediaPlayer>
#include <QThread>
#include <QUrl>
#include <QVideoSink>

MediaSource::MediaSource(QObject *parent)
    : QObject(parent)
    , m_player(new QMediaPlayer(this))
    , m_sink(new QVideoSink(this))
    , m_audio(new QAudioOutput(this))
{
    m_player->setVideoSink(m_sink);
    m_player->setAudioOutput(m_audio);
    m_audio->setVolume(0.0f); // the engine ramps it up when the clip plays
    // HAP movies: m_player only plays the soundtrack, so switch its video track off once known.
    connect(m_player, &QMediaPlayer::tracksChanged, this, [this] {
        if (m_hap)
            m_player->setActiveVideoTrack(-1);
    });
    m_player->setLoops(QMediaPlayer::Infinite);
    connect(m_player, &QMediaPlayer::positionChanged, this, &MediaSource::onPlayerPosition);
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::LoadedMedia && !m_hap && m_isVideo)
            applyPlayerPlayback();
        if (status != QMediaPlayer::EndOfMedia || m_hap || !m_isVideo)
            return;
        if (m_params.mode == PlayMode::OnceHold || m_params.mode == PlayMode::OnceClear) {
            if (!m_endedReported) {
                m_endedReported = true;
                emit ended();
            }
        } else { // manual looping (in/out points set): back to the in point
            m_player->setPosition(qint64(m_params.in * m_player->duration()));
            m_player->play();
            emit looped();
        }
    });
    connect(m_sink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
        if (frame.isValid() && m_isVideo && !m_hap)
            emit videoFrame(frame);
    }, Qt::DirectConnection);
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, &MediaSource::stateChanged);
    connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &msg) {
        m_error = msg;
        emit stateChanged();
    });
}

MediaSource::~MediaSource()
{
    stopHap();
    stopLive();
}

void MediaSource::open(const QString &path)
{
    close();
    if (path.isEmpty())
        return;
    if (path.startsWith(QLatin1String("capture:"))) {
        openCapture(path.mid(8));
        return;
    }
    if (path.startsWith(QLatin1String("ndi:"))) {
        openNdi(path.mid(4));
        return;
    }
    if (!QFileInfo::exists(path)) {
        m_error = tr("File not found: %1").arg(path);
        emit stateChanged();
        return;
    }

    const QByteArray suffix = QFileInfo(path).suffix().toLower().toLatin1();
    if (QImageReader::supportedImageFormats().contains(suffix)) {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const QImage image = reader.read();
        if (image.isNull())
            m_error = reader.errorString();
        m_hasImage = !image.isNull();
        emit stillImage(image);
    } else if (MovReader::isHap(path)) {
        auto *hap = new HapPlayer;
        QString err;
        if (!hap->open(path, &err)) {
            delete hap;
            m_error = err;
        } else {
            m_isVideo = true;
            m_hap = hap;
            m_hapThread = new QThread(this);
            m_hapThread->setObjectName(QStringLiteral("HAP reader"));
            m_hap->moveToThread(m_hapThread);
            connect(m_hap, &HapPlayer::frame, this, &MediaSource::hapFrame, Qt::DirectConnection);
            connect(m_hap, &HapPlayer::stateChanged, this, [this](bool playing) {
                m_hapPlaying = playing;
                emit stateChanged();
            });
            connect(m_hap, &HapPlayer::error, this, [this](const QString &msg) {
                m_error = msg;
                emit stateChanged();
            });
            connect(m_hap, &HapPlayer::looped, this, [this] {
                syncSoundtrack();
                emit looped();
            });
            connect(m_hap, &HapPlayer::ended, this, [this] {
                m_player->pause();
                emit ended();
            });
            m_hapThread->start(QThread::HighPriority);
            QMetaObject::invokeMethod(m_hap, [hap = m_hap, params = m_params] {
                hap->setParams(params);
                hap->restart();
            });
            // The soundtrack (if any) from an audio-only decoder, started together with the pictures.
            m_player->setSource(QUrl::fromLocalFile(path));
            m_player->play();
        }
    } else {
        m_isVideo = true;
        m_player->setSource(QUrl::fromLocalFile(path));
        m_player->play();
    }
    emit stateChanged();
}

void MediaSource::openCapture(const QString &deviceId)
{
#if QT_CONFIG(permissions)
    // macOS (and others) ask the user once; open again when they answer.
    const QCameraPermission permission;
    switch (qApp->checkPermission(permission)) {
    case Qt::PermissionStatus::Undetermined:
        qApp->requestPermission(permission, this, [this, deviceId] { open(QStringLiteral("capture:") + deviceId); });
        return;
    case Qt::PermissionStatus::Denied:
        m_error = tr("Camera access was denied. Allow Display Hawk under System Settings > Privacy & Security > Camera.");
        emit stateChanged();
        return;
    case Qt::PermissionStatus::Granted:
        break;
    }
#endif
    QCameraDevice device;
    for (const QCameraDevice &d : QMediaDevices::videoInputs())
        if (QString::fromUtf8(d.id()) == deviceId || d.description() == deviceId)
            device = d;
    if (device.isNull()) {
        m_error = tr("Capture device not connected");
        emit stateChanged();
        return;
    }
    m_isVideo = true;
    m_camera = new QCamera(device, this);
    // Highest resolution, then frame rate; avoid MJPEG, which would need decoding on the CPU.
    QCameraFormat best;
    auto score = [](const QCameraFormat &f) {
        const qint64 pixels = qint64(f.resolution().width()) * f.resolution().height();
        const bool jpeg = f.pixelFormat() == QVideoFrameFormat::Format_Jpeg;
        return std::make_tuple(!jpeg, pixels, f.maxFrameRate());
    };
    for (const QCameraFormat &f : device.videoFormats())
        if (best.isNull() || score(f) > score(best))
            best = f;
    if (!best.isNull())
        m_camera->setCameraFormat(best);
    m_captureSink = new QVideoSink(this);
    connect(m_captureSink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
        if (frame.isValid() && m_camera)
            emit videoFrame(frame);
    }, Qt::DirectConnection);
    m_capture = new QMediaCaptureSession(this);
    m_capture->setCamera(m_camera);
    m_capture->setVideoSink(m_captureSink);
    connect(m_camera, &QCamera::errorOccurred, this, [this](QCamera::Error, const QString &msg) {
        m_error = msg;
        emit stateChanged();
    });
    m_camera->start();
    qInfo("Capture: %s at %dx%d %.0f fps", qPrintable(device.description()), best.resolution().width(),
          best.resolution().height(), double(best.maxFrameRate()));
    emit stateChanged();
}

void MediaSource::openNdi(const QString &sourceName)
{
    if (!Ndi::available()) {
        m_error = Ndi::runtimeStatus();
        emit stateChanged();
        return;
    }
    m_isVideo = true;
    m_ndi = new NdiReceiver(sourceName);
    // Frames arrive on the receiver thread; the render thread setters behind stillImage are thread-safe.
    connect(m_ndi, &NdiReceiver::frame, this, &MediaSource::stillImage, Qt::DirectConnection);
    connect(m_ndi, &NdiReceiver::status, this, [this](const QString &message) {
        if (!message.startsWith(QLatin1String("Connected")))
            m_error = message;
        emit stateChanged();
    });
    m_ndi->start();
    emit stateChanged();
}

void MediaSource::stopLive()
{
    if (m_ndi) {
        m_ndi->stop();
        delete m_ndi;
        m_ndi = nullptr;
    }
    if (m_camera) {
        m_camera->stop();
        delete m_capture;
        delete m_camera;
        delete m_captureSink;
        m_capture = nullptr;
        m_camera = nullptr;
        m_captureSink = nullptr;
    }
}

void MediaSource::stopHap()
{
    if (!m_hap)
        return;
    QMetaObject::invokeMethod(m_hap, &HapPlayer::stop, Qt::BlockingQueuedConnection);
    m_hapThread->quit();
    m_hapThread->wait();
    delete m_hap;
    delete m_hapThread;
    m_hap = nullptr;
    m_hapThread = nullptr;
    m_hapPlaying = false;
}

void MediaSource::close()
{
    m_isVideo = false;
    stopHap();
    stopLive();
    m_player->stop();
    m_player->setSource(QUrl());
    m_error.clear();
    m_hasImage = false;
    emit stillImage(QImage());
}

void MediaSource::play()
{
    if (m_hap)
        QMetaObject::invokeMethod(m_hap, &HapPlayer::play);
    if (m_isVideo && !isLive())
        m_player->play();
}

void MediaSource::pause()
{
    if (m_hap)
        QMetaObject::invokeMethod(m_hap, &HapPlayer::pause);
    if (m_isVideo && !isLive())
        m_player->pause();
}

void MediaSource::restart()
{
    m_endedReported = false;
    if (m_hap) {
        QMetaObject::invokeMethod(m_hap, &HapPlayer::restart);
        syncSoundtrack();
    } else if (m_isVideo && !isLive()) {
        m_player->setPosition(qint64(m_params.in * m_player->duration()));
        m_player->play();
    }
}

void MediaSource::setPlayback(const PlaybackParams &params)
{
    if (params == m_params)
        return;
    m_params = params;
    if (m_hap) {
        QMetaObject::invokeMethod(m_hap, [hap = m_hap, params] { hap->setParams(params); });
        syncSoundtrack();
    } else if (m_isVideo && !isLive()) {
        applyPlayerPlayback();
    }
}

void MediaSource::applyPlayerPlayback()
{
    m_player->setPlaybackRate(qBound(0.05, m_params.speed, 8.0));
    // Gapless looping by the decoder when the whole movie loops; otherwise in/out are handled here.
    const bool whole = m_params.in <= 0.0 && m_params.out >= 1.0;
    const bool loops = m_params.mode == PlayMode::Loop || m_params.mode == PlayMode::Bounce;
    m_player->setLoops(loops && whole ? QMediaPlayer::Infinite : 1);
    const qint64 duration = m_player->duration();
    if (duration > 0 && m_player->position() < qint64(m_params.in * duration))
        m_player->setPosition(qint64(m_params.in * duration));
}

void MediaSource::onPlayerPosition(qint64 ms)
{
    if (m_hap || !m_isVideo)
        return;
    const qint64 duration = m_player->duration();
    const qint64 last = m_lastPositionMs;
    m_lastPositionMs = ms;
    if (duration <= 0)
        return;
    const qint64 outMs = qint64(m_params.out * duration);
    const bool once = m_params.mode == PlayMode::OnceHold || m_params.mode == PlayMode::OnceClear;
    if (m_params.out < 1.0 && ms >= outMs) {
        if (once) {
            m_player->pause();
            if (!m_endedReported) {
                m_endedReported = true;
                emit ended();
            }
        } else {
            m_player->setPosition(qint64(m_params.in * duration));
            emit looped();
        }
    } else if (ms + duration / 2 < last && !once) {
        emit looped(); // the decoder wrapped round by itself
    }
}

void MediaSource::syncSoundtrack()
{
    if (!m_hap || !m_player->hasAudio())
        return;
    // Reverse or bouncing pictures have no sensible sound: the soundtrack only plays forwards.
    m_player->setPlaybackRate(qBound(0.05, m_params.speed, 8.0));
    m_player->setPosition(qint64(m_hap->position() * m_player->duration()));
    if (m_params.reverse || m_params.mode == PlayMode::Bounce)
        m_player->pause();
    else if (m_hapPlaying)
        m_player->play();
}

void MediaSource::seek(double fraction)
{
    fraction = qBound(0.0, fraction, 1.0);
    if (m_hap) {
        QMetaObject::invokeMethod(m_hap, [hap = m_hap, fraction] { hap->seek(fraction); });
        if (m_player->hasAudio())
            m_player->setPosition(qint64(fraction * m_player->duration()));
    } else if (m_isVideo && !isLive()) {
        m_player->setPosition(qint64(fraction * m_player->duration()));
    }
}

double MediaSource::position() const
{
    if (m_hap)
        return m_hap->position();
    if (m_isVideo && !isLive() && m_player->duration() > 0)
        return double(m_player->position()) / m_player->duration();
    return -1.0;
}

double MediaSource::durationSeconds() const
{
    if (m_hap)
        return m_hap->durationSeconds();
    if (m_isVideo && !isLive())
        return m_player->duration() / 1000.0;
    return 0.0;
}

bool MediaSource::hasAudio() const
{
    return m_player->hasAudio();
}

void MediaSource::setVolume(double volume)
{
    m_audio->setVolume(float(qBound(0.0, volume, 1.0)));
}

double MediaSource::volume() const
{
    return m_audio->volume();
}

void MediaSource::setRhi(QRhi *rhi)
{
    m_sink->setRhi(rhi);
}

void MediaSource::setAudioDevice(const QAudioDevice &device)
{
    m_audio->setDevice(device);
}


bool MediaSource::isPlaying() const
{
    if (isLive())
        return m_error.isEmpty();
    if (m_hap)
        return m_hapPlaying;
    return m_isVideo && m_player->playbackState() == QMediaPlayer::PlayingState;
}
