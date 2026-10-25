#include "media/MovReader.h"

#include <QList>
#include <QPair>

namespace {

using Atom = QPair<QByteArray, QByteArray>; // type, payload

quint32 be32(const QByteArray &d, qsizetype pos)
{
    if (pos < 0 || pos + 4 > d.size())
        return 0;
    const auto *p = reinterpret_cast<const unsigned char *>(d.constData() + pos);
    return quint32(p[0]) << 24 | quint32(p[1]) << 16 | quint32(p[2]) << 8 | p[3];
}

quint64 be64(const QByteArray &d, qsizetype pos)
{
    return quint64(be32(d, pos)) << 32 | be32(d, pos + 4);
}

quint16 be16(const QByteArray &d, qsizetype pos)
{
    if (pos < 0 || pos + 2 > d.size())
        return 0;
    const auto *p = reinterpret_cast<const unsigned char *>(d.constData() + pos);
    return quint16(p[0] << 8 | p[1]);
}

QList<Atom> children(const QByteArray &d)
{
    QList<Atom> list;
    qsizetype pos = 0;
    while (pos + 8 <= d.size()) {
        quint64 size = be32(d, pos);
        const QByteArray type = d.mid(pos + 4, 4);
        qsizetype header = 8;
        if (size == 1) {
            size = be64(d, pos + 8);
            header = 16;
        } else if (size == 0) {
            size = quint64(d.size() - pos);
        }
        if (size < quint64(header) || pos + qsizetype(size) > d.size())
            break;
        list.append({type, d.mid(pos + header, qsizetype(size) - header)});
        pos += qsizetype(size);
    }
    return list;
}

QByteArray find(const QByteArray &d, const char *type)
{
    for (const Atom &a : children(d))
        if (a.first == type)
            return a.second;
    return {};
}

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

} // namespace

bool MovReader::open(const QString &path, QString *error)
{
    close();
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::ReadOnly))
        return fail(error, m_file.errorString());

    // Walk the top-level atoms; only moov (the index) is read into memory.
    qint64 pos = 0;
    const qint64 fileSize = m_file.size();
    while (pos + 8 <= fileSize) {
        m_file.seek(pos);
        const QByteArray header = m_file.read(16);
        quint64 size = be32(header, 0);
        qint64 headerSize = 8;
        if (size == 1) {
            size = be64(header, 8);
            headerSize = 16;
        } else if (size == 0) {
            size = quint64(fileSize - pos);
        }
        if (size < quint64(headerSize))
            break;
        if (header.mid(4, 4) == "moov") {
            m_file.seek(pos + headerSize);
            return parseMoov(m_file.read(qint64(size) - headerSize), error);
        }
        pos += qint64(size);
    }
    return fail(error, QStringLiteral("no movie index (moov) found"));
}

void MovReader::close()
{
    m_file.close();
    m_codec.clear();
    m_size = QSize();
    m_offsets.clear();
    m_sizes.clear();
}

bool MovReader::parseMoov(const QByteArray &moov, QString *error)
{
    for (const Atom &trak : children(moov)) {
        if (trak.first != "trak")
            continue;
        const QByteArray mdia = find(trak.second, "mdia");
        if (find(mdia, "hdlr").mid(8, 4) != "vide")
            continue;

        const QByteArray mdhd = find(mdia, "mdhd");
        const quint32 timescale = be32(mdhd, mdhd.isEmpty() || mdhd[0] != 1 ? 12 : 20);
        const QByteArray stbl = find(find(mdia, "minf"), "stbl");

        // Sample description: codec fourcc and picture size of the first entry.
        const QByteArray stsd = find(stbl, "stsd");
        m_codec = stsd.mid(12, 4);
        m_size = QSize(be16(stsd, 40), be16(stsd, 42));

        const QByteArray stts = find(stbl, "stts");
        const quint32 delta = be32(stts, 12);
        if (timescale > 0 && delta > 0)
            m_fps = double(timescale) / delta;

        const QByteArray stsz = find(stbl, "stsz");
        const quint32 fixedSize = be32(stsz, 4);
        const quint32 count = be32(stsz, 8);

        QVector<quint64> chunkOffsets;
        if (const QByteArray stco = find(stbl, "stco"); !stco.isEmpty()) {
            for (quint32 i = 0, n = be32(stco, 4); i < n; ++i)
                chunkOffsets.append(be32(stco, 8 + i * 4));
        } else if (const QByteArray co64 = find(stbl, "co64"); !co64.isEmpty()) {
            for (quint32 i = 0, n = be32(co64, 4); i < n; ++i)
                chunkOffsets.append(be64(co64, 8 + i * 8));
        }

        // stsc runs: from chunk `first` on, each chunk holds `perChunk` samples.
        const QByteArray stsc = find(stbl, "stsc");
        QVector<QPair<quint32, quint32>> runs;
        for (quint32 i = 0, n = be32(stsc, 4); i < n; ++i)
            runs.append({be32(stsc, 8 + i * 12), be32(stsc, 12 + i * 12)});
        if (m_codec.size() != 4 || m_size.isEmpty() || count == 0 || chunkOffsets.isEmpty() || runs.isEmpty())
            return fail(error, QStringLiteral("incomplete video track"));

        quint32 sample = 0;
        for (int c = 0; c < chunkOffsets.size() && sample < count; ++c) {
            quint32 perChunk = 0;
            for (const auto &run : std::as_const(runs))
                if (run.first <= quint32(c + 1))
                    perChunk = run.second;
            quint64 offset = chunkOffsets[c];
            for (quint32 s = 0; s < perChunk && sample < count; ++s, ++sample) {
                const quint32 size = fixedSize ? fixedSize : be32(stsz, 12 + sample * 4);
                m_offsets.append(qint64(offset));
                m_sizes.append(size);
                offset += size;
            }
        }
        if (m_offsets.isEmpty())
            return fail(error, QStringLiteral("empty sample table"));
        return true;
    }
    return fail(error, QStringLiteral("no video track"));
}

QByteArray MovReader::readFrame(int index)
{
    if (index < 0 || index >= m_offsets.size() || !m_file.seek(m_offsets[index]))
        return {};
    return m_file.read(m_sizes[index]);
}

bool MovReader::isHap(const QString &path)
{
    const QString lower = path.toLower();
    if (!lower.endsWith(QLatin1String(".mov")) && !lower.endsWith(QLatin1String(".mp4"))
        && !lower.endsWith(QLatin1String(".avi")))
        return false;
    MovReader r;
    return r.open(path) && r.codec().startsWith("Hap");
}
