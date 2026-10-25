#pragma once

#include <QByteArray>
#include <QFile>
#include <QSize>
#include <QVector>

// Minimal QuickTime writer: one video track of self-contained frames (HAP) at a constant frame
// rate, and optionally one 16-bit PCM audio track interleaved with it. Frames go straight to
// disk; the index (moov) is written by finish(). Files over 4 GB get 64-bit offsets.
class MovWriter
{
public:
    struct Video {
        QByteArray fourcc;    // e.g. "Hap1"
        QSize size;
        double fps = 30.0;
        int depth = 24;       // 32 when the frames carry alpha
        QByteArray compressorName = "Hap";
    };

    ~MovWriter();
    bool open(const QString &path, const Video &video, QString *error = nullptr);
    // 16-bit little-endian PCM; set before the first audio is written.
    void setAudioFormat(int sampleRate, int channels);
    bool writeVideoFrame(const QByteArray &sample);
    void writeAudio(const QByteArray &pcm); // interleaved samples, written between video frames
    bool finish(QString *error = nullptr);  // writes the index; until then the file is unplayable
    void abort();                           // closes and deletes the file

    int framesWritten() const { return int(m_videoOffsets.size()); }
    qint64 audioSamplesWritten() const { return m_audioSamples; }
    // Movie time units per second and per frame (exact for 23.976, 29.97, 59.94...).
    static void timing(double fps, quint32 &timescale, quint32 &delta);

private:
    bool flushAudio();

    QFile m_file;
    Video m_video;
    qint64 m_mdatHeader = 0; // where the mdat header (with a spare "wide" atom before it) starts
    QVector<qint64> m_videoOffsets;
    QVector<quint32> m_videoSizes;
    int m_sampleRate = 0;
    int m_channels = 0;
    QByteArray m_pendingAudio;
    QVector<qint64> m_audioChunkOffsets;
    QVector<quint32> m_audioChunkSamples;
    qint64 m_audioSamples = 0;
    bool m_failed = false;
};
