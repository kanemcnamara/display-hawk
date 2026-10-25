#pragma once

#include "render/Effects.h"
#include "render/Generators.h"

#include <QStringList>
#include <QVector4D>

// ISF (Interactive Shader Format, isf.video) support: .fs files with a JSON header describing their
// inputs. Filters (with an "inputImage") become effects, the rest generators. Each is translated
// to the Vulkan-style GLSL our shader pipeline uses and compiled at runtime (IsfRenderer).
// Ids are "isf:" + the file path. Single-pass shaders only (no PASSES / persistent buffers).
namespace Isf {

struct Input {
    enum Type { Float, Bool, Long, Color, Point2D, Event, Image };
    QString name;
    QString label;
    Type type = Float;
    QVector4D def, min, max;
    QStringList labels; // Long: the choices
    QVector<int> values;
    int firstParam = -1; // index of its first value in the effect/generator parameter list
    int binding = -1;    // Image: sampler binding
};

struct Info {
    QString id;
    QString path;
    QString name;
    QString description;
    QStringList categories;
    bool filter = false; // has inputImage: an effect
    QVector<Input> inputs;
    QString vertexSource, fragmentSource; // translated, ready to bake
    QString error;                        // why it cannot be used, if it cannot
};

QString defaultFolder(); // Documents/Display Hawk/ISF (created if missing)

// Parses and registers every .fs in the folders (and the bundled examples). Safe to call again:
// files already known are kept as they were. Call from the GUI thread.
void scan(const QStringList &folders);
QStringList filterIds();
QStringList generatorIds();
const Info *info(const QString &id); // stable pointers; thread-safe lookup

// Parses one file (or ISF source text with a pseudo path).
Info parse(const QString &path, const QByteArray &source);

// Catalogue entries for the effect / generator UIs and renderers.
const EffectDef *effectDef(const QString &id);
const GeneratorDef *generatorDef(const QString &id);

} // namespace Isf
