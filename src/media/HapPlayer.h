#pragma once

#include "media/HapDecoder.h"
#include "media/MovReader.h"
#include "model/Project.h"

#include <QElapsedTimer>
#include <QObject>
#include <atomic>

class QTimer;

// How a movie plays: speed, direction, what happens at the ends, and the in/out points (0..1).
struct PlaybackParams {
    double speed = 1.0;
    bool reverse = false;
    PlayMode mode = PlayMode::Loop;
    double in = 0.0;
    double out = 1.0;

    bool operator==(const PlaybackParams &o) const
    {
        return speed == o.speed && reverse == o.reverse && mode == o.mode && in == o.in && out == o.out;
    }
    bool operator!=(const PlaybackParams &o) const { return !(*this == o); }
};

// Plays a HAP movie on its own thread: reads and decompresses the frame due at each moment and
// emits it. Frames go straight to the GPU as compressed textures, so there is no decode step, and
// any frame can be shown next: speed, reverse, bounce and scrubbing are exact.
class HapPlayer : public QObject
{
    Q_OBJECT
public:
    explicit HapPlayer(QObject *parent = nullptr);

    // Call before moving the player to its thread.
    bool open(const QString &path, QString *error);
    QSize size() const { return m_reader.size(); }
    double durationSeconds() const { return m_duration; }
    // Thread-safe: where playback is, 0..1 of the whole movie.
    double position() const { return m_position.load(); }

public slots:
    void play();
    void pause();
    void restart(); // from the in point (out point when reversed)
    void setParams(const PlaybackParams &params);
    void seek(double fraction);
    void stop();

signals:
    // Emitted on the player's thread; connect with Qt::DirectConnection to a thread-safe receiver.
    void frame(const HapFrame &frame);
    void stateChanged(bool playing);
    void error(const QString &message);
    void looped(); // passed the out point (in point when reversed) and went round
    void ended();  // play-once modes: reached the end and stopped there

private:
    void tick();
    void show(bool force);
    double inFrame() const;
    double outFrame() const;

    MovReader m_reader;
    QTimer *m_timer;
    QElapsedTimer m_clock;
    qint64 m_lastNs = 0;
    PlaybackParams m_params;
    double m_pos = 0.0; // in frames
    int m_dir = 1;
    double m_duration = 0.0;
    std::atomic<double> m_position{0.0};
    bool m_playing = false;
    int m_lastFrame = -1;
    bool m_reportedError = false;
};
