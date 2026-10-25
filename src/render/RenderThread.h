#pragma once

#include "model/Project.h"
#include "render/RenderEngine.h"
#include "render/RhiQuad.h"

#include <QElapsedTimer>
#include <QMutex>
#include <QSemaphore>
#include <QThread>
#include <QVideoFrame>
#include <QWaitCondition>

// Owns the QRhi and every GPU resource. All public methods are called from the GUI thread and only
// touch the mutex-protected "shared" state; run() copies it at the start of each frame.
class RenderThread : public QThread
{
    Q_OBJECT
public:
    RenderThread(QRhi::Implementation backend, QOffscreenSurface *fallbackSurface, QObject *parent = nullptr);

    bool waitForInit(QString *error);
    void requestStop();

    void setProject(const Project &project);
    void setVideoFrame(const QVideoFrame &frame); // any thread
    void setStillImage(const QImage &image);      // any thread
    void setPreview(int view, const PreviewRequest &request);
    void addOutput(OutputWindow *window);
    void removeOutput(OutputWindow *window);

signals:
    void previewReady(int view, const QImage &image, const QRectF &region, bool outputMode, int screen);
    void statsUpdated(int fps, const QSize &compSize);

protected:
    void run() override;

private:
    struct Output {
        OutputWindow *window = nullptr;
        QRhiSwapChain *swapChain = nullptr;
        QRhiRenderPassDescriptor *rp = nullptr;
    };
    struct Target {
        QRhiTexture *tex = nullptr;
        QRhiTextureRenderTarget *rt = nullptr;
        QRhiRenderPassDescriptor *rp = nullptr;
        QSize size;
    };

    bool createRhi();
    void syncState();
    bool ensureSwapChain(Output &o);
    void releaseOutput(Output &o);
    bool ensureTarget(Target &t, const QSize &size, bool readable);
    void releaseTarget(Target &t);
    void uploadImage(QRhiResourceUpdateBatch *u, QRhiTexture *&tex, const QImage &image);
    void renderComposition(QRhiCommandBuffer *cb);
    void renderPreviews(QRhiCommandBuffer *cb);
    void renderOutput(QRhiCommandBuffer *cb, Output &o);
    QVector<RhiQuad::Params> screenDraws(const Screen &screen, const QSizeF &target, const QRectF &screenRect) const;
    void deliverReadbacks();
    QByteArray patternKey() const;
    QByteArray overlayKey() const;

    const QRhi::Implementation m_backend;
    QOffscreenSurface *m_fallbackSurface;

    // Shared with the GUI thread, guarded by m_mutex.
    QMutex m_mutex;
    QWaitCondition m_removed;
    bool m_stop = false;
    Project m_sharedProject;
    bool m_projectDirty = true;
    QVideoFrame m_sharedFrame;
    QImage m_sharedImage;
    bool m_mediaDirty = false;
    PreviewRequest m_sharedPreview[2];
    QVector<OutputWindow *> m_toAdd;
    QVector<OutputWindow *> m_toRemove;

    QSemaphore m_initDone;
    bool m_initOk = false;
    QString m_initError;

    // Render-thread only.
    QRhi *m_rhi = nullptr;
    RhiQuad m_quad;
    Project m_project;
    PreviewRequest m_preview[2];
    QVector<Output> m_outputs;
    Target m_comp;
    Target m_previewTarget[2];
    QRhiTexture *m_mediaTex = nullptr;
    QRhiTexture *m_patternTex = nullptr;
    QRhiTexture *m_overlayTex = nullptr;
    QByteArray m_patternKey;
    QByteArray m_overlayKey;
    QVideoFrame m_videoFrame;
    QImage m_stillImage;
    bool m_newMedia = false;
    bool m_hasMedia = false;
    QRhiReadbackResult m_readback[2];
    bool m_readbackPending[2] = {false, false};
    bool m_readbackDone[2] = {false, false};
    PreviewRequest m_readbackRequest[2];
    QElapsedTimer m_previewClock[2];
};
