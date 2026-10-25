#include "media/MovWriter.h"

#include <QtMath>

namespace {

void be16(QByteArray &d, quint16 v)
{
    d.append(char(v >> 8));
    d.append(char(v & 0xFF));
}

void be32(QByteArray &d, quint32 v)
{
    for (int s = 24; s >= 0; s -= 8)
        d.append(char(v >> s & 0xFF));
}

void be64(QByteArray &d, quint64 v)
{
    be32(d, quint32(v >> 32));
    be32(d, quint32(v & 0xFFFFFFFF));
}

QByteArray atom(const char *type, const QByteArray &payload)
{
    QByteArray a;
    be32(a, quint32(8 + payload.size()));
    a.append(type, 4);
    a.append(payload);
    return a;
}

QByteArray matrix()
{
    QByteArray m;
    for (quint32 v : {0x10000u, 0u, 0u, 0u, 0x10000u, 0u, 0u, 0u, 0x40000000u})
        be32(m, v);
    return m;
}

QByteArray tkhd(quint32 trackId, quint32 duration, bool audio, const QSize &size)
{
    QByteArray d;
    be32(d, 0x0000000F); // version 0; enabled, in movie, in preview, in poster
    be32(d, 0);          // created
    be32(d, 0);          // modified
    be32(d, trackId);
    be32(d, 0);
    be32(d, duration);
    d.append(8, '\0');
    be16(d, 0);                     // layer
    be16(d, 0);                     // alternate group
    be16(d, audio ? 0x0100 : 0);    // volume
    be16(d, 0);
    d.append(matrix());
    be32(d, quint32(size.width()) << 16);
    be32(d, quint32(size.height()) << 16);
    return atom("tkhd", d);
}

QByteArray mdhd(quint32 timescale, quint32 duration)
{
    QByteArray d;
    be32(d, 0);
    be32(d, 0);
    be32(d, 0);
    be32(d, timescale);
    be32(d, duration);
    be16(d, 0); // language
    be16(d, 0); // quality
    return atom("mdhd", d);
}

QByteArray hdlr(const char *componentType, const char *subtype, const QByteArray &name)
{
    QByteArray d;
    be32(d, 0);
    d.append(componentType, 4);
    d.append(subtype, 4);
    d.append(12, '\0');
    d.append(char(name.size())); // Pascal string
    d.append(name);
    return atom("hdlr", d);
}

QByteArray dinf()
{
    QByteArray ref;
    be32(ref, 12);
    ref.append("alis", 4);
    be32(ref, 1); // self-reference: the data is in this file
    QByteArray dref;
    be32(dref, 0);
    be32(dref, 1);
    dref.append(ref);
    return atom("dinf", atom("dref", dref));
}

// Chunk offsets: 32-bit unless the file has grown past 4 GB.
QByteArray chunkOffsets(const QVector<qint64> &offsets)
{
    const bool wide = !offsets.isEmpty() && offsets.last() > 0xFFFFFFFFll;
    QByteArray d;
    be32(d, 0);
    be32(d, quint32(offsets.size()));
    for (qint64 o : offsets) {
        if (wide)
            be64(d, quint64(o));
        else
            be32(d, quint32(o));
    }
    return atom(wide ? "co64" : "stco", d);
}

} // namespace

MovWriter::~MovWriter()
{
    if (m_file.isOpen())
        abort();
}

void MovWriter::timing(double fps, quint32 &timescale, quint32 &delta)
{
    fps = qBound(1.0, fps, 1000.0);
    // NTSC rates (24000/1001, 30000/1001, 60000/1001...) exactly; anything else to 1/1000 frame.
    const double ntsc = fps * 1.001;
    if (qAbs(ntsc - qRound(ntsc)) < 0.002 && qAbs(fps - qRound(fps)) > 0.002) {
        timescale = quint32(qRound(ntsc)) * 1000;
        delta = 1001;
    } else {
        timescale = quint32(qRound(fps * 1000.0));
        delta = 1000;
    }
}

bool MovWriter::open(const QString &path, const Video &video, QString *error)
{
    m_video = video;
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = m_file.errorString();
        return false;
    }
    QByteArray head;
    QByteArray ftyp;
    ftyp.append("qt  ", 4);
    be32(ftyp, 0x200);
    ftyp.append("qt  ", 4);
    head.append(atom("ftyp", ftyp));
    m_mdatHeader = head.size();
    head.append(atom("wide", {})); // becomes part of a 64-bit mdat header if the file passes 4 GB
    be32(head, 0);                 // mdat size, filled in by finish()
    head.append("mdat", 4);
    m_failed = m_file.write(head) != head.size();
    return !m_failed;
}

void MovWriter::setAudioFormat(int sampleRate, int channels)
{
    m_sampleRate = sampleRate;
    m_channels = channels;
}

bool MovWriter::flushAudio()
{
    const int frameBytes = 2 * m_channels;
    if (m_pendingAudio.isEmpty() || frameBytes <= 0)
        return true;
    const qsizetype whole = m_pendingAudio.size() / frameBytes * frameBytes;
    if (whole == 0)
        return true;
    m_audioChunkOffsets.append(m_file.pos());
    m_audioChunkSamples.append(quint32(whole / frameBytes));
    m_audioSamples += whole / frameBytes;
    m_failed = m_failed || m_file.write(m_pendingAudio.constData(), whole) != whole;
    m_pendingAudio.remove(0, whole);
    return !m_failed;
}

bool MovWriter::writeVideoFrame(const QByteArray &sample)
{
    if (!m_file.isOpen() || m_failed)
        return false;
    flushAudio(); // the sound up to this frame goes just before it
    m_videoOffsets.append(m_file.pos());
    m_videoSizes.append(quint32(sample.size()));
    m_failed = m_file.write(sample) != sample.size();
    return !m_failed;
}

void MovWriter::writeAudio(const QByteArray &pcm)
{
    if (!m_file.isOpen() || m_sampleRate <= 0)
        return;
    m_pendingAudio.append(pcm);
    if (m_pendingAudio.size() > 2 * m_channels * m_sampleRate) // more than a second without video
        flushAudio();
}

bool MovWriter::finish(QString *error)
{
    auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        abort();
        return false;
    };
    if (!m_file.isOpen())
        return fail(QStringLiteral("not open"));
    flushAudio();
    if (m_failed)
        return fail(m_file.errorString());
    if (m_videoOffsets.isEmpty())
        return fail(QStringLiteral("no video frames"));

    // mdat size: 32 bits after the spare "wide" atom, or a 64-bit header written over both.
    const qint64 end = m_file.pos();
    const qint64 mdatSize = end - (m_mdatHeader + 8);
    QByteArray header;
    if (mdatSize <= 0xFFFFFFFFll) {
        be32(header, quint32(mdatSize));
        m_file.seek(m_mdatHeader + 8);
    } else {
        be32(header, 1);
        header.append("mdat", 4);
        be64(header, quint64(end - m_mdatHeader));
        m_file.seek(m_mdatHeader);
    }
    m_file.write(header);
    m_file.seek(end);

    quint32 timescale = 0, delta = 0;
    timing(m_video.fps, timescale, delta);
    const quint32 frames = quint32(m_videoOffsets.size());
    const quint32 videoDuration = frames * delta;
    const bool audio = m_sampleRate > 0 && m_audioSamples > 0;
    const quint32 audioDuration = audio ? quint32(qRound64(double(m_audioSamples) * timescale / m_sampleRate)) : 0;

    // Video track.
    QByteArray entry;
    entry.append(6, '\0');
    be16(entry, 1);              // data reference
    be16(entry, 0);              // version
    be16(entry, 0);              // revision
    entry.append("DHwk", 4);     // vendor
    be32(entry, 0);              // temporal quality
    be32(entry, 512);            // spatial quality
    be16(entry, quint16(m_video.size.width()));
    be16(entry, quint16(m_video.size.height()));
    be32(entry, 72 << 16);
    be32(entry, 72 << 16);
    be32(entry, 0);              // data size
    be16(entry, 1);              // frames per sample
    QByteArray name = m_video.compressorName.left(31);
    entry.append(char(name.size()));
    entry.append(name);
    entry.append(31 - name.size(), '\0');
    be16(entry, quint16(m_video.depth));
    be16(entry, 0xFFFF);         // no colour table
    QByteArray stsd;
    be32(stsd, 0);
    be32(stsd, 1);
    be32(stsd, quint32(8 + entry.size()));
    stsd.append(m_video.fourcc.left(4));
    stsd.append(entry);
    QByteArray stts;
    be32(stts, 0);
    be32(stts, 1);
    be32(stts, frames);
    be32(stts, delta);
    QByteArray stsc;
    be32(stsc, 0);
    be32(stsc, 1);
    be32(stsc, 1); // from chunk 1 on
    be32(stsc, 1); // one frame per chunk
    be32(stsc, 1);
    QByteArray stsz;
    be32(stsz, 0);
    be32(stsz, 0);
    be32(stsz, frames);
    for (quint32 s : std::as_const(m_videoSizes))
        be32(stsz, s);
    const QByteArray videoStbl = atom("stbl", atom("stsd", stsd) + atom("stts", stts) + atom("stsc", stsc)
                                                  + atom("stsz", stsz) + chunkOffsets(m_videoOffsets));
    QByteArray vmhd;
    be32(vmhd, 1);
    be16(vmhd, 0x40); // graphics mode: copy
    be16(vmhd, 0x8000);
    be16(vmhd, 0x8000);
    be16(vmhd, 0x8000);
    const QByteArray videoMinf = atom("minf", atom("vmhd", vmhd) + hdlr("dhlr", "alis", "DataHandler") + dinf() + videoStbl);
    const QByteArray videoTrak =
        atom("trak", tkhd(1, videoDuration, false, m_video.size)
                         + atom("mdia", mdhd(timescale, videoDuration) + hdlr("mhlr", "vide", "VideoHandler") + videoMinf));

    // Audio track: 16-bit little-endian PCM ("sowt"), one sample per audio frame.
    QByteArray audioTrak;
    if (audio) {
        QByteArray sowt;
        sowt.append(6, '\0');
        be16(sowt, 1);
        be16(sowt, 0); // version 0
        be16(sowt, 0);
        be32(sowt, 0);
        be16(sowt, quint16(m_channels));
        be16(sowt, 16);
        be16(sowt, 0); // compression id
        be16(sowt, 0); // packet size
        be32(sowt, quint32(m_sampleRate) << 16);
        QByteArray astsd;
        be32(astsd, 0);
        be32(astsd, 1);
        be32(astsd, quint32(8 + sowt.size()));
        astsd.append("sowt", 4);
        astsd.append(sowt);
        QByteArray astts;
        be32(astts, 0);
        be32(astts, 1);
        be32(astts, quint32(m_audioSamples));
        be32(astts, 1);
        QByteArray runs; // stsc: a new entry wherever the samples per chunk change
        quint32 runCount = 0, last = 0;
        for (int c = 0; c < m_audioChunkSamples.size(); ++c) {
            if (m_audioChunkSamples[c] == last)
                continue;
            last = m_audioChunkSamples[c];
            be32(runs, quint32(c + 1));
            be32(runs, last);
            be32(runs, 1);
            ++runCount;
        }
        QByteArray astsc;
        be32(astsc, 0);
        be32(astsc, runCount);
        astsc.append(runs);
        QByteArray astsz;
        be32(astsz, 0);
        be32(astsz, quint32(2 * m_channels)); // every sample the same size
        be32(astsz, quint32(m_audioSamples));
        const QByteArray audioStbl = atom("stbl", atom("stsd", astsd) + atom("stts", astts) + atom("stsc", astsc)
                                                      + atom("stsz", astsz) + chunkOffsets(m_audioChunkOffsets));
        const QByteArray audioMinf =
            atom("minf", atom("smhd", QByteArray(8, '\0')) + hdlr("dhlr", "alis", "DataHandler") + dinf() + audioStbl);
        audioTrak = atom("trak", tkhd(2, audioDuration, true, QSize())
                                     + atom("mdia", mdhd(quint32(m_sampleRate), quint32(m_audioSamples))
                                                        + hdlr("mhlr", "soun", "SoundHandler") + audioMinf));
    }

    QByteArray mvhd;
    be32(mvhd, 0);
    be32(mvhd, 0);
    be32(mvhd, 0);
    be32(mvhd, timescale);
    be32(mvhd, qMax(videoDuration, audioDuration));
    be32(mvhd, 0x10000); // rate
    be16(mvhd, 0x100);   // volume
    mvhd.append(10, '\0');
    mvhd.append(matrix());
    mvhd.append(24, '\0');
    be32(mvhd, audio ? 3 : 2); // next track id
    const QByteArray moov = atom("moov", atom("mvhd", mvhd) + videoTrak + audioTrak);
    if (m_file.write(moov) != moov.size())
        return fail(m_file.errorString());
    m_file.close();
    return true;
}

void MovWriter::abort()
{
    if (m_file.isOpen())
        m_file.close();
    m_file.remove();
}
