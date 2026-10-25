#include "control/PixelMapper.h"

#include "control/ArtNet.h"
#include "control/Sacn.h"
#include "model/Document.h"
#include "render/RenderEngine.h"

#include <QImage>
#include <QUdpSocket>
#include <QtMath>

namespace {

// The composition's colour at a point (composition pixels), bilinear between the readback's pixels.
QColor sample(const QImage &img, const QPointF &p, const QSize &comp)
{
    const double x = p.x() * img.width() / qMax(1, comp.width()) - 0.5;
    const double y = p.y() * img.height() / qMax(1, comp.height()) - 0.5;
    const int x0 = qBound(0, int(qFloor(x)), img.width() - 1), y0 = qBound(0, int(qFloor(y)), img.height() - 1);
    const int x1 = qMin(x0 + 1, img.width() - 1), y1 = qMin(y0 + 1, img.height() - 1);
    const double fx = qBound(0.0, x - qFloor(x), 1.0), fy = qBound(0.0, y - qFloor(y), 1.0);
    auto px = [&img](int xx, int yy) { return QColor::fromRgba(img.pixel(xx, yy)); };
    double rgb[3];
    const QColor a = px(x0, y0), b = px(x1, y0), c = px(x0, y1), d = px(x1, y1);
    const double ca[3] = {a.redF(), a.greenF(), a.blueF()}, cb[3] = {b.redF(), b.greenF(), b.blueF()},
                 cc[3] = {c.redF(), c.greenF(), c.blueF()}, cd[3] = {d.redF(), d.greenF(), d.blueF()};
    for (int k = 0; k < 3; ++k)
        rgb[k] = (ca[k] * (1 - fx) + cb[k] * fx) * (1 - fy) + (cc[k] * (1 - fx) + cd[k] * fx) * fy;
    return QColor::fromRgbF(float(rgb[0]), float(rgb[1]), float(rgb[2]));
}

} // namespace

PixelMapper::PixelMapper(Document *doc, RenderEngine *engine, QObject *parent)
    : QObject(parent)
    , m_doc(doc)
    , m_engine(engine)
    , m_socket(new QUdpSocket(this))
{
    connect(m_engine, &RenderEngine::dmxFrame, this, &PixelMapper::onFrame);
    connect(m_doc, &Document::changed, this, &PixelMapper::sync);
    sync();
}

void PixelMapper::sync()
{
    const Project &p = m_doc->project();
    bool any = false;
    for (const PixelFixture &f : p.fixtures)
        any = any || f.enabled;
    const bool active = p.dmxOut.enabled && any;
    if (active == m_active && p.dmxOut.rate == m_rate)
        return;
    m_active = active;
    m_rate = p.dmxOut.rate;
    m_engine->setDmxSampling(active, m_rate);
    qInfo("DMX pixel mapping output %s", active ? "on" : "off");
}

QHash<int, QByteArray> PixelMapper::universes(const Project &p, const QImage &img)
{
    QHash<int, QByteArray> out;
    if (img.isNull())
        return out;
    for (const PixelFixture &f : p.fixtures) {
        if (!f.enabled)
            continue;
        const int per = channelsPerPixel(f.order);
        int universe = f.universe;
        int channel = f.address - 1;
        for (const QPointF &pt : f.samplePoints()) {
            if (channel + per > 512) { // the next universe carries on from channel 1
                ++universe;
                channel = 0;
            }
            const QColor c = sample(img, pt, p.compSize());
            const int r = c.red(), g = c.green(), b = c.blue();
            quint8 v[4] = {0, 0, 0, 0};
            switch (f.order) {
            case ColorOrder::RGB: v[0] = r, v[1] = g, v[2] = b; break;
            case ColorOrder::GRB: v[0] = g, v[1] = r, v[2] = b; break;
            case ColorOrder::BRG: v[0] = b, v[1] = r, v[2] = g; break;
            case ColorOrder::RBG: v[0] = r, v[1] = b, v[2] = g; break;
            case ColorOrder::GBR: v[0] = g, v[1] = b, v[2] = r; break;
            case ColorOrder::BGR: v[0] = b, v[1] = g, v[2] = r; break;
            case ColorOrder::RGBW: { // white takes the part all three share
                const int w = qMin(r, qMin(g, b));
                v[0] = quint8(r - w), v[1] = quint8(g - w), v[2] = quint8(b - w), v[3] = quint8(w);
                break;
            }
            case ColorOrder::White: v[0] = quint8(qRound(0.299 * r + 0.587 * g + 0.114 * b)); break;
            }
            QByteArray &data = out[universe];
            if (data.size() < 512)
                data = data.leftJustified(512, '\0');
            for (int k = 0; k < per; ++k)
                data[channel + k] = char(v[k]);
            channel += per;
        }
    }
    return out;
}

void PixelMapper::onFrame(const QImage &composition)
{
    if (!m_active)
        return;
    const Project &p = m_doc->project();
    const QHash<int, QByteArray> data = universes(p, composition);
    // Where to: "ip[:port]", or broadcast (Art-Net) / each universe's multicast group (sACN).
    QHostAddress host;
    quint16 port = p.dmxOut.protocol == DmxProtocol::Sacn ? Sacn::kPort : ArtNet::kPort;
    const QString target = p.dmxOut.target.trimmed();
    if (!target.isEmpty()) {
        const int colon = target.lastIndexOf(QLatin1Char(':'));
        host = QHostAddress(colon > 0 ? target.left(colon) : target);
        if (colon > 0)
            port = quint16(target.mid(colon + 1).toInt());
    }
    m_sequence = (m_sequence + 1) & 0xFF;
    for (auto it = data.cbegin(); it != data.cend(); ++it) {
        if (p.dmxOut.protocol == DmxProtocol::Sacn) {
            const QByteArray packet = Sacn::build(it.key(), it.value(), 100, m_sequence);
            m_socket->writeDatagram(packet, host.isNull() ? Sacn::multicastGroup(it.key()) : host, port);
        } else {
            const QByteArray packet = ArtNet::buildDmx(it.key(), it.value(), m_sequence ? m_sequence : 1);
            m_socket->writeDatagram(packet, host.isNull() ? QHostAddress::Broadcast : host, port);
        }
        ++m_packets;
    }
    emit sent(int(data.size()));
}
