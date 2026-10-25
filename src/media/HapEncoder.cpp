#include "media/HapEncoder.h"

#include "media/Snappy.h"

#include <QThreadPool>
#include <QVector>

#include <cfloat>
#include <cmath>
#include <cstring>
#include <functional>

// Texture formats (https://github.com/Vidvox/hap/blob/master/documentation/HapVideoDRAFT.md):
// DXT1/BC1 blocks are two RGB565 endpoints and 2-bit indices into them and two colours between;
// DXT5/BC3 blocks add an alpha block: two 8-bit endpoints and 3-bit indices into an 8-step ramp.
// Hap Q stores scaled YCoCg in DXT5: Co and Cg (times a per-block scale of 1, 2 or 4) around 128
// in red and green, the scale in blue, and luma in the alpha block, where it keeps 8 bits.

namespace Hap {

namespace {

constexpr float kPaletteWeights[4] = {1.0f, 0.0f, 2.0f / 3.0f, 1.0f / 3.0f}; // share of endpoint 0

int quantize(float v, int max)
{
    return qBound(0, int(v * (float(max) / 255.0f) + 0.5f), max);
}

quint16 pack565(const float *c)
{
    return quint16(quantize(c[0], 31) << 11 | quantize(c[1], 63) << 5 | quantize(c[2], 31));
}

void unpack565(quint16 v, float *c)
{
    const int r = v >> 11 & 31, g = v >> 5 & 63, b = v & 31;
    c[0] = float(r << 3 | r >> 2);
    c[1] = float(g << 2 | g >> 4);
    c[2] = float(b << 3 | b >> 2);
}

// The closest of the four palette colours for each pixel; returns the total squared error.
float assignIndices(const float px[16][3], quint16 c0, quint16 c1, quint32 &indices)
{
    float pal[4][3];
    unpack565(c0, pal[0]);
    unpack565(c1, pal[1]);
    for (int k = 0; k < 3; ++k) {
        pal[2][k] = (2.0f * pal[0][k] + pal[1][k]) / 3.0f;
        pal[3][k] = (pal[0][k] + 2.0f * pal[1][k]) / 3.0f;
    }
    indices = 0;
    float error = 0.0f;
    for (int i = 0; i < 16; ++i) {
        int best = 0;
        float bestDist = FLT_MAX;
        for (int j = 0; j < 4; ++j) {
            const float dr = px[i][0] - pal[j][0], dg = px[i][1] - pal[j][1], db = px[i][2] - pal[j][2];
            const float d = dr * dr + dg * dg + db * db;
            if (d < bestDist) {
                bestDist = d;
                best = j;
            }
        }
        indices |= quint32(best) << (2 * i);
        error += bestDist;
    }
    return error;
}

void put16(quint8 *out, quint16 v)
{
    out[0] = quint8(v & 0xFF);
    out[1] = quint8(v >> 8);
}

// A DXT1 colour block (always in four-colour mode) for 16 RGB pixels.
void encodeColorBlock(const float px[16][3], quint8 *out)
{
    float mean[3] = {0, 0, 0};
    for (int i = 0; i < 16; ++i)
        for (int k = 0; k < 3; ++k)
            mean[k] += px[i][k] / 16.0f;
    float cov[3][3] = {};
    for (int i = 0; i < 16; ++i) {
        const float d[3] = {px[i][0] - mean[0], px[i][1] - mean[1], px[i][2] - mean[2]};
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b)
                cov[a][b] += d[a] * d[b];
    }
    // The principal axis (power iteration): the line the block's colours spread along.
    float axis[3] = {1.0f, 1.0f, 1.0f};
    float length = 0.0f;
    for (int it = 0; it < 8; ++it) {
        float v[3];
        for (int a = 0; a < 3; ++a)
            v[a] = cov[a][0] * axis[0] + cov[a][1] * axis[1] + cov[a][2] * axis[2];
        length = qMax(qAbs(v[0]), qMax(qAbs(v[1]), qAbs(v[2])));
        if (length < 1e-4f)
            break;
        for (int a = 0; a < 3; ++a)
            axis[a] = v[a] / length;
    }
    quint16 c0, c1;
    quint32 indices = 0;
    if (length < 1e-4f) { // one flat colour
        c0 = c1 = pack565(mean);
    } else {
        const float norm = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
        for (float &a : axis)
            a /= norm;
        float tMin = FLT_MAX, tMax = -FLT_MAX;
        for (int i = 0; i < 16; ++i) {
            const float t = (px[i][0] - mean[0]) * axis[0] + (px[i][1] - mean[1]) * axis[1] + (px[i][2] - mean[2]) * axis[2];
            tMin = qMin(tMin, t);
            tMax = qMax(tMax, t);
        }
        float e0[3], e1[3];
        for (int k = 0; k < 3; ++k) {
            e0[k] = qBound(0.0f, mean[k] + axis[k] * tMax, 255.0f);
            e1[k] = qBound(0.0f, mean[k] + axis[k] * tMin, 255.0f);
        }
        c0 = pack565(e0);
        c1 = pack565(e1);
        float error = assignIndices(px, c0, c1, indices);
        // Least-squares endpoints for the chosen indices, while that keeps lowering the error.
        for (int it = 0; it < 2; ++it) {
            float aa = 0, bb = 0, ab = 0, ax[3] = {0, 0, 0}, bx[3] = {0, 0, 0};
            for (int i = 0; i < 16; ++i) {
                const float w = kPaletteWeights[(indices >> (2 * i)) & 3];
                aa += w * w;
                bb += (1 - w) * (1 - w);
                ab += w * (1 - w);
                for (int k = 0; k < 3; ++k) {
                    ax[k] += w * px[i][k];
                    bx[k] += (1 - w) * px[i][k];
                }
            }
            const float det = aa * bb - ab * ab;
            if (qAbs(det) < 1e-6f)
                break;
            float a[3], b[3];
            for (int k = 0; k < 3; ++k) {
                a[k] = qBound(0.0f, (ax[k] * bb - bx[k] * ab) / det, 255.0f);
                b[k] = qBound(0.0f, (bx[k] * aa - ax[k] * ab) / det, 255.0f);
            }
            const quint16 n0 = pack565(a), n1 = pack565(b);
            quint32 nIndices = 0;
            const float nError = assignIndices(px, n0, n1, nIndices);
            if (nError >= error)
                break;
            c0 = n0;
            c1 = n1;
            indices = nIndices;
            error = nError;
        }
    }
    // Four-colour mode needs c0 > c1; swapping the endpoints swaps indices 0/1 and 2/3.
    if (c0 < c1) {
        std::swap(c0, c1);
        indices ^= 0x55555555u;
    } else if (c0 == c1) {
        indices = 0; // equal endpoints would select three-colour mode, where index 3 is black
    }
    put16(out, c0);
    put16(out + 2, c1);
    for (int i = 0; i < 4; ++i)
        out[4 + i] = quint8(indices >> (8 * i));
}

// A DXT5 alpha block (also BC4) for 16 values, in eight-step mode.
void encodeAlphaBlock(const quint8 v[16], quint8 *out)
{
    int lo = 255, hi = 0;
    for (int i = 0; i < 16; ++i) {
        lo = qMin(lo, int(v[i]));
        hi = qMax(hi, int(v[i]));
    }
    out[0] = quint8(hi);
    out[1] = quint8(lo);
    quint64 bits = 0;
    if (hi != lo) {
        int pal[8] = {hi, lo};
        for (int i = 2; i < 8; ++i)
            pal[i] = ((8 - i) * hi + (i - 1) * lo + 3) / 7;
        for (int i = 0; i < 16; ++i) {
            int best = 0, bestDist = 1 << 30;
            for (int j = 0; j < 8; ++j) {
                const int d = qAbs(int(v[i]) - pal[j]);
                if (d < bestDist) {
                    bestDist = d;
                    best = j;
                }
            }
            bits |= quint64(best) << (3 * i);
        }
    }
    for (int i = 0; i < 6; ++i)
        out[2 + i] = quint8(bits >> (8 * i));
}

// Runs fn(first, last) over [0, count) in pieces on the pool (or here, without one).
void parallelRanges(int count, QThreadPool *pool, const std::function<void(int, int)> &fn)
{
    const int pieces = pool ? qMin(count, qMax(1, pool->maxThreadCount()) * 2) : 1;
    if (pieces <= 1) {
        fn(0, count);
        return;
    }
    QVector<QPair<int, int>> ranges;
    for (int p = 0; p < pieces; ++p)
        ranges.append({count * p / pieces, count * (p + 1) / pieces});
    for (int p = 1; p < pieces; ++p)
        pool->start([&fn, r = ranges[p]] { fn(r.first, r.second); });
    fn(ranges[0].first, ranges[0].second);
    pool->waitForDone();
}

QByteArray section(quint8 type, const QByteArray &payload)
{
    QByteArray out;
    const quint32 size = quint32(payload.size());
    if (size < (1u << 24)) {
        out.append(char(size & 0xFF));
        out.append(char(size >> 8 & 0xFF));
        out.append(char(size >> 16 & 0xFF));
        out.append(char(type));
    } else {
        out.append(3, '\0');
        out.append(char(type));
        for (int i = 0; i < 4; ++i)
            out.append(char(size >> (8 * i) & 0xFF));
    }
    out.append(payload);
    return out;
}

quint8 textureNibble(Format f)
{
    switch (f) {
    case Format::Hap: return 0x0B;
    case Format::HapAlpha: return 0x0E;
    case Format::HapQ: return 0x0F;
    }
    return 0x0B;
}

} // namespace

QString formatName(Format f)
{
    switch (f) {
    case Format::Hap: return QStringLiteral("Hap");
    case Format::HapAlpha: return QStringLiteral("Hap Alpha");
    case Format::HapQ: return QStringLiteral("Hap Q");
    }
    return {};
}

QByteArray fourcc(Format f)
{
    switch (f) {
    case Format::Hap: return QByteArrayLiteral("Hap1");
    case Format::HapAlpha: return QByteArrayLiteral("Hap5");
    case Format::HapQ: return QByteArrayLiteral("HapY");
    }
    return {};
}

QByteArray compressTexture(const QImage &image, Format f, QThreadPool *pool)
{
    const QImage img = image.format() == QImage::Format_RGBA8888 ? image : image.convertToFormat(QImage::Format_RGBA8888);
    const int w = img.width(), h = img.height();
    if (w <= 0 || h <= 0)
        return {};
    const int bw = (w + 3) / 4, bh = (h + 3) / 4;
    const int blockBytes = f == Format::Hap ? 8 : 16;
    QByteArray out(qsizetype(bw) * bh * blockBytes, Qt::Uninitialized);
    quint8 *dst = reinterpret_cast<quint8 *>(out.data());

    parallelRanges(bh, pool, [&](int firstRow, int lastRow) {
        float px[16][3];
        quint8 alpha[16];
        for (int by = firstRow; by < lastRow; ++by) {
            for (int bx = 0; bx < bw; ++bx) {
                for (int i = 0; i < 16; ++i) { // edges repeat out to whole blocks
                    const int x = qMin(bx * 4 + (i & 3), w - 1), y = qMin(by * 4 + (i >> 2), h - 1);
                    const uchar *p = img.constScanLine(y) + 4 * x;
                    if (f == Format::HapQ) {
                        const float r = p[0], g = p[1], b = p[2];
                        px[i][0] = (r - b) / 2.0f;              // Co
                        px[i][1] = (-r + 2.0f * g - b) / 4.0f;  // Cg
                        alpha[i] = quint8(qBound(0, int((r + 2.0f * g + b) / 4.0f + 0.5f), 255)); // Y
                    } else {
                        px[i][0] = p[0];
                        px[i][1] = p[1];
                        px[i][2] = p[2];
                        alpha[i] = p[3];
                    }
                }
                quint8 *block = dst + (qsizetype(by) * bw + bx) * blockBytes;
                if (f == Format::Hap) {
                    encodeColorBlock(px, block);
                    continue;
                }
                if (f == Format::HapQ) {
                    // The larger the scale, the more of Co/Cg's precision survives quantisation.
                    float extent = 0.0f;
                    for (int i = 0; i < 16; ++i)
                        extent = qMax(extent, qMax(qAbs(px[i][0]), qAbs(px[i][1])));
                    const int scale = extent < 31.5f ? 4 : extent < 63.5f ? 2 : 1;
                    for (int i = 0; i < 16; ++i) {
                        px[i][0] = qBound(0.0f, px[i][0] * scale + 128.0f, 255.0f);
                        px[i][1] = qBound(0.0f, px[i][1] * scale + 128.0f, 255.0f);
                        px[i][2] = float((scale - 1) * 8);
                    }
                }
                encodeAlphaBlock(alpha, block);
                encodeColorBlock(px, block + 8);
            }
        }
    });
    return out;
}

QByteArray makeFrame(const QByteArray &texture, Format f, int chunks, QThreadPool *pool)
{
    const quint8 nibble = textureNibble(f);
    const int blockBytes = f == Format::Hap ? 8 : 16;
    const int blocks = int(texture.size() / blockBytes);
    chunks = qBound(1, chunks, qMax(1, blocks));
    QVector<QByteArray> parts(chunks);
    QVector<bool> compressed(chunks);
    parallelRanges(chunks, pool, [&](int first, int last) {
        for (int c = first; c < last; ++c) {
            const qsizetype begin = qsizetype(blocks) * c / chunks * blockBytes;
            const qsizetype end = c == chunks - 1 ? texture.size() : qsizetype(blocks) * (c + 1) / chunks * blockBytes;
            const QByteArray packed = snappyCompress(texture.constData() + begin, end - begin);
            compressed[c] = packed.size() < end - begin;
            parts[c] = compressed[c] ? packed : texture.mid(begin, end - begin);
        }
    });
    if (chunks == 1)
        return section(quint8((compressed[0] ? 0xB0 : 0xA0) | nibble), parts[0]);

    // Complex: the decode instructions (each chunk's compressor and size), then the chunks.
    QByteArray kinds, sizes, data;
    for (int c = 0; c < chunks; ++c) {
        kinds.append(char(compressed[c] ? 0x0B : 0x0A));
        const quint32 n = quint32(parts[c].size());
        for (int i = 0; i < 4; ++i)
            sizes.append(char(n >> (8 * i) & 0xFF));
        data.append(parts[c]);
    }
    const QByteArray instructions = section(0x01, section(0x02, kinds) + section(0x03, sizes));
    return section(quint8(0xC0 | nibble), instructions + data);
}

QImage textureToImage(const HapFrame &frame)
{
    if (!frame.isValid() || frame.kind == HapFrame::BC7 || frame.kind == HapFrame::YCoCgAlpha)
        return {};
    const QSize size = frame.size;
    const int bw = (size.width() + 3) / 4, bh = (size.height() + 3) / 4;
    const int blockBytes = frame.kind == HapFrame::BC1 ? 8 : 16;
    if (frame.color.size() < qsizetype(bw) * bh * blockBytes)
        return {};
    QImage img(size, QImage::Format_RGBA8888);
    const auto *src = reinterpret_cast<const quint8 *>(frame.color.constData());
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            const quint8 *block = src + (qsizetype(by) * bw + bx) * blockBytes;
            int alpha[16];
            for (int &a : alpha)
                a = 255;
            if (blockBytes == 16) {
                const int a0 = block[0], a1 = block[1];
                int pal[8] = {a0, a1};
                for (int i = 2; i < 8; ++i)
                    pal[i] = a0 > a1 ? ((8 - i) * a0 + (i - 1) * a1) / 7 : (i < 6 ? ((6 - i) * a0 + (i - 1) * a1) / 5 : (i == 6 ? 0 : 255));
                quint64 bits = 0;
                for (int i = 0; i < 6; ++i)
                    bits |= quint64(block[2 + i]) << (8 * i);
                for (int i = 0; i < 16; ++i)
                    alpha[i] = pal[(bits >> (3 * i)) & 7];
                block += 8;
            }
            const quint16 c0 = quint16(block[0] | block[1] << 8), c1 = quint16(block[2] | block[3] << 8);
            float pal[4][3];
            unpack565(c0, pal[0]);
            unpack565(c1, pal[1]);
            const bool four = c0 > c1 || blockBytes == 16;
            for (int k = 0; k < 3; ++k) {
                pal[2][k] = four ? (2 * pal[0][k] + pal[1][k]) / 3 : (pal[0][k] + pal[1][k]) / 2;
                pal[3][k] = four ? (pal[0][k] + 2 * pal[1][k]) / 3 : 0;
            }
            const quint32 indices = quint32(block[4] | block[5] << 8 | block[6] << 16 | quint32(block[7]) << 24);
            for (int i = 0; i < 16; ++i) {
                const int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= size.width() || y >= size.height())
                    continue;
                const float *c = pal[(indices >> (2 * i)) & 3];
                uchar *p = img.scanLine(y) + 4 * x;
                if (frame.kind == HapFrame::YCoCg) { // as the player's shader does it
                    const float scale = c[2] / 8.0f + 1.0f;
                    const float co = (c[0] - 128.0f) / scale, cg = (c[1] - 128.0f) / scale, luma = float(alpha[i]);
                    p[0] = quint8(qBound(0, qRound(luma + co - cg), 255));
                    p[1] = quint8(qBound(0, qRound(luma + cg), 255));
                    p[2] = quint8(qBound(0, qRound(luma - co - cg), 255));
                    p[3] = 255;
                } else {
                    p[0] = quint8(c[0]);
                    p[1] = quint8(c[1]);
                    p[2] = quint8(c[2]);
                    p[3] = quint8(alpha[i]);
                }
            }
        }
    }
    return img;
}

} // namespace Hap
