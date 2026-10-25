#pragma once

#include <QStringList>
#include <rhi/qrhi.h>

// GPU texture sharing with other apps on the same machine: Spout on Windows (Direct3D 11 backend)
// and Syphon on macOS (Metal backend). Frames never leave the GPU.
//
// Senders and receivers live on the render thread and are called inside a QRhi frame, outside any
// render pass; they wrap their native work in beginExternal()/endExternal().
namespace TextureShare {

QString technology();              // "Spout", "Syphon", or empty where unsupported
bool supportsBackend(QRhi *rhi);   // Spout needs Direct3D 11, Syphon needs Metal
QStringList sources();             // what other apps (and Display Hawk) are publishing

} // namespace TextureShare

class TextureSender
{
public:
    virtual ~TextureSender() = default;
    // The texture format publish() expects (what the target should be rendered in).
    virtual QRhiTexture::Format format() const = 0;
    virtual void publish(QRhiCommandBuffer *cb, QRhiTexture *texture) = 0;

    static TextureSender *create(QRhi *rhi, const QString &name); // nullptr where unsupported
};

class TextureReceiver
{
public:
    virtual ~TextureReceiver() = default;
    // The newest frame from the source, wrapped as a QRhiTexture owned by the receiver (valid until
    // the next call), or nullptr while the source is not there.
    virtual QRhiTexture *receive(QRhiCommandBuffer *cb) = 0;

    static TextureReceiver *create(QRhi *rhi, const QString &source); // nullptr where unsupported
};
