#pragma once

#include "model/Project.h"
#include "render/BlendCompositor.h"
#include "render/EffectRenderer.h"
#include "render/GeneratorRenderer.h"
#include "render/IsfRenderer.h"
#include "render/RenderEngine.h"
#include "render/RhiQuad.h"
#include "media/TextureShare.h"
#include "render/YuvConverter.h"

#include <QElapsedTimer>
#include <QMutex>
#include <QSemaphore>
#include <QThread>
#include <QVideoFrame>
#include <QWaitCondition>

class NdiSender;

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
    void setNdiOutputs(const QVector<QPair<int, QString>> &outputs);
    void setShareOutputs(const QVector<QPair<int, QString>> &outputs);
    void setModulation(double master, const QVector<double> &layerOpacity, const QVector<EffectParamMod> &effects);
    void setIdentifySlices(bool on);
    // Preview monitor: renders `clip` alone (fitted into `area` of the composition) at `size` and
    // emits clipPreviewReady; an empty clip turns it off.
    void setClipPreview(const Clip &clip, const QRect &area, const QSize &size);
    void setRecording(bool on, int fps);
    void setDmxSampling(bool on, int fps);
    void addOutput(OutputWindow *window);
    void removeOutput(OutputWindow *window);

signals:
    void previewReady(int view, const QImage &image, const QRectF &region, bool outputMode, int screen);
    void statsUpdated(int fps, const QSize &compSize);
    void clipPreviewReady(const QString &clipId, const QImage &image);
    void recordFrame(const QImage &image);
    void dmxFrame(const QImage &image); // the composition, scaled down, for pixel mapping

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
        // Mixers draw onto earlier passes: a second render target that keeps the contents.
        QRhiTextureRenderTarget *rtLoad = nullptr;
        QRhiRenderPassDescriptor *rpLoad = nullptr;
        QSize size;
        QRhiTexture::Format format = QRhiTexture::RGBA8;
    };
    // Accumulates layers: fixed-function blends are batched into one pass; a shader blend mode
    // reads what is there so far and writes into the other texture of the pair.
    struct Mixer {
        Target t[2];
        int cur = 0;
        bool started = false;
        bool drew = false;
        QColor clear;
        QVector<RhiQuad::Params> pending;
        qint64 lastUsed = 0;
    };
    void mixBegin(Mixer &m, const QColor &clear);
    void mixFlush(QRhiCommandBuffer *cb, Mixer &m, const QSize &size, QRhiResourceUpdateBatch *u = nullptr);
    void mixLayer(QRhiCommandBuffer *cb, Mixer &m, const RhiQuad::Params &quad, BlendMode mode, const QSize &size);
    QRhiTexture *mixFinish(QRhiCommandBuffer *cb, Mixer &m, const QSize &size, QRhiResourceUpdateBatch *u = nullptr);
    void releaseMixer(Mixer &m, bool deferred);

    bool createRhi();
    void syncState();
    bool ensureSwapChain(Output &o);
    void releaseOutput(Output &o);
    bool ensureTarget(Target &t, const QSize &size, bool readable, QRhiTexture::Format format = QRhiTexture::RGBA8,
                      bool loadable = false);
    void releaseTarget(Target &t);
    void uploadImage(QRhiResourceUpdateBatch *u, QRhiTexture *&tex, const QImage &image);
    void renderComposition(QRhiCommandBuffer *cb);
    void appendClipDraw(QRhiCommandBuffer *cb, QVector<RhiQuad::Params> &draws, QRhiResourceUpdateBatch *u,
                        const Clip &clip, const QRect &target, const QSize &compSize, float opacity);
    void collectGarbage(qint64 now);
    // Where a clip goes during a transition: shifted, zoomed about the layer's target, cut to a
    // scissor rect (wipes), at an opacity.
    struct Placement {
        QPointF offset;
        double zoom = 1.0;
        QRectF scissor; // null = the whole target
        float opacity = 1.0f;
    };
    void placeClip(QRhiCommandBuffer *cb, QVector<RhiQuad::Params> &draws, QRhiResourceUpdateBatch *u,
                   const Clip &clip, const QRect &target, const QSize &compSize, const Placement &placement,
                   double timeSec);
    static void transformDraws(QVector<RhiQuad::Params> &draws, int from, const QPointF &centre, double scale,
                               const QPointF &offset, double rotation, const QRectF &scissor);
    struct ClipTarget { // clips with effects render on their own first
        Target target;
        qint64 lastUsed = 0;
    };
    QHash<QString, ClipTarget> m_clipTargets;
    void renderPreviews(QRhiCommandBuffer *cb);
    void renderOutput(QRhiCommandBuffer *cb, Output &o);
    QVector<EffectRenderer::Override> effectOverrides(int layer) const;
    QVector<RhiQuad::Params> screenDraws(const Screen &screen, const QSizeF &target, const QRectF &screenRect) const;
    void deliverReadbacks();
    void syncNdiOutputs();
    void renderNdiOutputs(QRhiCommandBuffer *cb);
    void syncShareOutputs();
    void renderShareOutputs(QRhiCommandBuffer *cb);
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
    PreviewRequest m_sharedPreview[kPreviewViews];
    QVector<OutputWindow *> m_toAdd;
    QVector<OutputWindow *> m_toRemove;
    QVector<QPair<int, QString>> m_sharedNdi;
    bool m_ndiDirty = false;
    QVector<QPair<int, QString>> m_sharedShare;
    bool m_shareDirty = false;
    double m_sharedMasterMod = 1.0;
    QVector<double> m_sharedLayerMod;
    QVector<EffectParamMod> m_sharedEffectMod;
    bool m_sharedIdentify = false;
    struct ClipPreview {
        Clip clip;
        QRect area;
        QSize size;
    };
    ClipPreview m_sharedClipPreview, m_clipPreview;
    Target m_clipPreviewLayer, m_clipPreviewOut;
    QRhiReadbackResult m_clipPreviewReadback;
    bool m_clipPreviewPending = false;
    bool m_clipPreviewDone = false;
    QString m_clipPreviewId;
    QElapsedTimer m_clipPreviewClock;
    void renderClipPreview(QRhiCommandBuffer *cb);
    // The composition read back at a steady rate: for recording, and for DMX pixel mapping.
    struct CompReadback {
        bool sharedOn = false;
        int sharedFps = 30;
        bool on = false;
        int fps = 30;
        QSize maxSize;
        Target target;
        QRhiReadbackResult result;
        bool pending = false;
        bool done = false;
        QElapsedTimer clock;
        qint64 next = 0;
    };
    CompReadback m_recordRb{false, 30, false, 30, QSize(3840, 2160)};
    CompReadback m_dmxRb{false, 40, false, 40, QSize(1024, 1024)}; // pixel maps are coarse
    void renderCompReadback(QRhiCommandBuffer *cb, CompReadback &r);
    QImage takeCompReadback(CompReadback &r);
    bool m_identify = false;
    // Per screen while identifying slices: the overlay drawn on top of its output.
    struct ScreenOverlay {
        QRhiTexture *tex = nullptr;
        QByteArray key;
    };
    QVector<ScreenOverlay> m_screenOverlays;
    QVector<ScreenOverlay> m_maskOverlays; // projection masks, per screen (tex null = no masks)
    void syncMaskOverlays(QRhiResourceUpdateBatch *u);
    void syncScreenOverlays(QRhiResourceUpdateBatch *u);
    void releaseScreenOverlays();
    double m_masterMod = 1.0;      // render-thread copies
    QVector<double> m_layerMod;
    QVector<EffectParamMod> m_effectMod;

    QSemaphore m_initDone;
    bool m_initOk = false;
    QString m_initError;

    // Render-thread only.
    QRhi *m_rhi = nullptr;
    RhiQuad m_quad;
    EffectRenderer m_fx;
    YuvConverter m_yuv;
    // DISPLAYHAWK_PERF=1 logs how long video frames take to prepare on the render thread.
    bool m_perfLog = false;
    qint64 m_perfNs = 0;
    int m_perfFrames = 0;
    int m_perfGpuFrames = 0;
    int m_perfZeroCopyFrames = 0;
public:
    QRhi *rhi() const { return m_rhi; } // valid once waitForInit() returned true
    // Video frames counted since start: how many arrived as hardware textures (zero-copy).
    int zeroCopyFramesTotal() const { return m_zeroCopyTotal.loadRelaxed(); }
private:
    QAtomicInt m_zeroCopyTotal;
    Project m_project;
    PreviewRequest m_preview[kPreviewViews];
    QVector<Output> m_outputs;
    Mixer m_compMix;                  // layers over the background
    QHash<QString, Mixer> m_groupMix; // layer groups, by group id
    QSize m_compSize;
    BlendCompositor m_blend;
    GeneratorRenderer m_gen;
    IsfRenderer m_isf;
    Target m_compFinal;               // composition effects, then mask / overlay / master on top
    QRhiTexture *m_compOut = nullptr; // what screens and previews show: the mix or m_compFinal
    Target m_previewTarget[kPreviewViews];
    // Per clip: its pattern image or its newest media frame.
    struct ClipTex {
        QRhiTexture *tex = nullptr;     // patterns, images and CPU-converted video
        YuvConverter::Target yuv;       // video converted on the GPU
        bool useYuv = false;            // the newest frame went through the GPU conversion pass
        TextureReceiver *share = nullptr; // Spout / Syphon input
        bool shareFailed = false;
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
    QRhiTexture *m_compMaskTex = nullptr; // the composition's masks (null = none)
    QByteArray m_compMaskKey;
    QRhiReadbackResult m_readback[kPreviewViews];
    bool m_readbackPending[kPreviewViews] = {};
    bool m_readbackDone[kPreviewViews] = {};
    PreviewRequest m_readbackRequest[kPreviewViews];
    QElapsedTimer m_previewClock[kPreviewViews];

    // Screens published over NDI: rendered off-screen, read back, sent.
    struct NdiOutput {
        int screen = -1;
        QString name;
        Target target;
        NdiSender *sender = nullptr;
        QRhiReadbackResult readback;
        bool pending = false;
        bool done = false;
    };
    QVector<NdiOutput *> m_ndiOutputs;
    QVector<QPair<int, QString>> m_ndiWanted;

    // Screens shared with other apps on this machine (Spout / Syphon): rendered, then published.
    struct ShareOutput {
        int screen = -1;
        QString name;
        Target target;
        TextureSender *sender = nullptr;
        bool failed = false;
    };
    QVector<ShareOutput *> m_shareOutputs;
    QVector<QPair<int, QString>> m_shareWanted;
};
