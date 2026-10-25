#pragma once

#include "media/HapDecoder.h"
#include "media/MovReader.h"

#include <QElapsedTimer>
#include <QObject>

class QTimer;

// Plays a HAP movie on its own thread: reads and decompresses the frame due at each moment and
// emits it. Frames go straight to the GPU as compressed textures, so there is no decode step.
class HapPlayer : public QObject
{
    Q_OBJECT
public:
    explicit HapPlayer(QObject *parent = nullptr);

    // Call before moving the player to its thread.
    bool open(const QString &path, QString *error);
    QSize size() const { return m_reader.size(); }

public slots:
    void play();
    void pause();
    void restart();
    void setLoop(bool loop);
    void stop();

signals:
    // Emitted on the player's thread; connect with Qt::DirectConnection to a thread-safe receiver.
    void frame(const HapFrame &frame);
    void stateChanged(bool playing);
    void error(const QString &message);

private:
    void tick();
    qint64 positionNs() const;

    MovReader m_reader;
    QTimer *m_timer;
    QElapsedTimer m_clock;
    qint64 m_baseNs = 0;  // position when the clock last started
    bool m_playing = false;
    bool m_loop = true;
    int m_lastFrame = -1;
    bool m_reportedError = false;
};
