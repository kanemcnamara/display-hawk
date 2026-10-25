#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QRectF>
#include <QSurface>
#include <QVector>

class Document;
class MediaSource;
class OutputWindow;
class QTimer;
class QOffscreenSurface;
class RenderThread;

// What an editor view wants to see: a region of the composition (input) or of one screen (output),
// rendered at the view's pixel resolution.
struct PreviewRequest {
    bool enabled = false;
    bool outputMode = false;
    int screen = -1;
    QRectF region;   // canvas pixels
    QSize pixelSize; // device pixels to render at

    bool operator==(const PreviewRequest &o) const
    {
        return enabled == o.enabled && outputMode == o.outputMode && screen == o.screen && region == o.region
               && pixelSize == o.pixelSize;
    }
    bool operator!=(const PreviewRequest &o) const { return !(*this == o); }
};

// A live value for one effect parameter (audio-reactive modulation). layer -1 = composition.
struct EffectParamMod {
    int layer = -1;
    int effect = 0;
    int param = 0;
    double value = 0.0;
};

// GUI-thread facade over the render thread. The render thread owns the QRhi (Metal on macOS,
// Direct3D 11 on Windows by default), renders the composition, presents every output with vsync
// and sends preview images back for the editor.
class RenderEngine : public QObject
{
    Q_OBJECT
public:
    explicit RenderEngine(Document *doc, QObject *parent = nullptr);
    ~RenderEngine() override;

    bool start(QString *error);
    void stop();

    QString backendName() const { return m_backendName; }
    QSurface::SurfaceType surfaceType() const { return m_surfaceType; }
    // Decoder for a playing media clip, or nullptr when the clip is not playing.
    MediaSource *mediaFor(const QString &clipId) const { return m_media.value(clipId); }

    // Audio: the level a clip is currently playing at (0..1, slider scale, after fades), and the
    // output device (empty = system default; remembered on this machine).
    double audioLevel(const QString &clipId) const { return m_volumes.value(clipId).current; }
    void setAudioDevice(const QByteArray &deviceId);
    QByteArray audioDeviceId() const { return m_audioDeviceId; }

    void addOutput(OutputWindow *window);
    // Blocks until the render thread has released the window's swapchain; delete the window afterwards.
    void removeOutput(OutputWindow *window);

    void setPreview(int view, const PreviewRequest &request);

    // Screens published as NDI sources (screen index -> NDI source name); empty stops them all.
    void setNdiOutputs(const QVector<QPair<int, QString>> &outputs);
    // Screens shared with other apps on this machine (Spout / Syphon), screen index -> name.
    void setShareOutputs(const QVector<QPair<int, QString>> &outputs);
    // Draws every slice's outline, name, port and the port routes over the outputs (for the LED tech).
    void setIdentifySlices(bool on);
    // The preview monitor: renders this clip (by id; empty = off) at `size`, its media running muted
    // if it is not playing anyway.
    void setPreviewClip(const QString &clipId, const QSize &size);
    // Recording: the composition read back `fps` times a second, delivered as recordFrame.
    void setRecording(bool on, int fps);
    QString previewClip() const { return m_previewClip; }
    bool identifySlices() const { return m_identifySlices; }

    // Live modulation on top of the faders (audio-reactive): master and per-layer opacity
    // multipliers, and effect parameter values. (1, {}, {}) means none.
    void setModulation(double master, const QVector<double> &layerOpacity,
                       const QVector<EffectParamMod> &effects = {});

signals:
    void mediaStateChanged();
    // A playing media clip went round its loop (ended = false) or, playing once, reached its end.
    void clipFinished(const QString &clipId, bool ended);
    void clipPreviewReady(const QString &clipId, const QImage &image);
    void recordFrame(const QImage &image);
    void previewReady(int view, const QImage &image, const QRectF &region, bool outputMode, int screen);
    void statsUpdated(int fps, const QSize &compSize);

private:
    void pushProject();
    void syncMedia();
    void releaseMedia(const QString &clipId);
    void stepVolumes();
    void rampTo(const QString &clipId, double target, int ms);

    Document *m_doc;
    QHash<QString, MediaSource *> m_media; // clip id -> decoder, for clips that are playing
    QHash<QString, QString> m_mediaPaths;
    struct Ramp {
        double current = 0.0;
        double target = 0.0;
        double step = 1.0; // per tick
    };
    QHash<QString, Ramp> m_volumes;     // clip id -> audio level
    QHash<QString, int> m_mediaLayer;   // clip id -> layer it last played on (for fade-out time)
    QTimer *m_volumeTimer = nullptr;
    QByteArray m_audioDeviceId;
    RenderThread *m_thread = nullptr;
    QOffscreenSurface *m_fallbackSurface = nullptr;
    QString m_backendName;
    bool m_identifySlices = false;
    QString m_previewClip;
    QSize m_previewSize;
    void pushClipPreview();
    QSurface::SurfaceType m_surfaceType = QSurface::OpenGLSurface;
};
