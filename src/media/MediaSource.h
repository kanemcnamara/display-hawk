#pragma once

#include "media/HapDecoder.h"
#include "media/HapPlayer.h"

#include <QImage>
#include <QObject>
#include <QVideoFrame>

class QAudioDevice;
class QRhi;
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
    // The renderer's graphics device: hardware-decoded frames then stay on the GPU (zero-copy).
    void setRhi(QRhi *rhi);
    void close();
    void play();
    void pause();
    void restart();
    // Speed, direction, end behaviour and in/out points. Reverse and bounce need a HAP movie;
    // other movies play forwards (bounce loops).
    void setPlayback(const PlaybackParams &params);
    void seek(double fraction); // 0..1 of the whole movie
    double position() const;    // 0..1, -1 when unknown
    double durationSeconds() const;

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
    void looped(); // went round from the out point to the in point
    void ended();  // a play-once clip reached its end

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
    PlaybackParams m_params;
    qint64 m_lastPositionMs = 0;
    bool m_endedReported = false;
    void applyPlayerPlayback();
    void onPlayerPosition(qint64 ms);
    void syncSoundtrack(); // HAP: keeps the audio-only decoder at the pictures' position and speed
};
