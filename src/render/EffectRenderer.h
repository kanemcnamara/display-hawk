#pragma once

#include "model/Project.h"

#include <QHash>
#include <rhi/qrhi.h>

// Runs effect chains (shaders/fx.frag) on the render thread. Each chain has its own pair of
// ping-pong targets, keyed by the caller (a layer id, or the composition). Not thread-safe.
class IsfRenderer;

class EffectRenderer
{
public:
    // A live value that replaces one effect parameter (audio-reactive modulation).
    struct Override {
        int effect = 0;
        int param = 0;
        double value = 0.0;
    };

    bool init(QRhi *rhi);
    void release();
    void newFrame() { m_offset = 0; }

    // Call once per frame before apply(): the project whose LED walls the panel effects follow.
    void setFrame(qint64 now, const Project *project) { m_now = now; m_project = project; }
    void setIsf(IsfRenderer *isf) { m_isf = isf; }

    // Applies the enabled effects of `chain` to `input` (premultiplied, rendered, `size` pixels) and
    // returns the result: `input` itself when nothing is enabled. `timeSec` drives animated effects.
    QRhiTexture *apply(QRhiCommandBuffer *cb, const QString &key, QRhiTexture *input, const QSize &size,
                       const QVector<EffectInstance> &chain, const QVector<Override> &overrides, double timeSec);

    // Must be called before a texture that may have been an effect input is destroyed.
    void forgetTexture(QRhiTexture *tex);

    // Frees chains not used for `idleMs`.
    void collectGarbage(qint64 now, qint64 idleMs);

private:
    struct Chain {
        QRhiTexture *tex[2] = {nullptr, nullptr};
        QRhiTextureRenderTarget *rt[2] = {nullptr, nullptr};
        QRhiRenderPassDescriptor *rp = nullptr;
        QSize size;
        qint64 lastUsed = 0;
        QHash<int, QRhiTexture *> history; // effect index -> its previous output (trails)
    };
    bool ensureChain(Chain &c, const QSize &size);
    QRhiTexture *historyFor(Chain &c, int effect, const QSize &size, QRhiResourceUpdateBatch *u);
    void releaseChain(Chain &c, bool deferred);
    QRhiGraphicsPipeline *pipelineFor(QRhiRenderPassDescriptor *rp);
    QRhiShaderResourceBindings *srbFor(QRhiTexture *tex, QRhiTexture *history = nullptr);
    QRhiShaderResourceBindings *newSrb(QRhiTexture *tex, QRhiTexture *ids, QRhiTexture *table, QRhiTexture *history);
    void clearSrbs();
    // Rebuilds the LED panel textures when the walls or the composition size changed; the uploads go
    // into m_pendingUpload, which the same apply() submits.
    void syncPanels(const Project &project, const QSize &compSize);

    QRhi *m_rhi = nullptr;
    QShader m_vs, m_fs;
    QRhiBuffer *m_ubuf = nullptr;
    QRhiSampler *m_linear = nullptr;
    QRhiSampler *m_nearest = nullptr;
    QRhiTexture *m_panelId = nullptr;    // nullptr until the first project: the placeholders are bound
    QRhiTexture *m_panelTable = nullptr;
    QRhiTexture *m_placeholderId = nullptr;
    QRhiTexture *m_placeholderTable = nullptr;
    int m_panelCount = 0;
    QByteArray m_panelKey;
    QRhiShaderResourceBindings *m_layoutSrb = nullptr; // placeholders only; kept for the pipeline's lifetime
    QHash<QPair<QRhiTexture *, QRhiTexture *>, QRhiShaderResourceBindings *> m_srbs; // (input, history)
    QRhiGraphicsPipeline *m_pipeline = nullptr;
    QVector<quint32> m_pipelineFormat;
    QHash<QString, Chain> m_chains;
    quint32 m_stride = 0;
    quint32 m_offset = 0;
    qint64 m_now = 0;
    const Project *m_project = nullptr;
    IsfRenderer *m_isf = nullptr;
    bool m_warnedFull = false;
    QRhiResourceUpdateBatch *m_pendingUpload = nullptr;
};
