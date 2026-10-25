#include "media/MediaSource.h"

#include "media/HapPlayer.h"
#include "media/MovReader.h"
#include "media/Ndi.h"

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
{
    m_player->setVideoSink(m_sink);
    m_player->setLoops(QMediaPlayer::Infinite);
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
            m_hapThread->start(QThread::HighPriority);
            QMetaObject::invokeMethod(m_hap, [hap = m_hap, loop = m_loop] {
                hap->setLoop(loop);
                hap->play();
            });
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
    else if (m_isVideo)
        m_player->play();
}

void MediaSource::pause()
{
    if (m_hap)
        QMetaObject::invokeMethod(m_hap, &HapPlayer::pause);
    else if (m_isVideo)
        m_player->pause();
}

void MediaSource::restart()
{
    if (m_hap) {
        QMetaObject::invokeMethod(m_hap, &HapPlayer::restart);
    } else if (m_isVideo) {
        m_player->setPosition(0);
        m_player->play();
    }
}

void MediaSource::setLoop(bool loop)
{
    m_loop = loop;
    m_player->setLoops(loop ? QMediaPlayer::Infinite : 1);
    if (m_hap)
        QMetaObject::invokeMethod(m_hap, [hap = m_hap, loop] { hap->setLoop(loop); });
}

bool MediaSource::isPlaying() const
{
    if (isLive())
        return m_error.isEmpty();
    if (m_hap)
        return m_hapPlaying;
    return m_isVideo && m_player->playbackState() == QMediaPlayer::PlayingState;
}
