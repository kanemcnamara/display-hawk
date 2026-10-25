#pragma once

#include <QHash>
#include <QString>
#include <QVector>
#include <rhi/qrhi.h>

// Compiles ISF shaders (render/Isf.h) on first use and renders them: as an effect pass (with an
// input image) or as a generator (without). Render thread only.
class IsfRenderer
{
public:
    bool init(QRhi *rhi);
    void release();
    void newFrame() { m_offset = 0; }

    // One full-target pass into `out`. `params` are the effect/generator parameter values in the
    // catalogue's order; `input` is the effect's input (premultiplied, rendered) or nullptr.
    // `premultiplyOutput`: effects produce premultiplied colour, generators straight alpha.
    // Returns false if the shader does not compile (logged once) or anything else fails.
    bool render(QRhiCommandBuffer *cb, QRhiTextureRenderTarget *out, const QString &id, const QVector<double> &params,
                QRhiTexture *input, double timeSec, bool premultiplyOutput);
    void forgetTexture(QRhiTexture *tex);
    QString error(const QString &id) const { return m_programs.value(id).error; }

private:
    struct Program {
        QShader vs, fs;
        QString error;
        bool tried = false;
        QRhiGraphicsPipeline *pipeline = nullptr;
        QVector<quint32> format;
        QRhiShaderResourceBindings *layout = nullptr;
        double lastTime = -1;
        int frames = 0;
    };
    Program &program(const QString &id);
    QRhiShaderResourceBindings *srbFor(Program &p, const QString &id, QRhiTexture *input);

    QRhi *m_rhi = nullptr;
    QRhiBuffer *m_ubuf = nullptr;
    QRhiSampler *m_sampler = nullptr;
    QRhiTexture *m_blank = nullptr;
    bool m_blankUploaded = false;
    QHash<QString, Program> m_programs;
    QHash<QPair<QString, QRhiTexture *>, QRhiShaderResourceBindings *> m_srbs;
    quint32 m_stride = 0;
    quint32 m_offset = 0;
};
