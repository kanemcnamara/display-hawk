// Syphon (macOS): shares Metal textures (IOSurfaces) through the vendored Syphon framework sources.
// Compiled with ARC.
#include "media/TextureShare.h"

#include <rhi/qrhi_platform.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <Syphon/SyphonMetalClient.h>
#import <Syphon/SyphonMetalServer.h>
#import <Syphon/SyphonServerDirectory.h>

namespace {

id<MTLDevice> deviceOf(QRhi *rhi)
{
    const auto *h = static_cast<const QRhiMetalNativeHandles *>(rhi->nativeHandles());
    return h ? (__bridge id<MTLDevice>)h->dev : nil;
}

QString keyFor(NSDictionary *description)
{
    NSString *app = description[SyphonServerDescriptionAppNameKey] ?: @"";
    NSString *name = description[SyphonServerDescriptionNameKey] ?: @"";
    return name.length ? QStringLiteral("%1 - %2").arg(QString::fromNSString(app), QString::fromNSString(name))
                       : QString::fromNSString(app);
}

class SyphonSender final : public TextureSender
{
public:
    SyphonSender(QRhi *rhi, const QString &name)
    {
        m_server = [[SyphonMetalServer alloc] initWithName:name.toNSString() device:deviceOf(rhi) options:nil];
    }
    ~SyphonSender() override
    {
        [m_server stop];
        m_server = nil;
    }
    bool ok() const { return m_server != nil; }
    // BGRA matches Syphon's surfaces, so publishing is a plain GPU blit (no shader library needed).
    QRhiTexture::Format format() const override { return QRhiTexture::BGRA8; }
    void publish(QRhiCommandBuffer *cb, QRhiTexture *texture) override
    {
        cb->beginExternal(); // ends QRhi's encoder; the blit goes into the frame's own command buffer
        const auto *h = static_cast<const QRhiMetalCommandBufferNativeHandles *>(cb->nativeHandles());
        id<MTLTexture> t = (__bridge id<MTLTexture>)reinterpret_cast<void *>(texture->nativeTexture().object);
        if (h && h->commandBuffer && t) {
            [m_server publishFrameTexture:t
                          onCommandBuffer:(__bridge id<MTLCommandBuffer>)h->commandBuffer
                              imageRegion:NSMakeRect(0, 0, t.width, t.height)
                                  flipped:NO];
        }
        cb->endExternal();
    }

private:
    SyphonMetalServer *m_server = nil;
};

class SyphonReceiver final : public TextureReceiver
{
public:
    SyphonReceiver(QRhi *rhi, const QString &source)
        : m_rhi(rhi)
        , m_source(source)
        , m_device(deviceOf(rhi))
    {
    }
    ~SyphonReceiver() override
    {
        delete m_wrap;
        [m_client stop];
        m_client = nil;
    }
    QRhiTexture *receive(QRhiCommandBuffer *) override
    {
        if (!m_client || !m_client.isValid) {
            m_client = nil;
            for (NSDictionary *d in [SyphonServerDirectory sharedDirectory].servers) {
                if (keyFor(d) == m_source) {
                    m_client = [[SyphonMetalClient alloc] initWithServerDescription:d device:m_device options:nil
                                                                    newFrameHandler:nil];
                    break;
                }
            }
            if (!m_client)
                return nullptr;
        }
        id<MTLTexture> t = [m_client newFrameImage];
        if (!t)
            return m_wrap;
        if (t != m_current) {
            if (m_wrap)
                m_wrap->deleteLater();
            const QRhiTexture::Format fmt =
                t.pixelFormat == MTLPixelFormatRGBA8Unorm ? QRhiTexture::RGBA8 : QRhiTexture::BGRA8;
            m_wrap = m_rhi->newTexture(fmt, QSize(int(t.width), int(t.height)));
            if (!m_wrap->createFrom({quint64(reinterpret_cast<quintptr>((__bridge void *)t)), 0})) {
                delete m_wrap;
                m_wrap = nullptr;
            }
            // Keep the last few frames alive: frames still in flight may sample them.
            m_keep[m_keepNext] = t;
            m_keepNext = (m_keepNext + 1) % 3;
            m_current = t;
        }
        return m_wrap;
    }

private:
    QRhi *m_rhi;
    QString m_source;
    id<MTLDevice> m_device;
    SyphonMetalClient *m_client = nil;
    id<MTLTexture> m_current = nil;
    id<MTLTexture> m_keep[3] = {nil, nil, nil};
    int m_keepNext = 0;
    QRhiTexture *m_wrap = nullptr;
};

} // namespace

namespace TextureShare {

QString technology()
{
    return QStringLiteral("Syphon");
}

bool supportsBackend(QRhi *rhi)
{
    return rhi && rhi->backend() == QRhi::Metal;
}

QStringList sources()
{
    QStringList out;
    @autoreleasepool {
        for (NSDictionary *d in [SyphonServerDirectory sharedDirectory].servers)
            out << keyFor(d);
    }
    return out;
}

} // namespace TextureShare

TextureSender *TextureSender::create(QRhi *rhi, const QString &name)
{
    if (!TextureShare::supportsBackend(rhi))
        return nullptr;
    auto *s = new SyphonSender(rhi, name);
    if (!s->ok()) {
        delete s;
        return nullptr;
    }
    return s;
}

TextureReceiver *TextureReceiver::create(QRhi *rhi, const QString &source)
{
    return TextureShare::supportsBackend(rhi) ? new SyphonReceiver(rhi, source) : nullptr;
}
