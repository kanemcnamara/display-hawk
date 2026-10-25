#pragma once

#include "media/HapEncoder.h"
#include "media/MovWriter.h"

#include <QMutex>
#include <QObject>
#include <QThreadPool>

#include <atomic>
#include <memory>

class QAudioBuffer;
class QAudioBufferOutput;
class QMediaPlayer;
class QVideoFrame;
class QVideoSink;

// Converts one movie to HAP: Qt Multimedia decodes it (as fast as the encoder keeps up: the
// decoder waits while a frame is being encoded, so no frame is dropped), each frame is
// block-compressed on a low-priority thread pool and written to a QuickTime file with the sound
// as 16-bit PCM. Runs in the background; one conversion per converter.
class HapConverter : public QObject
{
    Q_OBJECT
public:
    struct Options {
        Hap::Format format = Hap::Format::Hap;
        bool audio = true;
    };

    explicit HapConverter(QObject *parent = nullptr);
    ~HapConverter() override;

    bool start(const QString &source, const QString &target, const Options &options);
    void cancel();
    bool isRunning() const { return m_running; }

    // "<name> (Hap).mov" next to the source (a free name: " 2", " 3"... if taken).
    static QString defaultTarget(const QString &source, Hap::Format format, const QString &folder = QString());

signals:
    void progress(double fraction);
    void finished(bool ok, const QString &message);

private:
    void onLoaded();
    void onFrame(const QVideoFrame &frame, int generation);   // decoder thread
    void onAudio(const QAudioBuffer &buffer, int generation); // audio thread
    void complete(bool ok, const QString &message);

    QMediaPlayer *m_player = nullptr;
    QVideoSink *m_sink = nullptr;
    QAudioBufferOutput *m_audio = nullptr;
    QThreadPool m_pool;
    QMutex m_mutex;
    std::unique_ptr<MovWriter> m_writer;
    QSize m_size; // of the first frame; later frames are scaled to it
    Options m_options;
    QString m_target;
    double m_fps = 30.0;
    qint64 m_durationUs = 0;
    qint64 m_firstUs = -1;
    int m_nextFrame = 0;
    QByteArray m_lastSample;
    bool m_opened = false;
    bool m_done = false;
    bool m_running = false;
    std::atomic<bool> m_cancel{false};
    // Which conversion is current: a late callback from a previous one (its player lingers until
    // deleted) is ignored rather than writing into the next file.
    std::atomic<int> m_generation{0};
    QString m_error;
};
