#include "model/Library.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

namespace Library {

namespace {

QString folder(const QString &name)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                        + QStringLiteral("/Display Hawk/") + name;
    QDir().mkpath(dir);
    return dir;
}

// Names become file names: keep them safe on every file system.
QString fileName(const QString &name)
{
    QString n = name.trimmed();
    for (const QChar c : QStringLiteral("\\/:*?\"<>|"))
        n.replace(c, QLatin1Char('_'));
    return n;
}

QStringList names(const QString &dir, const QString &suffix)
{
    QStringList out;
    for (const QFileInfo &f : QDir(dir).entryInfoList({QStringLiteral("*.") + suffix}, QDir::Files, QDir::Name))
        out << f.completeBaseName();
    return out;
}

bool write(const QString &path, const QJsonObject &obj, QString *error)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    f.write(QJsonDocument(obj).toJson());
    if (!f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

} // namespace

QString presetFolder()
{
    return folder(QStringLiteral("Presets"));
}

QString templateFolder()
{
    return folder(QStringLiteral("Templates"));
}

QStringList presetNames()
{
    return names(presetFolder(), QStringLiteral("dhfx"));
}

bool savePreset(const QString &name, const QVector<EffectInstance> &chain, QString *error)
{
    const QJsonObject obj{{"format", "display-hawk-effects"}, {"version", 1}, {"effects", effectChainToJson(chain)}};
    return write(presetFolder() + QLatin1Char('/') + fileName(name) + QStringLiteral(".dhfx"), obj, error);
}

QVector<EffectInstance> loadPreset(const QString &name, bool *ok)
{
    QFile f(presetFolder() + QLatin1Char('/') + fileName(name) + QStringLiteral(".dhfx"));
    const bool opened = f.open(QIODevice::ReadOnly);
    const QJsonObject obj = opened ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
    if (ok)
        *ok = opened && obj["format"].toString() == QLatin1String("display-hawk-effects");
    return effectChainFromJson(obj["effects"].toArray());
}

QStringList templateNames()
{
    return names(templateFolder(), QStringLiteral("dhawk"));
}

QString templatePath(const QString &name)
{
    return templateFolder() + QLatin1Char('/') + fileName(name) + QStringLiteral(".dhawk");
}

bool saveTemplate(const QString &name, const Project &project, QString *error)
{
    Project t = project;
    // A starting point: nothing playing, faders up, not blacked out.
    for (Layer &l : t.layers)
        l.activeColumn = -1;
    t.blackout = false;
    t.master = 1.0;
    return write(templatePath(name), t.toJson(), error);
}

} // namespace Library
