#include "media/Ndi.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QMutex>
#include <QTimer>
#include <cstdint>
#include <cstring>

// Mirrors of the NDI SDK's public C types (Processing.NDI.structs.h), in their natural C layout.
namespace {

struct NdiSource {
    const char *name;
    const char *urlAddress;
};
struct NdiFindCreate {
    bool showLocalSources;
    const char *groups;
    const char *extraIps;
};
struct NdiRecvCreateV3 {
    NdiSource source;
    int colorFormat; // NDIlib_recv_color_format_e
    int bandwidth;   // NDIlib_recv_bandwidth_e
    bool allowVideoFields;
    const char *recvName;
};
struct NdiSendCreate {
    const char *name;
    const char *groups;
    bool clockVideo;
    bool clockAudio;
};
struct NdiVideoFrameV2 {
    int xres;
    int yres;
    int fourCC;
    int frameRateN;
    int frameRateD;
    float pictureAspectRatio;
    int frameFormatType;
    int64_t timecode;
    uint8_t *data;
    int lineStrideInBytes;
    const char *metadata;
    int64_t timestamp;
};

constexpr int fourcc(char a, char b, char c, char d)
{
    return int(uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24);
}
constexpr int kFourCC_RGBA = fourcc('R', 'G', 'B', 'A');
constexpr int kFourCC_RGBX = fourcc('R', 'G', 'B', 'X');
constexpr int kRecvColorRGBX_RGBA = 2;
constexpr int kRecvBandwidthHighest = 100;
constexpr int kFrameTypeVideo = 1;
constexpr int kFrameFormatProgressive = 1;
constexpr int64_t kSendTimecodeSynthesize = INT64_MAX;

struct Api {
    bool (*initialize)();
    const char *(*version)();
    void *(*findCreateV2)(const NdiFindCreate *);
    const NdiSource *(*findGetCurrentSources)(void *, uint32_t *);
    void *(*recvCreateV3)(const NdiRecvCreateV3 *);
    void (*recvDestroy)(void *);
    int (*recvCaptureV2)(void *, NdiVideoFrameV2 *, void *, void *, uint32_t);
    void (*recvFreeVideoV2)(void *, const NdiVideoFrameV2 *);
    void *(*sendCreate)(const NdiSendCreate *);
    void (*sendDestroy)(void *);
    void (*sendVideoAsyncV2)(void *, const NdiVideoFrameV2 *);
    int (*sendGetNoConnections)(void *, uint32_t);
};

QMutex g_mutex;
bool g_tried = false;
Api g_api{};
bool g_ok = false;
QString g_status;
void *g_finder = nullptr;

QStringList candidatePaths()
{
    QStringList dirs;
    for (const char *var : {"NDI_RUNTIME_DIR_V6", "NDI_RUNTIME_DIR_V5", "NDI_RUNTIME_DIR_V4"})
        if (qEnvironmentVariableIsSet(var))
            dirs << qEnvironmentVariable(var);
    dirs << QCoreApplication::applicationDirPath();
    QStringList paths;
#if defined(Q_OS_WIN)
    for (const QString &d : std::as_const(dirs))
        paths << QDir(d).filePath(QStringLiteral("Processing.NDI.Lib.x64.dll"));
    paths << QStringLiteral("Processing.NDI.Lib.x64.dll"); // on PATH
#elif defined(Q_OS_MACOS)
    for (const QString &d : std::as_const(dirs))
        paths << QDir(d).filePath(QStringLiteral("libndi.dylib"));
    paths << QStringLiteral("/usr/local/lib/libndi.dylib") << QStringLiteral("/Library/NDI SDK for Apple/lib/macOS/libndi.dylib")
          << QCoreApplication::applicationDirPath() + QStringLiteral("/../Frameworks/libndi.dylib");
#else
    for (const QString &d : std::as_const(dirs))
        paths << QDir(d).filePath(QStringLiteral("libndi.so.6"));
    paths << QStringLiteral("libndi.so.6") << QStringLiteral("libndi.so.5");
#endif
    return paths;
}

// Loads and initialises the runtime once; afterwards g_ok tells whether NDI can be used.
bool ensureLoaded()
{
    QMutexLocker lock(&g_mutex);
    if (g_tried)
        return g_ok;
    g_tried = true;
    for (const QString &path : candidatePaths()) {
        auto *lib = new QLibrary(path); // kept loaded for the life of the process
        if (!lib->load()) {
            delete lib;
            continue;
        }
        auto resolve = [lib](const char *name) { return lib->resolve(name); };
        g_api.initialize = reinterpret_cast<decltype(g_api.initialize)>(resolve("NDIlib_initialize"));
        g_api.version = reinterpret_cast<decltype(g_api.version)>(resolve("NDIlib_version"));
        g_api.findCreateV2 = reinterpret_cast<decltype(g_api.findCreateV2)>(resolve("NDIlib_find_create_v2"));
        g_api.findGetCurrentSources =
            reinterpret_cast<decltype(g_api.findGetCurrentSources)>(resolve("NDIlib_find_get_current_sources"));
        g_api.recvCreateV3 = reinterpret_cast<decltype(g_api.recvCreateV3)>(resolve("NDIlib_recv_create_v3"));
        g_api.recvDestroy = reinterpret_cast<decltype(g_api.recvDestroy)>(resolve("NDIlib_recv_destroy"));
        g_api.recvCaptureV2 = reinterpret_cast<decltype(g_api.recvCaptureV2)>(resolve("NDIlib_recv_capture_v2"));
        g_api.recvFreeVideoV2 = reinterpret_cast<decltype(g_api.recvFreeVideoV2)>(resolve("NDIlib_recv_free_video_v2"));
        g_api.sendCreate = reinterpret_cast<decltype(g_api.sendCreate)>(resolve("NDIlib_send_create"));
        g_api.sendDestroy = reinterpret_cast<decltype(g_api.sendDestroy)>(resolve("NDIlib_send_destroy"));
        g_api.sendVideoAsyncV2 = reinterpret_cast<decltype(g_api.sendVideoAsyncV2)>(resolve("NDIlib_send_send_video_async_v2"));
        g_api.sendGetNoConnections =
            reinterpret_cast<decltype(g_api.sendGetNoConnections)>(resolve("NDIlib_send_get_no_connections"));
        const bool complete = g_api.initialize && g_api.findCreateV2 && g_api.findGetCurrentSources && g_api.recvCreateV3
                              && g_api.recvDestroy && g_api.recvCaptureV2 && g_api.recvFreeVideoV2 && g_api.sendCreate
                              && g_api.sendDestroy && g_api.sendVideoAsyncV2;
        if (!complete) {
            g_status = QObject::tr("%1 is not a usable NDI runtime (missing functions).").arg(lib->fileName());
            continue;
        }
        if (!g_api.initialize()) {
            g_status = QObject::tr("The NDI runtime could not start (this CPU may not be supported).");
            return false;
        }
        g_ok = true;
        g_status = QObject::tr("NDI runtime %1 loaded from %2")
                       .arg(g_api.version ? QString::fromLatin1(g_api.version()) : QStringLiteral("?"),
                            QDir::toNativeSeparators(lib->fileName()));
        qInfo("%s", qPrintable(g_status));
        // Discovery runs for the life of the app so the source list is ready when asked for.
        NdiFindCreate find{true, nullptr, nullptr};
        g_finder = g_api.findCreateV2(&find);
        return true;
    }
    if (g_status.isEmpty())
        g_status = QObject::tr("NDI runtime not found. Install NDI Tools or the NDI Runtime from ndi.video, then restart.");
    return false;
}

} // namespace

namespace Ndi {

bool available()
{
    return ensureLoaded();
}

QString runtimeStatus()
{
    ensureLoaded();
    QMutexLocker lock(&g_mutex);
    return g_status;
}

QStringList sources()
{
    QStringList names;
    if (!ensureLoaded() || !g_finder)
        return names;
    uint32_t count = 0;
    const NdiSource *list = g_api.findGetCurrentSources(g_finder, &count);
    for (uint32_t i = 0; list && i < count; ++i)
        names << QString::fromUtf8(list[i].name);
    return names;
}

} // namespace Ndi

// ---------------------------------------------------------------- receiver

NdiReceiver::NdiReceiver(const QString &sourceName, QObject *parent)
    : QThread(parent)
    , m_source(sourceName)
{
    setObjectName(QStringLiteral("NDI receiver"));
}

NdiReceiver::~NdiReceiver()
{
    stop();
}

void NdiReceiver::stop()
{
    m_stop = true;
    wait();
}

void NdiReceiver::run()
{
    if (!ensureLoaded()) {
        emit status(Ndi::runtimeStatus());
        return;
    }
    const QByteArray name = m_source.toUtf8();
    NdiRecvCreateV3 create{};
    create.source = {name.constData(), nullptr};
    create.colorFormat = kRecvColorRGBX_RGBA; // RGBA: uploads without conversion
    create.bandwidth = kRecvBandwidthHighest;
    create.allowVideoFields = false;
    create.recvName = "Display Hawk";
    void *recv = g_api.recvCreateV3(&create);
    if (!recv) {
        emit status(tr("Could not connect to NDI source %1").arg(m_source));
        return;
    }
    emit status(tr("Connected to %1").arg(m_source));
    while (!m_stop) {
        NdiVideoFrameV2 video{};
        if (g_api.recvCaptureV2(recv, &video, nullptr, nullptr, 100) != kFrameTypeVideo)
            continue;
        if (video.data && video.xres > 0 && video.yres > 0
            && (video.fourCC == kFourCC_RGBA || video.fourCC == kFourCC_RGBX)) {
            // Deep copy: the runtime reuses its buffer once the frame is freed.
            const QImage view(video.data, video.xres, video.yres, video.lineStrideInBytes,
                              video.fourCC == kFourCC_RGBA ? QImage::Format_RGBA8888 : QImage::Format_RGBX8888);
            emit frame(view.copy());
        }
        g_api.recvFreeVideoV2(recv, &video);
    }
    g_api.recvDestroy(recv);
}

// ---------------------------------------------------------------- sender

NdiSender::NdiSender(const QString &name)
    : m_name(name)
    , m_nameUtf8(name.toUtf8())
{
    if (!ensureLoaded())
        return;
    NdiSendCreate create{m_nameUtf8.constData(), nullptr, false, false}; // pacing comes from our render loop
    m_instance = g_api.sendCreate(&create);
    if (!m_instance)
        qWarning("NDI: could not create sender %s", qPrintable(name));
}

NdiSender::~NdiSender()
{
    if (m_instance) {
        g_api.sendVideoAsyncV2(m_instance, nullptr); // flush: the runtime lets go of our buffers
        g_api.sendDestroy(m_instance);
    }
}

void NdiSender::send(const QImage &rgba, int frameRateN, int frameRateD)
{
    if (!m_instance || rgba.isNull())
        return;
    // The previous buffer may still be read by the runtime; use the other one.
    QImage &buffer = m_inFlight[m_next];
    m_next ^= 1;
    buffer = rgba.format() == QImage::Format_RGBA8888 ? rgba : rgba.convertToFormat(QImage::Format_RGBA8888);
    NdiVideoFrameV2 f{};
    f.xres = buffer.width();
    f.yres = buffer.height();
    f.fourCC = kFourCC_RGBA;
    f.frameRateN = frameRateN;
    f.frameRateD = frameRateD;
    f.pictureAspectRatio = 0.0f; // square pixels
    f.frameFormatType = kFrameFormatProgressive;
    f.timecode = kSendTimecodeSynthesize;
    f.data = const_cast<uint8_t *>(buffer.constBits());
    f.lineStrideInBytes = int(buffer.bytesPerLine());
    g_api.sendVideoAsyncV2(m_instance, &f);
}

int NdiSender::connections() const
{
    return m_instance && g_api.sendGetNoConnections ? g_api.sendGetNoConnections(m_instance, 0) : 0;
}
