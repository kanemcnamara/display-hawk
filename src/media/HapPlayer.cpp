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
    m_duration = m_reader.frameCount() / qMax(1.0, m_reader.frameRate());
    // Poll at twice the frame rate (at least 60 Hz, for fast playback) so a frame is never more
    // than half a frame late.
    m_timer->setInterval(qMax(1, qFloor(500.0 / qMax(60.0, m_reader.frameRate()))));
    return true;
}

double HapPlayer::inFrame() const
{
    return qBound(0.0, m_params.in, 1.0) * m_reader.frameCount();
}

double HapPlayer::outFrame() const
{
    // At least one frame between the in and out points.
    return qMax(inFrame() + 1.0, qBound(0.0, m_params.out, 1.0) * m_reader.frameCount());
}

void HapPlayer::play()
{
    if (m_playing)
        return;
    m_playing = true;
    m_clock.start();
    m_lastNs = 0;
    m_timer->start();
    tick();
    emit stateChanged(true);
}

void HapPlayer::pause()
{
    if (!m_playing)
        return;
    m_playing = false;
    m_timer->stop();
    emit stateChanged(false);
}

void HapPlayer::restart()
{
    m_dir = m_params.reverse ? -1 : 1;
    m_pos = m_params.reverse ? outFrame() - 0.001 : inFrame();
    m_lastFrame = -1;
    if (!m_playing)
        play();
    else
        show(true);
}

void HapPlayer::setParams(const PlaybackParams &params)
{
    const bool reverseChanged = params.reverse != m_params.reverse;
    m_params = params;
    if (reverseChanged)
        m_dir = params.reverse ? -1 : 1;
    m_pos = qBound(inFrame(), m_pos, outFrame() - 0.001); // keep inside new in/out points
}

void HapPlayer::seek(double fraction)
{
    m_pos = qBound(inFrame(), fraction * m_reader.frameCount(), outFrame() - 0.001);
    show(true);
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
    const qint64 now = m_clock.nsecsElapsed();
    const double dt = (now - m_lastNs) / 1e9;
    m_lastNs = now;
    if (m_playing)
        m_pos += dt * m_reader.frameRate() * qBound(0.01, m_params.speed, 16.0) * m_dir;

    const double lo = inFrame(), hi = outFrame(), len = hi - lo;
    const bool once = m_params.mode == PlayMode::OnceHold || m_params.mode == PlayMode::OnceClear;
    bool wrapped = false, finished = false;
    if (m_pos >= hi) {
        if (once && m_dir > 0) {
            m_pos = hi - 0.001;
            finished = true;
        } else if (m_params.mode == PlayMode::Bounce) {
            m_pos = qMax(lo, hi - std::fmod(m_pos - hi, len) - 0.001);
            m_dir = -1;
        } else {
            m_pos = lo + std::fmod(m_pos - lo, len);
            wrapped = true;
        }
    } else if (m_pos < lo) {
        if (once && m_dir < 0) {
            m_pos = lo;
            finished = true;
        } else if (m_params.mode == PlayMode::Bounce) {
            m_pos = qMin(hi - 0.001, lo + std::fmod(lo - m_pos, len));
            m_dir = 1;
            wrapped = true; // one full back-and-forth
        } else {
            m_pos = hi - std::fmod(lo - m_pos, len);
            wrapped = true;
        }
    }
    show(false);
    if (wrapped)
        emit looped();
    if (finished) {
        pause();
        emit ended();
    }
}

void HapPlayer::show(bool force)
{
    const int count = m_reader.frameCount();
    if (count == 0)
        return;
    m_position.store(m_pos / count);
    const int index = qBound(0, int(m_pos), count - 1);
    if (index == m_lastFrame && !force)
        return;
    m_lastFrame = index;

    HapFrame f;
    QString err;
    if (!decodeHapFrame(m_reader.readFrame(index), m_reader.size(), f, &err)) {
        if (!m_reportedError) {
            m_reportedError = true;
            qWarning("HAP frame %d: %s", index, qPrintable(err));
            emit error(err);
        }
        return;
    }
    f.index = index;
    emit frame(f);
}
