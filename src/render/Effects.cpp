#include "render/Effects.h"

namespace {

EffectParamDef num(const char *name, double min, double max, double def, const char *suffix = "")
{
    return {QString::fromUtf8(name), min, max, def, QString::fromUtf8(suffix), {}};
}

EffectParamDef choice(const char *name, const QStringList &choices, int def = 0)
{
    return {QString::fromUtf8(name), 0.0, double(choices.size() - 1), double(def), QString(), choices};
}

const QStringList kPanelOrders{QStringLiteral("Left to right"), QStringLiteral("Top to bottom"),
                               QStringLiteral("Panel number"), QStringLiteral("Random")};

QVector<EffectDef> buildCatalogue()
{
    const QString colour = QStringLiteral("Colour"), shape = QStringLiteral("Shape"),
                  texture = QStringLiteral("Texture"), time = QStringLiteral("Time"),
                  panels = QStringLiteral("LED panels"), key = QStringLiteral("Key");
    return {
        {"color", "Colour", colour, FxMode::Color, 1, false,
         {num("Brightness", -1, 1, 0), num("Contrast", 0, 3, 1, "x"), num("Saturation", 0, 3, 1, "x"),
          num("Hue", -180, 180, 0, "°")}},
        {"tint", "Tint", colour, FxMode::Tint, 1, false,
         {num("Red", 0, 1, 1), num("Green", 0, 1, 0.6), num("Blue", 0, 1, 0.2), num("Amount", 0, 1, 1)}},
        {"invert", "Invert", colour, FxMode::Invert, 1, false, {num("Amount", 0, 1, 1)}},
        {"posterize", "Posterize", colour, FxMode::Posterize, 1, false, {num("Levels", 2, 32, 5)}},
        {"transform", "Transform", shape, FxMode::Transform, 1, false,
         {num("Position X", -1, 1, 0), num("Position Y", -1, 1, 0), num("Scale", 0.1, 4, 1, "x"),
          num("Rotation", -180, 180, 0, "°"), choice("Outside", {QStringLiteral("Transparent"), QStringLiteral("Tile")})}},
        {"mirror", "Mirror", shape, FxMode::Mirror, 1, false,
         {choice("Mode", {QStringLiteral("Left onto right"), QStringLiteral("Right onto left"),
                          QStringLiteral("Top onto bottom"), QStringLiteral("Bottom onto top"), QStringLiteral("Four ways")})}},
        {"kaleidoscope", "Kaleidoscope", shape, FxMode::Kaleidoscope, 1, false,
         {num("Segments", 2, 16, 6), num("Rotation", -180, 180, 0, "°"), num("Centre X", 0, 1, 0.5),
          num("Centre Y", 0, 1, 0.5)}},
        {"pixelate", "Pixelate", shape, FxMode::Pixelate, 1, false, {num("Block size", 1, 128, 16, " px")}},
        {"blur", "Blur", texture, FxMode::Blur, 2, false, {num("Radius", 0, 64, 8, " px")}},
        {"rgbsplit", "RGB Split", texture, FxMode::RgbSplit, 1, false,
         {num("Distance", 0, 50, 8, " px"), num("Angle", -180, 180, 0, "°")}},
        {"edges", "Edges", texture, FxMode::Edges, 1, false, {num("Strength", 0, 4, 1.5, "x"), num("Edges only", 0, 1, 1)}},
        {"strobe", "Strobe", time, FxMode::Strobe, 1, false,
         {num("Rate", 0, 30, 8, " Hz"), num("On time", 0, 1, 0.5), choice("Flash to", {QStringLiteral("Black"), QStringLiteral("White")})}},
        {"colorKey", "Colour Key", key, FxMode::ColorKey, 1, false,
         {num("Red", 0, 1, 1), num("Green", 0, 1, 1), num("Blue", 0, 1, 1), num("Tolerance", 0, 1, 0.15),
          num("Softness", 0, 1, 0.1), num("Spill removal", 0, 1, 0.5)}},
        {"lumaKey", "Luma Key", key, FxMode::LumaKey, 1, false,
         {num("Threshold", 0, 1, 0.1), num("Softness", 0, 1, 0.05), choice("Remove", {QStringLiteral("Dark"), QStringLiteral("Bright")})}},
        {"panelOutline", "Panel Outline", panels, FxMode::PanelOutline, 1, true,
         {num("Thickness", 1, 16, 2, " px"), num("Red", 0, 1, 1), num("Green", 0, 1, 1), num("Blue", 0, 1, 1),
          num("Opacity", 0, 1, 1)}},
        {"panelChase", "Panel Chase", panels, FxMode::PanelChase, 1, true,
         {choice("Order", kPanelOrders), num("Speed", 0, 5, 0.5, " /s"), num("Width", 0.01, 1, 0.15),
          num("Softness", 0, 1, 0.5), choice("Mode", {QStringLiteral("Reveal content"), QStringLiteral("Add white")})}},
        {"panelTwinkle", "Panel Twinkle", panels, FxMode::PanelTwinkle, 1, true,
         {num("Rate", 0.1, 20, 3, " Hz"), num("Density", 0, 1, 0.25), num("Fade", 0.05, 1, 0.4),
          choice("Mode", {QStringLiteral("Reveal content"), QStringLiteral("Add white")})}},
        {"panelColours", "Panel Colours", panels, FxMode::PanelColours, 1, true,
         {choice("Order", kPanelOrders), num("Spread", 0, 1, 1), num("Drift", -2, 2, 0.1, " /s"), num("Amount", 0, 1, 1)}},
    };
}

} // namespace

const QVector<EffectDef> &effectDefs()
{
    static const QVector<EffectDef> defs = buildCatalogue();
    return defs;
}

const EffectDef *effectDef(const QString &id)
{
    for (const EffectDef &d : effectDefs())
        if (d.id == id)
            return &d;
    return nullptr;
}

EffectInstance makeEffect(const QString &id)
{
    EffectInstance e;
    e.type = id;
    if (const EffectDef *d = effectDef(id))
        for (const EffectParamDef &p : d->params)
            e.params.append(p.def);
    return e;
}

double effectParam(const EffectInstance &e, int index)
{
    if (index < e.params.size())
        return e.params[index];
    const EffectDef *d = effectDef(e.type);
    return d && index < d->params.size() ? d->params[index].def : 0.0;
}
