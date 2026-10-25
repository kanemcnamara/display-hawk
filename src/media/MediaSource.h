#pragma once

#include <QImage>
#include <QObject>
#include <QVideoFrame>

class QMediaPlayer;
class QVideoSink;

// Loads a still image or decodes a video. Frames are pushed out through signals, which may be
// emitted from a decoder thread; connect with Qt::DirectConnection to thread-safe receivers.
class MediaSource : public QObject
{
    Q_OBJECT
public:
    explicit MediaSource(QObject *parent = nullptr);

    void open(const QString &path);
    void close();
    void play();
    void pause();
    void restart();
    void setLoop(bool loop);

    bool isVideo() const { return m_isVideo; }
    bool isPlaying() const;
    bool isOpen() const { return m_isVideo || m_hasImage; }
    QString errorString() const { return m_error; }

signals:
    void videoFrame(const QVideoFrame &frame);
    void stillImage(const QImage &image); // a null image clears the media
    void stateChanged();

private:
    QMediaPlayer *m_player = nullptr;
    QVideoSink *m_sink = nullptr;
    QString m_error;
    bool m_isVideo = false;
    bool m_hasImage = false;
};
