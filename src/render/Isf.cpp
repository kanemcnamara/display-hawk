#include "render/Isf.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QReadWriteLock>
#include <QRegularExpression>
#include <QStandardPaths>
#include <map>
#include <memory>

namespace Isf {

namespace {

constexpr int kMaxInputs = 32;   // vec4 slots in the uniform block
constexpr int kMaxImages = 4;    // sampler bindings 1..4

struct Entry {
    Info info;
    EffectDef effect;
    GeneratorDef generator;
};

QReadWriteLock g_lock;
std::map<QString, std::unique_ptr<Entry>> g_entries; // never erased: pointers stay valid
QStringList g_filters, g_generators;

QVector4D vec(const QJsonValue &v, const QVector4D &fallback)
{
    if (v.isDouble())
        return QVector4D(float(v.toDouble()), 0, 0, 0);
    if (v.isBool())
        return QVector4D(v.toBool() ? 1 : 0, 0, 0, 0);
    if (v.isArray()) {
        const QJsonArray a = v.toArray();
        return QVector4D(float(a.at(0).toDouble()), float(a.at(1).toDouble()), float(a.at(2).toDouble()),
                         float(a.size() > 3 ? a.at(3).toDouble() : 1.0));
    }
    return fallback;
}

QString uniformBlock()
{
    return QStringLiteral("layout(std140, binding = 0) uniform IsfBuf {\n"
                          "    mat4 clipCorr;\n"
                          "    vec4 isf_info;  // RENDERSIZE.xy, TIME, TIMEDELTA\n"
                          "    vec4 isf_info2; // FRAMEINDEX, input is Y-up, premultiply the output\n"
                          "    vec4 isf_date;\n"
                          "    vec4 isf_in[%1];\n"
                          "} u;\n").arg(kMaxInputs);
}

void buildCatalogueEntries(Entry &e)
{
    Info &info = e.info;
    QVector<EffectParamDef> params;
    for (Input &in : info.inputs) {
        if (in.type == Input::Image)
            continue;
        in.firstParam = int(params.size());
        const QString label = in.label.isEmpty() ? in.name : in.label;
        switch (in.type) {
        case Input::Float:
            params.append({label, in.min.x(), in.max.x() > in.min.x() ? in.max.x() : in.min.x() + 1.0, in.def.x(), QString(), {}});
            break;
        case Input::Bool:
        case Input::Event:
            params.append({label, 0, 1, in.def.x(), QString(), {QStringLiteral("Off"), QStringLiteral("On")}});
            break;
        case Input::Long: {
            QStringList choices = in.labels;
            if (choices.isEmpty())
                for (int v : std::as_const(in.values))
                    choices << QString::number(v);
            if (choices.isEmpty())
                choices << QStringLiteral("0") << QStringLiteral("1");
            const int idx = qMax(0, in.values.indexOf(int(in.def.x())));
            params.append({label, 0, double(choices.size() - 1), double(in.values.isEmpty() ? in.def.x() : idx), QString(), choices});
            break;
        }
        case Input::Color: // a Red/Green/Blue run shows as one colour button
            params.append({QStringLiteral("Red"), 0, 1, in.def.x(), QString(), {}});
            params.append({QStringLiteral("Green"), 0, 1, in.def.y(), QString(), {}});
            params.append({QStringLiteral("Blue"), 0, 1, in.def.z(), QString(), {}});
            params.append({label + QStringLiteral(" opacity"), 0, 1, in.def.w(), QString(), {}});
            break;
        case Input::Point2D:
            params.append({label + QStringLiteral(" X"), in.min.x(), in.max.x() > in.min.x() ? in.max.x() : 1920.0, in.def.x(), QString(), {}});
            params.append({label + QStringLiteral(" Y"), in.min.y(), in.max.y() > in.min.y() ? in.max.y() : 1080.0, in.def.y(), QString(), {}});
            break;
        case Input::Image:
            break;
        }
    }
    e.effect = EffectDef{info.id, info.name, QStringLiteral("ISF"), -1, 1, false, params};
    e.generator = GeneratorDef{info.id, info.name, -1, params};
}

} // namespace

QString defaultFolder()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                        + QStringLiteral("/Display Hawk/ISF");
    QDir().mkpath(dir);
    return dir;
}

Info parse(const QString &path, const QByteArray &source)
{
    Info info;
    info.path = path;
    info.id = QStringLiteral("isf:") + path;
    info.name = QFileInfo(path).completeBaseName();
    QString src = QString::fromUtf8(source);

    // The JSON header is the first /* ... */ comment.
    const int open = src.indexOf(QLatin1String("/*"));
    const int close = open >= 0 ? src.indexOf(QLatin1String("*/"), open) : -1;
    if (open < 0 || close < 0) {
        info.error = QStringLiteral("no ISF JSON header");
        return info;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(src.mid(open + 2, close - open - 2).toUtf8(), &pe);
    if (!doc.isObject()) {
        info.error = QStringLiteral("JSON header: %1").arg(pe.errorString());
        return info;
    }
    const QJsonObject header = doc.object();
    QString body = src.mid(close + 2);
    info.description = header["DESCRIPTION"].toString();
    for (const QJsonValue &c : header["CATEGORIES"].toArray())
        info.categories << c.toString();
    if (header.contains("PASSES") && header["PASSES"].toArray().size() > 1) {
        info.error = QStringLiteral("multi-pass ISF shaders are not supported yet");
        return info;
    }

    int binding = 1;
    for (const QJsonValue &v : header["INPUTS"].toArray()) {
        const QJsonObject o = v.toObject();
        Input in;
        in.name = o["NAME"].toString();
        in.label = o["LABEL"].toString();
        const QString type = o["TYPE"].toString().toLower();
        if (type == QLatin1String("float")) {
            in.type = Input::Float;
            in.min = vec(o["MIN"], QVector4D(0, 0, 0, 0));
            in.max = vec(o["MAX"], QVector4D(1, 0, 0, 0));
            in.def = vec(o["DEFAULT"], in.min);
        } else if (type == QLatin1String("bool")) {
            in.type = Input::Bool;
            in.def = vec(o["DEFAULT"], QVector4D());
        } else if (type == QLatin1String("event")) {
            in.type = Input::Event;
        } else if (type == QLatin1String("long")) {
            in.type = Input::Long;
            for (const QJsonValue &x : o["VALUES"].toArray())
                in.values << x.toInt();
            for (const QJsonValue &x : o["LABELS"].toArray())
                in.labels << x.toString();
            in.def = vec(o["DEFAULT"], QVector4D(in.values.value(0), 0, 0, 0));
        } else if (type == QLatin1String("color")) {
            in.type = Input::Color;
            in.def = vec(o["DEFAULT"], QVector4D(1, 1, 1, 1));
        } else if (type == QLatin1String("point2d")) {
            in.type = Input::Point2D;
            in.def = vec(o["DEFAULT"], QVector4D(0, 0, 0, 0));
            in.min = vec(o["MIN"], QVector4D(0, 0, 0, 0));
            in.max = vec(o["MAX"], QVector4D(0, 0, 0, 0));
            if (!o["MIN"].isArray())
                in.min = QVector4D(0, 0, 0, 0);
        } else if (type == QLatin1String("image")) {
            in.type = Input::Image;
            if (binding > kMaxImages) {
                info.error = QStringLiteral("more than %1 image inputs").arg(kMaxImages);
                return info;
            }
            in.binding = binding++;
            if (in.name == QLatin1String("inputImage"))
                info.filter = true;
        } else {
            info.error = QStringLiteral("input %1: type %2 is not supported").arg(in.name, type);
            return info;
        }
        if (info.inputs.size() >= kMaxInputs) {
            info.error = QStringLiteral("more than %1 inputs").arg(kMaxInputs);
            return info;
        }
        info.inputs << in;
    }

    // The body: old-style GLSL with ISF's built-ins. Its main() becomes isf_main(), called by ours.
    body.remove(QRegularExpression(QStringLiteral("^\\s*#version[^\\n]*"), QRegularExpression::MultilineOption));
    body.replace(QRegularExpression(QStringLiteral("\\bvoid\\s+main\\s*\\(\\s*(void)?\\s*\\)")), QStringLiteral("void isf_main()"));
    body.replace(QRegularExpression(QStringLiteral("\\bvarying\\b")), QStringLiteral("/*varying*/"));

    QString defines;
    for (int i = 0; i < info.inputs.size(); ++i) {
        const Input &in = info.inputs[i];
        const QString slot = QStringLiteral("u.isf_in[%1]").arg(i);
        switch (in.type) {
        case Input::Float: defines += QStringLiteral("#define %1 %2.x\n").arg(in.name, slot); break;
        case Input::Bool:
        case Input::Event: defines += QStringLiteral("#define %1 (%2.x > 0.5)\n").arg(in.name, slot); break;
        case Input::Long: defines += QStringLiteral("#define %1 int(%2.x)\n").arg(in.name, slot); break;
        case Input::Color: defines += QStringLiteral("#define %1 %2\n").arg(in.name, slot); break;
        case Input::Point2D: defines += QStringLiteral("#define %1 %2.xy\n").arg(in.name, slot); break;
        case Input::Image:
            defines += QStringLiteral("layout(binding = %1) uniform sampler2D %2;\n").arg(in.binding).arg(in.name);
            break;
        }
    }

    info.fragmentSource = QStringLiteral("#version 440\n"
                                         "layout(location = 0) in vec2 vUV;\n"
                                         "layout(location = 0) out vec4 isf_out;\n")
                          + uniformBlock() + defines
                          + QStringLiteral(
                              "#define RENDERSIZE u.isf_info.xy\n"
                              "#define TIME u.isf_info.z\n"
                              "#define TIMEDELTA u.isf_info.w\n"
                              "#define FRAMEINDEX int(u.isf_info2.x)\n"
                              "#define DATE u.isf_date\n"
                              "#define PASSINDEX 0\n"
                              "vec2 isf_FragNormCoord;\n"
                              "vec4 isf_FragCoord;\n"
                              "vec4 isf_color;\n"
                              "#define vv_FragNormCoord isf_FragNormCoord\n"
                              "#define gl_FragColor isf_color\n"
                              "#define gl_FragCoord isf_FragCoord\n"
                              "#define texture2D texture\n"
                              // ISF coordinates start bottom-left; ours top-left. Inputs arrive premultiplied.
                              "vec4 isf_sample(sampler2D s, vec2 nc) {\n"
                              "    vec2 t = vec2(nc.x, 1.0 - nc.y);\n"
                              "    if (u.isf_info2.y > 0.5) t.y = 1.0 - t.y;\n"
                              "    vec4 c = texture(s, t);\n"
                              "    return c.a > 0.0001 ? vec4(c.rgb / c.a, c.a) : vec4(0.0);\n"
                              "}\n"
                              "#define IMG_NORM_PIXEL(i, c) isf_sample(i, c)\n"
                              "#define IMG_PIXEL(i, c) isf_sample(i, (c) / vec2(textureSize(i, 0)))\n"
                              "#define IMG_THIS_NORM_PIXEL(i) isf_sample(i, isf_FragNormCoord)\n"
                              "#define IMG_THIS_PIXEL(i) isf_sample(i, isf_FragNormCoord)\n"
                              "#define IMG_SIZE(i) vec2(textureSize(i, 0))\n"
                              "#line 1\n")
                          + body
                          + QStringLiteral(
                              "\nvoid main() {\n"
                              "    isf_FragNormCoord = vec2(vUV.x, 1.0 - vUV.y);\n"
                              "    isf_FragCoord = vec4(isf_FragNormCoord * RENDERSIZE, 0.0, 1.0);\n"
                              "    isf_color = vec4(0.0);\n"
                              "    isf_main();\n"
                              "    isf_out = u.isf_info2.z > 0.5 ? vec4(isf_color.rgb * isf_color.a, isf_color.a) : isf_color;\n"
                              "}\n");
    info.vertexSource = QStringLiteral("#version 440\n"
                                       "layout(location = 0) out vec2 vUV;\n")
                        + uniformBlock()
                        + QStringLiteral("void main() {\n"
                                         "    vec2 c = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));\n"
                                         "    gl_Position = u.clipCorr * vec4(c.x * 2.0 - 1.0, 1.0 - c.y * 2.0, 0.0, 1.0);\n"
                                         "    vUV = c;\n"
                                         "}\n");
    return info;
}

void scan(const QStringList &folders)
{
    QStringList paths;
    QDirIterator bundled(QStringLiteral(":/isf"), {QStringLiteral("*.fs")}, QDir::Files);
    while (bundled.hasNext())
        paths << bundled.next();
    for (const QString &folder : folders) {
        QDirIterator it(folder, {QStringLiteral("*.fs")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext())
            paths << it.next();
    }
    for (const QString &path : std::as_const(paths)) {
        const QString id = QStringLiteral("isf:") + path;
        {
            QReadLocker lock(&g_lock);
            if (g_entries.count(id))
                continue;
        }
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        auto e = std::make_unique<Entry>();
        e->info = parse(path, f.readAll());
        if (!e->info.error.isEmpty()) {
            qWarning("ISF %s: %s", qPrintable(path), qPrintable(e->info.error));
            continue;
        }
        buildCatalogueEntries(*e);
        QWriteLocker lock(&g_lock);
        (e->info.filter ? g_filters : g_generators) << id;
        g_entries[id] = std::move(e);
    }
}

QStringList filterIds()
{
    QReadLocker lock(&g_lock);
    return g_filters;
}

QStringList generatorIds()
{
    QReadLocker lock(&g_lock);
    return g_generators;
}

const Info *info(const QString &id)
{
    QReadLocker lock(&g_lock);
    const auto it = g_entries.find(id);
    return it == g_entries.end() ? nullptr : &it->second->info;
}

const EffectDef *effectDef(const QString &id)
{
    QReadLocker lock(&g_lock);
    const auto it = g_entries.find(id);
    return it == g_entries.end() || !it->second->info.filter ? nullptr : &it->second->effect;
}

const GeneratorDef *generatorDef(const QString &id)
{
    QReadLocker lock(&g_lock);
    const auto it = g_entries.find(id);
    return it == g_entries.end() || it->second->info.filter ? nullptr : &it->second->generator;
}

} // namespace Isf
