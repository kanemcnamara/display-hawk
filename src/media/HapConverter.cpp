#include "media/HapConverter.h"

#include <QAudioBuffer>
#include <QAudioBufferOutput>
#include <QAudioFormat>
#include <QDir>
#include <QFileInfo>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QThread>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>

namespace {

constexpr double kDecodeRate = 8.0; // ahead of the encoder; the decoder waits for each frame anyway

} // namespace

HapConverter::HapConverter(QObject *parent)
    : QObject(parent)
{
    // Leave a core for the show: the render thread and the UI keep running while converting.
    m_pool.setMaxThreadCount(qMax(1, QThread::idealThreadCount() - 2));
    m_pool.setThreadPriority(QThread::LowPriority);
}

HapConverter::~HapConverter()
{
    cancel();
    // The decoder threads end with the player: before the lock and pool they use go.
    const QObjectList kids = children();
    qDeleteAll(kids);
}

QString HapConverter::defaultTarget(const QString &source, Hap::Format format, const QString &folder)
{
    const QFileInfo info(source);
    const QString dir = folder.isEmpty() ? info.absolutePath() : folder;
    const QString base = QStringLiteral("%1 (%2)").arg(info.completeBaseName(), Hap::formatName(format));
    QString path = QDir(dir).filePath(base + QStringLiteral(".mov"));
    for (int n = 2; QFileInfo::exists(path); ++n)
        path = QDir(dir).filePath(QStringLiteral("%1 %2.mov").arg(base).arg(n));
    return path;
}

bool HapConverter::start(const QString &source, const QString &target, const Options &options)
{
    if (m_running)
        return false;
    m_options = options;
    m_target = target;
    m_fps = 30.0;
    m_durationUs = 0;
    m_firstUs = -1;
    m_nextFrame = 0;
    m_lastSample.clear();
    m_opened = false;
    m_done = false;
    m_cancel = false;
    m_size = QSize();
    m_writer = std::make_unique<MovWriter>();
    m_error.clear();
    m_running = true;

    m_player = new QMediaPlayer(this);
    m_sink = new QVideoSink(this);
    m_player->setVideoOutput(m_sink);
    if (options.audio) {
        QAudioFormat format;
        format.setSampleRate(48000);
        format.setChannelCount(2);
        format.setSampleFormat(QAudioFormat::Int16);
        m_audio = new QAudioBufferOutput(format, this);
        m_player->setAudioBufferOutput(m_audio);
        // Direct: buffers are written as they are decoded, on the decoder's thread.
        connect(m_audio, &QAudioBufferOutput::audioBufferReceived, this, &HapConverter::onAudio, Qt::DirectConnection);
    }
    connect(m_sink, &QVideoSink::videoFrameChanged, this, &HapConverter::onFrame, Qt::DirectConnection);
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::LoadedMedia && !m_player->isPlaying())
            onLoaded();
        else if (status == QMediaPlayer::EndOfMedia)
            complete(true, QString());
        else if (status == QMediaPlayer::InvalidMedia)
            complete(false, tr("cannot read this file"));
    });
    connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &message) {
        complete(false, message);
    });
    m_player->setSource(QUrl::fromLocalFile(source));
    return true;
}

void HapConverter::onLoaded()
{
    if (!m_player->hasVideo()) {
        complete(false, tr("no video in this file"));
        return;
    }
    const double fps = m_player->metaData().value(QMediaMetaData::VideoFrameRate).toDouble();
    m_fps = fps > 1.0 && fps < 1000.0 ? fps : 30.0;
    m_durationUs = m_player->duration() * 1000;
    m_player->setPlaybackRate(kDecodeRate);
    m_player->play();
}

void HapConverter::onFrame(const QVideoFrame &frame)
{
    if (m_cancel || !frame.isValid())
        return;
    QImage image = frame.toImage().convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull())
        return;
    // Frames arrive one at a time on the decoder thread, so the size needs no lock.
    if (m_size.isEmpty())
        m_size = image.size();
    else if (image.size() != m_size)
        image = image.scaled(m_size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    // Encoding happens here, outside the lock: the audio keeps flowing meanwhile.
    const QByteArray texture = Hap::compressTexture(image, m_options.format, &m_pool);
    const int chunks = texture.size() > (256 << 10) ? qBound(1, m_pool.maxThreadCount(), 8) : 1;
    const QByteArray sample = Hap::makeFrame(texture, m_options.format, chunks, &m_pool);

    QMutexLocker lock(&m_mutex);
    if (m_done || m_cancel)
        return;
    if (!m_opened) {
        MovWriter::Video v;
        v.fourcc = Hap::fourcc(m_options.format);
        v.size = image.size();
        v.fps = m_fps;
        v.depth = m_options.format == Hap::Format::HapAlpha ? 32 : 24;
        v.compressorName = Hap::formatName(m_options.format).toLatin1();
        QString error;
        if (!m_writer->open(m_target, v, &error)) {
            m_error = error;
            m_cancel = true;
            QMetaObject::invokeMethod(this, [this] { complete(false, m_error); }, Qt::QueuedConnection);
            return;
        }
        m_opened = true;
    }
    // Frames go where their time says: a gap (a dropped or missing frame) repeats the last one.
    const qint64 t = frame.startTime();
    if (m_firstUs < 0)
        m_firstUs = t;
    const int index = t >= 0 ? int(qRound64(double(t - m_firstUs) * m_fps / 1e6)) : m_nextFrame;
    if (index < m_nextFrame)
        return; // a frame we already have
    while (m_nextFrame < index && !m_lastSample.isEmpty()) {
        m_writer->writeVideoFrame(m_lastSample);
        ++m_nextFrame;
    }
    m_writer->writeVideoFrame(sample);
    m_nextFrame = index + 1;
    m_lastSample = sample;
    const double fraction = m_durationUs > 0 ? qBound(0.0, double(t - qMax<qint64>(0, m_firstUs)) / m_durationUs, 1.0) : 0.0;
    QMetaObject::invokeMethod(this, [this, fraction] { emit progress(fraction); }, Qt::QueuedConnection);
}

void HapConverter::onAudio(const QAudioBuffer &buffer)
{
    if (m_cancel || !buffer.isValid())
        return;
    const QAudioFormat format = buffer.format();
    const int channels = format.channelCount();
    if (channels <= 0 || format.sampleRate() <= 0)
        return;
    // 16-bit PCM for the file, whatever the decoder hands over.
    QByteArray pcm;
    if (format.sampleFormat() == QAudioFormat::Int16) {
        pcm = QByteArray(buffer.constData<char>(), buffer.byteCount());
    } else if (format.sampleFormat() == QAudioFormat::Float) {
        const float *f = buffer.constData<float>();
        const qsizetype n = buffer.sampleCount();
        pcm.resize(n * 2);
        auto *out = reinterpret_cast<qint16 *>(pcm.data());
        for (qsizetype i = 0; i < n; ++i)
            out[i] = qint16(qBound(-32768, qRound(f[i] * 32767.0f), 32767));
    } else {
        return;
    }
    QMutexLocker lock(&m_mutex);
    if (m_done)
        return;
    if (m_writer->audioSamplesWritten() == 0)
        m_writer->setAudioFormat(format.sampleRate(), channels);
    m_writer->writeAudio(pcm);
}

void HapConverter::cancel()
{
    if (!m_running)
        return;
    m_cancel = true;
    complete(false, tr("cancelled"));
}

void HapConverter::complete(bool ok, const QString &message)
{
    if (!m_running)
        return;
    m_running = false;
    // Stop decoding first: the decoder thread may be inside onFrame; the lock waits for it.
    if (m_player)
        m_player->stop();
    QString result = message;
    {
        QMutexLocker lock(&m_mutex);
        m_done = true;
        if (ok && m_opened) {
            QString error;
            ok = m_writer->finish(&error);
            if (!ok)
                result = error;
            else
                result = tr("%1 frames").arg(m_writer->framesWritten());
        } else {
            if (ok)
                result = tr("no frames decoded");
            ok = false;
            m_writer->abort();
        }
    }
    // Late callbacks from the decoder threads find m_done set; no more are delivered.
    if (m_sink)
        disconnect(m_sink, nullptr, this, nullptr);
    if (m_audio)
        disconnect(m_audio, nullptr, this, nullptr);
    if (m_player) {
        m_player->deleteLater();
        m_player = nullptr;
    }
    if (m_sink) {
        m_sink->deleteLater();
        m_sink = nullptr;
    }
    if (m_audio) {
        m_audio->deleteLater();
        m_audio = nullptr;
    }
    emit finished(ok, result);
}
