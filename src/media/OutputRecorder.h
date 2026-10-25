#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QObject>

class QMediaCaptureSession;
class QMediaRecorder;
class QVideoFrameInput;
class RenderEngine;

// Records the composition to a video file (H.264 in MP4 through Qt Multimedia's encoder):
// the render thread reads frames back at the recording's rate and they are handed to the encoder.
class OutputRecorder : public QObject
{
    Q_OBJECT
public:
    explicit OutputRecorder(RenderEngine *engine, QObject *parent = nullptr);
    ~OutputRecorder() override;

    bool start(const QString &path, int fps, QString *error);
    void stop();
    bool isRecording() const { return m_recording; }
    qint64 elapsedMs() const { return m_recording ? m_clock.elapsed() : 0; }
    int framesWritten() const { return m_frames; }
    QString path() const { return m_path; }

signals:
    void stateChanged();
    void errorOccurred(const QString &message);

private:
    void onFrame(const QImage &image);

    RenderEngine *m_engine;
    QMediaCaptureSession *m_session = nullptr;
    QMediaRecorder *m_recorder = nullptr;
    QVideoFrameInput *m_input = nullptr;
    QElapsedTimer m_clock;
    bool m_recording = false;
    bool m_ready = true; // the encoder can take another frame
    int m_frames = 0;
    int m_offered = 0; // frames the renderer delivered (the rest were dropped: encoder busy)
    int m_fps = 30;
    QString m_path;
};
