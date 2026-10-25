#include "media/OutputRecorder.h"

#include "render/RenderEngine.h"

#include <QMediaCaptureSession>
#include <QMediaFormat>
#include <QMediaRecorder>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoFrameInput>

OutputRecorder::OutputRecorder(RenderEngine *engine, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
{
    connect(m_engine, &RenderEngine::recordFrame, this, &OutputRecorder::onFrame);
}

OutputRecorder::~OutputRecorder()
{
    stop();
}

bool OutputRecorder::start(const QString &path, int fps, QString *error)
{
    stop();
    m_session = new QMediaCaptureSession(this);
    m_input = new QVideoFrameInput(this);
    m_recorder = new QMediaRecorder(this);
    m_session->setVideoFrameInput(m_input);
    m_session->setRecorder(m_recorder);
    QMediaFormat format(QMediaFormat::MPEG4);
    format.setVideoCodec(QMediaFormat::VideoCodec::H264);
    m_recorder->setMediaFormat(format);
    m_recorder->setQuality(QMediaRecorder::VeryHighQuality);
    m_recorder->setVideoFrameRate(fps);
    m_recorder->setOutputLocation(QUrl::fromLocalFile(path));
    connect(m_input, &QVideoFrameInput::readyToSendVideoFrame, this, [this] { m_ready = true; });
    connect(m_recorder, &QMediaRecorder::errorOccurred, this, [this](QMediaRecorder::Error, const QString &msg) {
        qWarning("Recording: %s", qPrintable(msg));
        emit errorOccurred(msg);
    });
    connect(m_recorder, &QMediaRecorder::recorderStateChanged, this, &OutputRecorder::stateChanged);
    m_fps = fps;
    m_frames = 0;
    m_ready = true;
    m_path = path;
    m_recorder->record();
    if (m_recorder->error() != QMediaRecorder::NoError) {
        if (error)
            *error = m_recorder->errorString();
        stop();
        return false;
    }
    m_recording = true;
    m_clock.start();
    m_engine->setRecording(true, fps);
    qInfo("Recording the composition to %s at %d fps", qPrintable(path), fps);
    emit stateChanged();
    return true;
}

void OutputRecorder::stop()
{
    if (!m_session)
        return;
    m_engine->setRecording(false, m_fps);
    if (m_recorder)
        m_recorder->stop();
    m_recording = false;
    delete m_recorder;
    delete m_input;
    delete m_session;
    m_recorder = nullptr;
    m_input = nullptr;
    m_session = nullptr;
    if (!m_path.isEmpty())
        qInfo("Recording stopped: %d frames in %s", m_frames, qPrintable(m_path));
    emit stateChanged();
}

void OutputRecorder::onFrame(const QImage &image)
{
    if (!m_recording || !m_input || image.isNull())
        return;
    if (!m_ready) // the encoder is behind: drop this frame rather than queue up
        return;
    QVideoFrame frame(image.convertToFormat(QImage::Format_RGBA8888));
    // Real-time timestamps: a dropped frame leaves a gap rather than shortening the video.
    const qint64 us = m_clock.nsecsElapsed() / 1000;
    frame.setStartTime(us);
    frame.setEndTime(us + 1000000 / m_fps);
    if (m_input->sendVideoFrame(frame))
        ++m_frames;
    else
        m_ready = false; // until readyToSendVideoFrame
}
