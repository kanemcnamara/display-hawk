#pragma once

#include <QByteArray>
#include <QFile>
#include <QSize>
#include <QVector>

// Minimal QuickTime/MP4 reader: finds the first video track and its sample table, then reads
// individual frames. Enough to play HAP, whose frames are self-contained.
class MovReader
{
public:
    bool open(const QString &path, QString *error = nullptr);
    void close();

    QByteArray codec() const { return m_codec; } // sample-description fourcc, e.g. "Hap1"
    QSize size() const { return m_size; }
    int frameCount() const { return int(m_offsets.size()); }
    double frameRate() const { return m_fps; }

    QByteArray readFrame(int index);

    // True when the file is a QuickTime/MP4 whose video track uses a HAP codec.
    static bool isHap(const QString &path);

private:
    bool parseMoov(const QByteArray &moov, QString *error);

    QFile m_file;
    QByteArray m_codec;
    QSize m_size;
    double m_fps = 30.0;
    QVector<qint64> m_offsets;
    QVector<quint32> m_sizes;
};
