#include "media/MediaSource.h"

#include <QFileInfo>
#include <QImageReader>
#include <QMediaPlayer>
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
        if (frame.isValid() && m_isVideo)
            emit videoFrame(frame);
    }, Qt::DirectConnection);
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, &MediaSource::stateChanged);
    connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &msg) {
        m_error = msg;
        emit stateChanged();
    });
}

void MediaSource::open(const QString &path)
{
    close();
    if (path.isEmpty())
        return;
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
    } else {
        m_isVideo = true;
        m_player->setSource(QUrl::fromLocalFile(path));
        m_player->play();
    }
    emit stateChanged();
}

void MediaSource::close()
{
    m_isVideo = false;
    m_player->stop();
    m_player->setSource(QUrl());
    m_error.clear();
    m_hasImage = false;
    emit stillImage(QImage());
}

void MediaSource::play()
{
    if (m_isVideo)
        m_player->play();
}

void MediaSource::pause()
{
    if (m_isVideo)
        m_player->pause();
}

void MediaSource::restart()
{
    if (m_isVideo) {
        m_player->setPosition(0);
        m_player->play();
    }
}

void MediaSource::setLoop(bool loop)
{
    m_player->setLoops(loop ? QMediaPlayer::Infinite : 1);
}

bool MediaSource::isPlaying() const
{
    return m_isVideo && m_player->playbackState() == QMediaPlayer::PlayingState;
}
