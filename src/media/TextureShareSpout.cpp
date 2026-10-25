// Spout (Windows): shares Direct3D 11 textures through the vendored SpoutDX SDK.
#include "media/TextureShare.h"

#include <rhi/qrhi_platform.h>

#include "SpoutDX.h"
#include "SpoutSenderNames.h"

#include <set>
#include <string>

namespace {

ID3D11Device *deviceOf(QRhi *rhi)
{
    const auto *h = static_cast<const QRhiD3D11NativeHandles *>(rhi->nativeHandles());
    return h ? static_cast<ID3D11Device *>(h->dev) : nullptr;
}

QRhiTexture::Format formatFor(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        return QRhiTexture::BGRA8;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return QRhiTexture::RGBA16F;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return QRhiTexture::RGBA32F;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return QRhiTexture::RGB10A2;
    default:
        return QRhiTexture::RGBA8;
    }
}

class SpoutSender final : public TextureSender
{
public:
    SpoutSender(QRhi *rhi, const QString &name)
    {
        m_ok = m_spout.OpenDirectX11(deviceOf(rhi)) && m_spout.SetSenderName(name.toLocal8Bit().constData());
    }
    ~SpoutSender() override
    {
        m_spout.ReleaseSender();
        m_spout.CloseDirectX11();
    }
    bool ok() const { return m_ok; }
    QRhiTexture::Format format() const override { return QRhiTexture::RGBA8; }
    void publish(QRhiCommandBuffer *cb, QRhiTexture *texture) override
    {
        // beginExternal flushes QRhi's recorded commands, so the copy reads the finished frame.
        cb->beginExternal();
        m_spout.SendTexture(reinterpret_cast<ID3D11Texture2D *>(texture->nativeTexture().object));
        cb->endExternal();
    }

private:
    spoutDX m_spout;
    bool m_ok = false;
};

class SpoutReceiver final : public TextureReceiver
{
public:
    SpoutReceiver(QRhi *rhi, const QString &source)
        : m_rhi(rhi)
    {
        m_spout.OpenDirectX11(deviceOf(rhi));
        m_spout.SetReceiverName(source.toLocal8Bit().constData());
    }
    ~SpoutReceiver() override
    {
        delete m_wrap;
        m_spout.ReleaseReceiver();
        m_spout.CloseDirectX11();
    }
    QRhiTexture *receive(QRhiCommandBuffer *cb) override
    {
        cb->beginExternal();
        const bool connected = m_spout.ReceiveTexture(); // copies the sender's frame into SpoutDX's texture
        const bool resized = connected && m_spout.IsUpdated();
        cb->endExternal();
        if (!connected)
            return nullptr;
        ID3D11Texture2D *t = m_spout.GetSenderTexture();
        if (!t)
            return nullptr;
        if (resized || t != m_wrapped || !m_wrap) {
            if (m_wrap)
                m_wrap->deleteLater();
            m_wrap = m_rhi->newTexture(formatFor(m_spout.GetSenderFormat()),
                                       QSize(int(m_spout.GetSenderWidth()), int(m_spout.GetSenderHeight())));
            if (!m_wrap->createFrom({quint64(reinterpret_cast<quintptr>(t)), 0})) {
                delete m_wrap;
                m_wrap = nullptr;
                return nullptr;
            }
            m_wrapped = t;
        }
        return m_wrap;
    }

private:
    QRhi *m_rhi;
    spoutDX m_spout;
    QRhiTexture *m_wrap = nullptr;
    ID3D11Texture2D *m_wrapped = nullptr;
};

} // namespace

namespace TextureShare {

QString technology()
{
    return QStringLiteral("Spout");
}

bool supportsBackend(QRhi *rhi)
{
    return rhi && rhi->backend() == QRhi::D3D11;
}

QStringList sources()
{
    spoutSenderNames names;
    std::set<std::string> senders;
    QStringList out;
    if (names.GetSenderNames(&senders))
        for (const std::string &s : senders)
            out << QString::fromLocal8Bit(s.c_str());
    return out;
}

} // namespace TextureShare

TextureSender *TextureSender::create(QRhi *rhi, const QString &name)
{
    if (!TextureShare::supportsBackend(rhi))
        return nullptr;
    auto *s = new SpoutSender(rhi, name);
    if (!s->ok()) {
        delete s;
        return nullptr;
    }
    return s;
}

TextureReceiver *TextureReceiver::create(QRhi *rhi, const QString &source)
{
    return TextureShare::supportsBackend(rhi) ? new SpoutReceiver(rhi, source) : nullptr;
}
