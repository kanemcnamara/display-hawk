#include "media/HapPlayer.h"

#include <QTimer>
#include <QtMath>

HapPlayer::HapPlayer(QObject *parent)
    : QObject(parent)
    , m_timer(new QTimer(this))
{
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, &HapPlayer::tick);
}

bool HapPlayer::open(const QString &path, QString *error)
{
    if (!m_reader.open(path, error))
        return false;
    if (!m_reader.codec().startsWith("Hap")) {
        if (error)
            *error = QStringLiteral("not a HAP movie (codec %1)").arg(QString::fromLatin1(m_reader.codec()));
        return false;
    }
    // Poll at twice the frame rate so a frame is never more than half a frame late.
    m_timer->setInterval(qMax(1, qFloor(500.0 / qMax(1.0, m_reader.frameRate()))));
    return true;
}

qint64 HapPlayer::positionNs() const
{
    return m_baseNs + (m_playing ? m_clock.nsecsElapsed() : 0);
}

void HapPlayer::play()
{
    if (m_playing)
        return;
    m_playing = true;
    m_clock.start();
    m_timer->start();
    tick();
    emit stateChanged(true);
}

void HapPlayer::pause()
{
    if (!m_playing)
        return;
    m_baseNs = positionNs();
    m_playing = false;
    m_timer->stop();
    emit stateChanged(false);
}

void HapPlayer::restart()
{
    m_baseNs = 0;
    m_lastFrame = -1;
    m_clock.start();
    if (!m_playing)
        play();
    else
        tick();
}

void HapPlayer::setLoop(bool loop)
{
    m_loop = loop;
}

void HapPlayer::stop()
{
    m_timer->stop();
    m_playing = false;
    m_reader.close();
}

void HapPlayer::tick()
{
    const int count = m_reader.frameCount();
    if (count == 0)
        return;
    qint64 index = qint64(positionNs() * m_reader.frameRate() / 1e9);
    if (index >= count) {
        if (m_loop) {
            index %= count;
        } else {
            index = count - 1;
            pause();
        }
    }
    if (index == m_lastFrame)
        return;
    m_lastFrame = int(index);

    HapFrame f;
    QString err;
    if (!decodeHapFrame(m_reader.readFrame(int(index)), m_reader.size(), f, &err)) {
        if (!m_reportedError) {
            m_reportedError = true;
            qWarning("HAP frame %lld: %s", index, qPrintable(err));
            emit error(err);
        }
        return;
    }
    f.index = int(index);
    emit frame(f);
}
