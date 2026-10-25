#pragma once

#include "render/Effects.h"

// The generator catalogue: procedural content drawn on the GPU by shaders/gen.frag, in the clip's
// two colours (Clip::solidColor in front, Clip::color2 behind) with up to 8 parameters.

struct GeneratorDef {
    QString id;
    QString name;
    int shaderMode = 0;
    QVector<EffectParamDef> params;
};

const QVector<GeneratorDef> &generatorDefs();
const GeneratorDef *generatorDef(const QString &id);
QVector<double> generatorDefaults(const QString &id);
double generatorParam(const Clip &clip, int index); // missing values at their defaults
