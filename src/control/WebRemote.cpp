#include "control/WebRemote.h"

#include "control/ControlRouter.h"
#include "control/CueList.h"
#include "model/Document.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

WebRemote::WebRemote(Document *doc, ControlRouter *router, CueList *cues, QObject *parent)
    : QObject(parent)
    , m_doc(doc)
    , m_router(router)
    , m_cues(cues)
{
}

bool WebRemote::setEnabled(bool on, int port, bool localOnly, QString *error)
{
    delete m_server;
    m_server = nullptr;
    if (!on)
        return true;
    m_server = new QTcpServer(this);
    if (!m_server->listen(localOnly ? QHostAddress::LocalHost : QHostAddress::AnyIPv4, quint16(port))) {
        if (error)
            *error = m_server->errorString();
        qWarning("Web remote: cannot listen on port %d: %s", port, qPrintable(m_server->errorString()));
        delete m_server;
        m_server = nullptr;
        return false;
    }
    connect(m_server, &QTcpServer::newConnection, this, &WebRemote::onConnection);
    qInfo("Web remote on port %d", int(m_server->serverPort()));
    return true;
}

bool WebRemote::isEnabled() const
{
    return m_server && m_server->isListening();
}

int WebRemote::port() const
{
    return m_server ? m_server->serverPort() : 0;
}

QStringList WebRemote::urls() const
{
    QStringList out;
    if (!isEnabled())
        return out;
    for (const QHostAddress &a : QNetworkInterface::allAddresses())
        if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback())
            out << QStringLiteral("http://%1:%2/").arg(a.toString()).arg(port());
    out << QStringLiteral("http://localhost:%1/").arg(port());
    return out;
}

void WebRemote::onConnection()
{
    while (QTcpSocket *s = m_server->nextPendingConnection()) {
        connect(s, &QTcpSocket::readyRead, this, [this, s] { onReadyRead(s); });
        connect(s, &QTcpSocket::disconnected, this, [this, s] {
            m_buffers.remove(s);
            s->deleteLater();
        });
    }
}

void WebRemote::onReadyRead(QTcpSocket *socket)
{
    QByteArray &buf = m_buffers[socket];
    buf += socket->readAll();
    if (buf.size() > 65536) { // not what we serve
        socket->disconnectFromHost();
        return;
    }
    const int end = buf.indexOf("\r\n\r\n");
    if (end < 0)
        return;
    // Request line and Content-Length (bodies are read and ignored: arguments come in the query).
    const QList<QByteArray> lines = buf.left(end).split('\n');
    const QList<QByteArray> request = lines.value(0).trimmed().split(' ');
    int length = 0;
    for (const QByteArray &l : lines)
        if (l.toLower().startsWith("content-length:"))
            length = l.mid(15).trimmed().toInt();
    if (buf.size() < end + 4 + length)
        return;
    buf.remove(0, end + 4 + length);
    if (request.size() < 2) {
        respond(socket, 400, "text/plain", "Bad request");
        return;
    }
    handle(socket, request[0], QString::fromUtf8(request[1]));
}

void WebRemote::respond(QTcpSocket *socket, int status, const QByteArray &type, const QByteArray &body)
{
    const QByteArray reason = status == 200 ? "OK" : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" : "Bad Request";
    QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason + "\r\n";
    head += "Content-Type: " + type + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n";
    head += "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: keep-alive\r\n\r\n";
    socket->write(head + body);
}

QByteArray WebRemote::stateJson() const
{
    const Project &p = m_doc->project();
    QJsonArray layers;
    for (int li = int(p.layers.size()) - 1; li >= 0; --li) { // top layer first, as on screen
        const Layer &l = p.layers[li];
        QJsonArray clips;
        for (int c = 0; c < p.columns; ++c) {
            const Clip clip = l.clips.value(c);
            clips.append(clip.isEmpty() ? QJsonValue() : QJsonValue(clip.displayName()));
        }
        layers.append(QJsonObject{{"index", li + 1}, {"name", l.name}, {"opacity", l.opacity},
                                  {"active", l.activeColumn + 1}, {"bypass", l.bypass}, {"solo", l.solo},
                                  {"clips", clips}});
    }
    QJsonArray cues;
    for (const Cue &c : p.cues)
        cues.append(QJsonObject{{"name", c.name}, {"what", c.description()},
                                {"time", c.time >= 0 ? timecodeText(c.time, p.timecodeFps) : QString()}});
    const QJsonObject state{{"master", p.master}, {"blackout", p.blackout}, {"bpm", p.bpm},
                            {"crossfader", p.crossfader}, {"columns", p.columns}, {"layers", layers},
                            {"cues", cues}, {"currentCue", m_cues ? m_cues->current() + 1 : 0}};
    return QJsonDocument(state).toJson(QJsonDocument::Compact);
}

void WebRemote::handle(QTcpSocket *socket, const QByteArray &method, const QString &target)
{
    const QUrl url(target);
    const QString path = url.path();
    if (method == "GET" && (path == QLatin1String("/") || path == QLatin1String("/index.html"))) {
        QFile page(QStringLiteral(":/web/index.html"));
        page.open(QIODevice::ReadOnly);
        respond(socket, 200, "text/html; charset=utf-8", page.readAll());
        return;
    }
    if (method == "GET" && path == QLatin1String("/api/state")) {
        respond(socket, 200, "application/json", stateJson());
        return;
    }
    if (path.startsWith(QLatin1String("/api/"))) {
        if (method != "POST" && method != "GET") {
            respond(socket, 405, "text/plain", "Use POST");
            return;
        }
        // Everything else is the OSC action of the same name.
        OscMessage m;
        m.address = QStringLiteral("/dh/") + path.mid(5);
        const QUrlQuery query(url);
        if (query.hasQueryItem(QStringLiteral("value")))
            m.args << query.queryItemValue(QStringLiteral("value")).toDouble();
        m_router->handleOsc(m, QStringLiteral("web ") + socket->peerAddress().toString());
        respond(socket, 200, "application/json", stateJson());
        return;
    }
    respond(socket, 404, "text/plain", "Not found");
}
