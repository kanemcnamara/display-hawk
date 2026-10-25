#pragma once

#include "media/HapDecoder.h"

#include <QImage>
#include <QObject>
#include <QVideoFrame>

class HapPlayer;
class QAudioDevice;
class QAudioOutput;
class NdiReceiver;
class QCamera;
class QMediaCaptureSession;
class QMediaPlayer;
class QThread;
class QVideoSink;

// Loads a still image, plays a HAP movie (GPU-compressed frames, read on a worker thread),
// decodes any other video through Qt Multimedia, or shows a live input: a camera / capture card
// ("capture:<device id>") or an NDI source ("ndi:<source name>"). Frames are pushed out through signals that may be
// emitted from other threads; connect with Qt::DirectConnection to thread-safe receivers.
class MediaSource : public QObject
{
    Q_OBJECT
public:
    explicit MediaSource(QObject *parent = nullptr);
    ~MediaSource() override;

    // A file path, or a live input key as produced by Clip::mediaKey().
    void open(const QString &key);
    bool isLive() const { return m_camera || m_ndi; }

    // Audio: files (including the soundtrack of HAP movies, played by an audio-only decoder
    // alongside the HAP reader). Live inputs carry no audio yet.
    bool hasAudio() const;
    void setVolume(double volume); // 0..1 (linear)
    double volume() const;
    void setAudioDevice(const QAudioDevice &device);
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
    void stopLive();
    void openCapture(const QString &deviceId);
    void openNdi(const QString &sourceName);

    QMediaPlayer *m_player = nullptr;
    QVideoSink *m_sink = nullptr;
    QAudioOutput *m_audio = nullptr;
    HapPlayer *m_hap = nullptr;
    QThread *m_hapThread = nullptr;
    QCamera *m_camera = nullptr;
    QMediaCaptureSession *m_capture = nullptr;
    QVideoSink *m_captureSink = nullptr;
    NdiReceiver *m_ndi = nullptr;
    QString m_error;
    bool m_isVideo = false;
    bool m_hasImage = false;
    bool m_hapPlaying = false;
    bool m_loop = true;
};
