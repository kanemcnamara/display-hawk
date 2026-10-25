#pragma once

#include "media/HapDecoder.h"

#include <QImage>
#include <QObject>
#include <QVideoFrame>

class HapPlayer;
class QMediaPlayer;
class QThread;
class QVideoSink;

// Loads a still image, plays a HAP movie (GPU-compressed frames, read on a worker thread) or
// decodes any other video through Qt Multimedia. Frames are pushed out through signals that may be
// emitted from other threads; connect with Qt::DirectConnection to thread-safe receivers.
class MediaSource : public QObject
{
    Q_OBJECT
public:
    explicit MediaSource(QObject *parent = nullptr);
    ~MediaSource() override;

    void open(const QString &path);
    void close();
    void play();
    void pause();
    void restart();
    void setLoop(bool loop);

    bool isVideo() const { return m_isVideo; }
    bool isHap() const { return m_hap != nullptr; }
    bool isPlaying() const;
    bool isOpen() const { return m_isVideo || m_hasImage; }
    QString errorString() const { return m_error; }

signals:
    void videoFrame(const QVideoFrame &frame);
    void hapFrame(const HapFrame &frame);
    void stillImage(const QImage &image); // a null image clears the media
    void stateChanged();

private:
    void stopHap();

    QMediaPlayer *m_player = nullptr;
    QVideoSink *m_sink = nullptr;
    HapPlayer *m_hap = nullptr;
    QThread *m_hapThread = nullptr;
    QString m_error;
    bool m_isVideo = false;
    bool m_hasImage = false;
    bool m_hapPlaying = false;
    bool m_loop = true;
};
