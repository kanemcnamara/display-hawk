#pragma once

#include "model/Project.h"

#include <QStringList>

// The user's library on disk, under Documents/Display Hawk: effect chain presets (Presets/*.dhfx)
// and setup templates (Templates/*.dhawk).
namespace Library {

QString presetFolder();
QString templateFolder();

QStringList presetNames();
bool savePreset(const QString &name, const QVector<EffectInstance> &chain, QString *error = nullptr);
QVector<EffectInstance> loadPreset(const QString &name, bool *ok = nullptr);

QStringList templateNames();
// The setup without what is playing (and without the file's own name): a starting point.
bool saveTemplate(const QString &name, const Project &project, QString *error = nullptr);
QString templatePath(const QString &name);

} // namespace Library
