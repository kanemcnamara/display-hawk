#pragma once

#include "model/Project.h"
#include "render/RenderEngine.h"
#include "render/RhiQuad.h"
#include "render/YuvConverter.h"

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
    // Media for one clip; callable from any thread (decoder threads deliver video frames).
    void setClipFrame(const QString &clipId, const QVideoFrame &frame);
    void setClipImage(const QString &clipId, const QImage &image);
    void setClipHap(const QString &clipId, const HapFrame &frame);
    void dropClipMedia(const QString &clipId);
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
    void appendClipDraw(QRhiCommandBuffer *cb, QVector<RhiQuad::Params> &draws, QRhiResourceUpdateBatch *u,
                        const Clip &clip, const QRect &target, const QSize &compSize, float opacity);
    void collectGarbage(qint64 now);
    void renderPreviews(QRhiCommandBuffer *cb);
    void renderOutput(QRhiCommandBuffer *cb, Output &o);
    QVector<RhiQuad::Params> screenDraws(const Screen &screen, const QSizeF &target, const QRectF &screenRect) const;
    void deliverReadbacks();
    QByteArray patternKey(const Clip &clip) const;
    QByteArray overlayKey() const;

    const QRhi::Implementation m_backend;
    QOffscreenSurface *m_fallbackSurface;

    // Shared with the GUI thread, guarded by m_mutex.
    QMutex m_mutex;
    QWaitCondition m_removed;
    bool m_stop = false;
    Project m_sharedProject;
    bool m_projectDirty = true;
    struct SharedMedia {
        QVideoFrame frame;
        QImage image;
        bool remove = false;
        HapFrame hap;
    };
    QHash<QString, SharedMedia> m_sharedMedia; // clip id -> newest media not yet taken by run()
    PreviewRequest m_sharedPreview[2];
    QVector<OutputWindow *> m_toAdd;
    QVector<OutputWindow *> m_toRemove;

    QSemaphore m_initDone;
    bool m_initOk = false;
    QString m_initError;

    // Render-thread only.
    QRhi *m_rhi = nullptr;
    RhiQuad m_quad;
    YuvConverter m_yuv;
    // DISPLAYHAWK_PERF=1 logs how long video frames take to prepare on the render thread.
    bool m_perfLog = false;
    qint64 m_perfNs = 0;
    int m_perfFrames = 0;
    int m_perfGpuFrames = 0;
    Project m_project;
    PreviewRequest m_preview[2];
    QVector<Output> m_outputs;
    Target m_comp;
    Target m_previewTarget[2];
    // Per clip: its pattern image or its newest media frame.
    struct ClipTex {
        QRhiTexture *tex = nullptr;     // patterns, images and CPU-converted video
        YuvConverter::Target yuv;       // video converted on the GPU
        bool useYuv = false;            // the newest frame went through the GPU conversion pass
        QRhiTexture *hapTex = nullptr;  // HAP / HAP Alpha / HAP R blocks, sampled directly
        bool useHap = false;
        HapFrame hap;
        QSize mediaSize;                // picture size (HAP textures are padded to multiples of 4)
        QSizeF srcScale{1.0, 1.0};      // picture size / texture size
        QByteArray patternKey;
        QVideoFrame frame;
        QImage image;
        bool newMedia = false;
        bool hasMedia = false;
        qint64 lastUsed = 0;
    };
    QHash<QString, ClipTex> m_clipTex;
    void releaseClipTex(ClipTex &ct, bool deferred);
    // Per layer: what is playing, the outgoing clip during a crossfade, and the layer texture.
    struct LayerState {
        Clip current;
        Clip previous;
        qint64 since = 0;
        int duration = 0;
        qint64 lastUsed = 0;
        Target target;
    };
    QHash<QString, LayerState> m_layers;
    QElapsedTimer m_clock;
    qint64 m_lastGc = 0;
    QRhiTexture *m_overlayTex = nullptr;
    QByteArray m_overlayKey;
    QRhiTexture *m_maskTex = nullptr;
    QByteArray m_maskKey;
    QRhiReadbackResult m_readback[2];
    bool m_readbackPending[2] = {false, false};
    bool m_readbackDone[2] = {false, false};
    PreviewRequest m_readbackRequest[2];
    QElapsedTimer m_previewClock[2];
};
