#pragma once

#include <QHash>
#include <QObject>

class ControlRouter;
class CueList;
class Document;
class QTcpServer;
class QTcpSocket;

// A small HTTP server for remote control from a browser, phone or show-control system:
//   GET  /                  a web remote (clip grid, columns, master, crossfader, tap, cues)
//   GET  /api/state         the show's state as JSON
//   POST /api/<path>[?value=<v>]  the OSC action /dh/<path> (e.g. /api/column/2/trigger,
//                           /api/master?value=0.5, /api/cue/go, /api/tempo/tap)
// Off by default; it listens on every network interface (or localhost only) once enabled.
class WebRemote : public QObject
{
    Q_OBJECT
public:
    WebRemote(Document *doc, ControlRouter *router, CueList *cues, QObject *parent = nullptr);

    bool setEnabled(bool on, int port, bool localOnly = false, QString *error = nullptr);
    bool isEnabled() const;
    int port() const;
    QStringList urls() const; // how to reach it from other devices

    // The JSON returned by /api/state (public for tests).
    QByteArray stateJson() const;

private:
    void onConnection();
    void onReadyRead(QTcpSocket *socket);
    void respond(QTcpSocket *socket, int status, const QByteArray &type, const QByteArray &body);
    void handle(QTcpSocket *socket, const QByteArray &method, const QString &target);

    Document *m_doc;
    ControlRouter *m_router;
    CueList *m_cues;
    QTcpServer *m_server = nullptr;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};
