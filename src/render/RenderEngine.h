#pragma once

#include <QImage>
#include <QObject>
#include <QRectF>
#include <QSurface>

class Document;
class MediaSource;
class OutputWindow;
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
    MediaSource *media() const { return m_media; }

    void addOutput(OutputWindow *window);
    // Blocks until the render thread has released the window's swapchain; delete the window afterwards.
    void removeOutput(OutputWindow *window);

    void setPreview(int view, const PreviewRequest &request);

signals:
    void previewReady(int view, const QImage &image, const QRectF &region, bool outputMode, int screen);
    void statsUpdated(int fps, const QSize &compSize);

private:
    void pushProject();
    void reloadMedia();

    Document *m_doc;
    MediaSource *m_media;
    RenderThread *m_thread = nullptr;
    QOffscreenSurface *m_fallbackSurface = nullptr;
    QString m_backendName;
    QSurface::SurfaceType m_surfaceType = QSurface::OpenGLSurface;
};
