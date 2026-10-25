#include "render/Generators.h"

#include "render/Isf.h"

namespace {

EffectParamDef num(const char *name, double min, double max, double def, const char *suffix = "")
{
    return {QString::fromUtf8(name), min, max, def, QString::fromUtf8(suffix), {}};
}

EffectParamDef choice(const char *name, const QStringList &choices, int def = 0)
{
    return {QString::fromUtf8(name), 0.0, double(choices.size() - 1), double(def), QString(), choices};
}

QVector<GeneratorDef> buildCatalogue()
{
    return {
        {"shapes", "Shapes", 0,
         {choice("Shape", {QStringLiteral("Circle"), QStringLiteral("Square"), QStringLiteral("Triangle"),
                           QStringLiteral("Star"), QStringLiteral("Ring"), QStringLiteral("Hexagon")}),
          num("Size", 0.01, 1, 0.5), num("Rotation", -180, 180, 0, "°"), num("Spin", -4, 4, 0, " rev/s"),
          num("Repeat", 1, 32, 1), num("Softness", 0, 1, 0.02), num("Pulse", 0, 1, 0), num("Pulse rate", 0, 10, 1, " Hz")}},
        {"lines", "Lines", 1,
         {num("Angle", -180, 180, 0, "°"), num("Count", 1, 128, 8), num("Width", 0.01, 1, 0.5),
          num("Speed", -10, 10, 0.5, " /s"), num("Softness", 0, 1, 0.05)}},
        {"gradient", "Gradient", 2,
         {choice("Type", {QStringLiteral("Linear"), QStringLiteral("Radial"), QStringLiteral("Angular"),
                          QStringLiteral("Diamond")}),
          num("Angle", -180, 180, 0, "°"), num("Speed", -5, 5, 0.2, " /s"), num("Repeat", 1, 16, 1),
          choice("Mirror", {QStringLiteral("Off"), QStringLiteral("On")}, 1)}},
        {"plasma", "Plasma", 3, {num("Scale", 0.2, 20, 3), num("Speed", 0, 5, 0.5), num("Detail", 1, 4, 2)}},
        {"noise", "Noise", 4,
         {num("Scale", 0.5, 50, 6), num("Speed", 0, 5, 0.3), num("Octaves", 1, 6, 4), num("Contrast", 0.2, 4, 1.2)}},
        {"particles", "Particles", 5,
         {num("Density", 2, 64, 16), num("Size", 0.01, 0.5, 0.12), num("Speed", 0, 5, 0.6),
          choice("Direction", {QStringLiteral("Up"), QStringLiteral("Down"), QStringLiteral("Left"),
                               QStringLiteral("Right"), QStringLiteral("Out from the centre")}),
          num("Twinkle", 0, 1, 0.5), num("Softness", 0, 1, 0.6)}},
        {"starfield", "Starfield", 6, {num("Speed", 0, 5, 0.6), num("Density", 1, 8, 4), num("Size", 0.2, 4, 1)}},
        {"tunnel", "Tunnel", 7,
         {num("Speed", -5, 5, 0.6), num("Rings", 1, 40, 8), num("Twist", -4, 4, 1),
          choice("Shape", {QStringLiteral("Round"), QStringLiteral("Square")}), num("Softness", 0, 1, 0.3)}},
        {"radar", "Radar sweep", 8, {num("Speed", -4, 4, 0.5, " rev/s"), num("Trail", 0.01, 1, 0.3), num("Rings", 0, 20, 4)}},
        {"checker", "Moving checker", 9, {num("Count", 2, 64, 8), num("Speed", -5, 5, 0.5, " /s"), num("Angle", -180, 180, 0, "°")}},
    };
}

} // namespace

const QVector<GeneratorDef> &generatorDefs()
{
    static const QVector<GeneratorDef> defs = buildCatalogue();
    return defs;
}

const GeneratorDef *generatorDef(const QString &id)
{
    if (id.startsWith(QLatin1String("isf:")))
        return Isf::generatorDef(id);
    for (const GeneratorDef &d : generatorDefs())
        if (d.id == id)
            return &d;
    return nullptr;
}

QVector<double> generatorDefaults(const QString &id)
{
    QVector<double> v;
    if (const GeneratorDef *d = generatorDef(id))
        for (const EffectParamDef &p : d->params)
            v << p.def;
    return v;
}

double generatorParam(const Clip &clip, int index)
{
    if (index < clip.genParams.size())
        return clip.genParams[index];
    const GeneratorDef *d = generatorDef(clip.generator);
    return d && index < d->params.size() ? d->params[index].def : 0.0;
}
