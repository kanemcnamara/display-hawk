// Platforms without Spout or Syphon.
#include "media/TextureShare.h"

namespace TextureShare {
QString technology() { return {}; }
bool supportsBackend(QRhi *) { return false; }
QStringList sources() { return {}; }
} // namespace TextureShare

TextureSender *TextureSender::create(QRhi *, const QString &) { return nullptr; }
TextureReceiver *TextureReceiver::create(QRhi *, const QString &) { return nullptr; }
