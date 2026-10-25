#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>

class Document;
class RenderEngine;
class QTimer;

// Acts when clips finish: "play once and clear" clips clear their layer, and layers on autopilot
// move on to their next (or a random) clip, when a movie reaches its out point or loops, or when
// a clip without an end (still, pattern, live input) has played for the layer's autopilot time.
class AutopilotController : public QObject
{
    Q_OBJECT
public:
    AutopilotController(Document *doc, RenderEngine *engine, QObject *parent = nullptr);

private:
    void onClipFinished(const QString &clipId, bool ended);
    void tick();
    void advance(int layer);
    bool hasEnd(const QString &clipId) const; // a movie: its end is reported by the player

    Document *m_doc;
    RenderEngine *m_engine;
    QTimer *m_timer;
    struct Playing {
        QString clipId;
        QElapsedTimer since;
    };
    QHash<QString, Playing> m_playing; // layer id -> what it plays and since when
};
