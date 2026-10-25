#pragma once

#include "model/Project.h"

#include <QHash>
#include <QHostAddress>
#include <QObject>

class Document;
class RenderEngine;
class QUdpSocket;

// DMX pixel mapping output: samples each fixture's pixels from the composition (as the outputs
// show it: master and blackout included) and sends them as Art-Net or sACN, universe by universe.
class PixelMapper : public QObject
{
    Q_OBJECT
public:
    PixelMapper(Document *doc, RenderEngine *engine, QObject *parent = nullptr);

    // The channel data of each universe for a composition picture (public for tests).
    static QHash<int, QByteArray> universes(const Project &project, const QImage &composition);
    int packetsSent() const { return m_packets; }

signals:
    void sent(int universes);

private:
    void sync();
    void onFrame(const QImage &composition);

    Document *m_doc;
    RenderEngine *m_engine;
    QUdpSocket *m_socket;
    int m_sequence = 0;
    int m_packets = 0;
    bool m_active = false;
    int m_rate = 0;
};
