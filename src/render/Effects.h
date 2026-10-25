#pragma once

#include "model/Project.h"

#include <QStringList>
#include <QVector>

// The effect catalogue, shared by the UI and the renderer. Each effect is a mode of the effects
// shader (shaders/fx.frag) with up to 8 parameters.

struct EffectParamDef {
    QString name;
    double min = 0.0;
    double max = 1.0;
    double def = 0.0;
    QString suffix;
    QStringList choices; // non-empty: an enumeration (value = choice index)
};

struct EffectDef {
    QString id;
    QString name;
    QString category;
    int shaderMode = 0;
    int passes = 1;
    bool usesPanels = false; // reads the LED panel map (outline, chase...)
    QVector<EffectParamDef> params;
};

const QVector<EffectDef> &effectDefs();
const EffectDef *effectDef(const QString &id);
EffectInstance makeEffect(const QString &id); // with default parameter values

// A parameter's value, with missing entries (older files, newer effects) at their defaults.
double effectParam(const EffectInstance &e, int index);

// Shader modes (must match shaders/fx.frag).
namespace FxMode {
enum {
    Color = 0, Tint, Invert, Posterize, Transform, Mirror, Kaleidoscope, Pixelate, Blur, RgbSplit, Edges, Strobe,
    PanelOutline, PanelChase, PanelTwinkle, PanelColours, ColorKey, LumaKey
};
}
