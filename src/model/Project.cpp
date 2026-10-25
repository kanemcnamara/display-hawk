#include "model/Project.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QUuid>

QString testPatternName(TestPattern p)
{
    switch (p) {
    case TestPattern::Grid: return QStringLiteral("Grid");
    case TestPattern::ColorBars: return QStringLiteral("Colour Bars");
    case TestPattern::Gradient: return QStringLiteral("Gradient Ramps");
    case TestPattern::Checker: return QStringLiteral("Checkerboard");
    case TestPattern::PanelMap: return QStringLiteral("LED Panel Map");
    case TestPattern::Solid: return QStringLiteral("Solid Colour");
    }
    return {};
}

QString scaleModeName(ScaleMode m)
{
    switch (m) {
    case ScaleMode::Fit: return QStringLiteral("Fit (letterbox)");
    case ScaleMode::Fill: return QStringLiteral("Fill (crop)");
    case ScaleMode::Stretch: return QStringLiteral("Stretch");
    case ScaleMode::Native: return QStringLiteral("Native (1:1 pixels)");
    }
    return {};
}

QString blendModeName(BlendMode m)
{
    switch (m) {
    case BlendMode::Normal: return QStringLiteral("Normal");
    case BlendMode::Add: return QStringLiteral("Add");
    case BlendMode::Screen: return QStringLiteral("Screen");
    case BlendMode::Multiply: return QStringLiteral("Multiply");
    case BlendMode::Lighten: return QStringLiteral("Lighten");
    case BlendMode::Darken: return QStringLiteral("Darken");
    }
    return {};
}

QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString Clip::displayName() const
{
    if (!name.isEmpty())
        return name;
    if (source == ContentSource::Media)
        return QFileInfo(mediaPath).completeBaseName();
    return testPatternName(pattern);
}

Clip Clip::fromPattern(TestPattern p)
{
    Clip c;
    c.id = newId();
    c.source = ContentSource::TestPattern;
    c.pattern = p;
    return c;
}

Clip Clip::fromMedia(const QString &path)
{
    Clip c;
    c.id = newId();
    c.source = ContentSource::Media;
    c.mediaPath = path;
    return c;
}

const Clip *Layer::activeClip() const
{
    if (activeColumn < 0 || activeColumn >= clips.size() || clips[activeColumn].isEmpty())
        return nullptr;
    return &clips[activeColumn];
}

QRect Project::targetRect(int wall) const
{
    if (wall >= 0 && wall < walls.size())
        return walls[wall].rect();
    return QRect(0, 0, compWidth, compHeight);
}

void Project::normalizeDeck()
{
    columns = qMax(1, columns);
    for (Layer &l : layers) {
        if (l.id.isEmpty())
            l.id = newId();
        for (const Clip &c : std::as_const(l.clips))
            if (!c.isEmpty())
                columns = qMax(columns, int(l.clips.size()));
    }
    for (Layer &l : layers) {
        l.clips.resize(columns);
        if (l.activeColumn >= columns)
            l.activeColumn = -1;
        if (l.targetWall >= walls.size())
            l.targetWall = -1;
    }
}

static QJsonObject rectToJson(const QRect &r)
{
    return QJsonObject{{"x", r.x()}, {"y", r.y()}, {"w", r.width()}, {"h", r.height()}};
}

static QRect rectFromJson(const QJsonObject &o)
{
    return QRect(o["x"].toInt(), o["y"].toInt(), o["w"].toInt(), o["h"].toInt());
}

static QJsonObject clipToJson(const Clip &c)
{
    if (c.isEmpty())
        return {};
    return QJsonObject{{"id", c.id},
                       {"name", c.name},
                       {"source", int(c.source)},
                       {"pattern", int(c.pattern)},
                       {"mediaPath", c.mediaPath},
                       {"scale", int(c.scale)},
                       {"loop", c.loop},
                       {"solidColor", c.solidColor.name()},
                       {"gridSpacing", c.gridSpacing}};
}

static Clip clipFromJson(const QJsonObject &o)
{
    Clip c;
    c.id = o["id"].toString();
    if (c.isEmpty())
        return c;
    c.name = o["name"].toString();
    c.source = ContentSource(o["source"].toInt(0));
    c.pattern = TestPattern(o["pattern"].toInt(0));
    c.mediaPath = o["mediaPath"].toString();
    c.scale = ScaleMode(o["scale"].toInt(0));
    c.loop = o["loop"].toBool(true);
    c.solidColor = QColor(o["solidColor"].toString("#ffffff"));
    c.gridSpacing = qMax(4, o["gridSpacing"].toInt(64));
    return c;
}

QJsonObject Project::toJson() const
{
    QJsonObject root;
    root["format"] = "display-hawk";
    root["version"] = 2;

    root["composition"] = QJsonObject{{"width", compWidth},
                                      {"height", compHeight},
                                      {"background", background.name()},
                                      {"panelOverlay", panelOverlay},
                                      {"overlayOpacity", overlayOpacity},
                                      {"master", master},
                                      {"blackout", blackout}};

    QJsonArray wallArr;
    for (const LedWall &w : walls) {
        wallArr.append(QJsonObject{{"name", w.name},
                                   {"panelPxW", w.panelPxW},
                                   {"panelPxH", w.panelPxH},
                                   {"cols", w.cols},
                                   {"rows", w.rows},
                                   {"panelMmW", w.panelMmW},
                                   {"panelMmH", w.panelMmH},
                                   {"x", w.x},
                                   {"y", w.y},
                                   {"color", w.color.name()}});
    }
    root["walls"] = wallArr;

    QJsonArray screenArr;
    for (const Screen &s : screens) {
        QJsonArray sliceArr;
        for (const Slice &sl : s.slices) {
            sliceArr.append(QJsonObject{{"name", sl.name},
                                        {"enabled", sl.enabled},
                                        {"input", rectToJson(sl.input)},
                                        {"output", rectToJson(sl.output)},
                                        {"rotation", sl.rotation},
                                        {"flipH", sl.flipH},
                                        {"flipV", sl.flipV},
                                        {"brightness", sl.brightness}});
        }
        screenArr.append(QJsonObject{{"name", s.name},
                                     {"width", s.width},
                                     {"height", s.height},
                                     {"display", s.display},
                                     {"enabled", s.enabled},
                                     {"slices", sliceArr}});
    }
    root["screens"] = screenArr;

    QJsonArray layerArr;
    for (const Layer &l : layers) {
        QJsonArray clipArr;
        for (const Clip &c : l.clips)
            clipArr.append(clipToJson(c));
        layerArr.append(QJsonObject{{"id", l.id},
                                    {"name", l.name},
                                    {"opacity", l.opacity},
                                    {"blend", int(l.blend)},
                                    {"bypass", l.bypass},
                                    {"solo", l.solo},
                                    {"targetWall", l.targetWall},
                                    {"transitionMs", l.transitionMs},
                                    {"activeColumn", l.activeColumn},
                                    {"clips", clipArr}});
    }
    root["deck"] = QJsonObject{{"columns", columns}, {"layers", layerArr}};
    return root;
}

Project Project::fromJson(const QJsonObject &root)
{
    Project p;
    const QJsonObject comp = root["composition"].toObject();
    p.compWidth = qMax(1, comp["width"].toInt(1920));
    p.compHeight = qMax(1, comp["height"].toInt(1080));
    p.background = QColor(comp["background"].toString("#000000"));
    p.panelOverlay = comp["panelOverlay"].toBool(false);
    p.overlayOpacity = comp["overlayOpacity"].toDouble(0.6);
    p.master = comp["master"].toDouble(1.0);
    p.blackout = comp["blackout"].toBool(false);

    for (const QJsonValue &v : root["walls"].toArray()) {
        const QJsonObject o = v.toObject();
        LedWall w;
        w.name = o["name"].toString(w.name);
        w.panelPxW = qMax(1, o["panelPxW"].toInt(w.panelPxW));
        w.panelPxH = qMax(1, o["panelPxH"].toInt(w.panelPxH));
        w.cols = qMax(1, o["cols"].toInt(w.cols));
        w.rows = qMax(1, o["rows"].toInt(w.rows));
        w.panelMmW = o["panelMmW"].toDouble(w.panelMmW);
        w.panelMmH = o["panelMmH"].toDouble(w.panelMmH);
        w.x = o["x"].toInt();
        w.y = o["y"].toInt();
        w.color = QColor(o["color"].toString(w.color.name()));
        p.walls.append(w);
    }

    for (const QJsonValue &v : root["screens"].toArray()) {
        const QJsonObject o = v.toObject();
        Screen s;
        s.name = o["name"].toString(s.name);
        s.width = qMax(1, o["width"].toInt(s.width));
        s.height = qMax(1, o["height"].toInt(s.height));
        s.display = o["display"].toString();
        s.enabled = o["enabled"].toBool(true);
        for (const QJsonValue &sv : o["slices"].toArray()) {
            const QJsonObject so = sv.toObject();
            Slice sl;
            sl.name = so["name"].toString(sl.name);
            sl.enabled = so["enabled"].toBool(true);
            sl.input = rectFromJson(so["input"].toObject());
            sl.output = rectFromJson(so["output"].toObject());
            sl.rotation = so["rotation"].toInt(0) & 3;
            sl.flipH = so["flipH"].toBool(false);
            sl.flipV = so["flipV"].toBool(false);
            sl.brightness = so["brightness"].toDouble(1.0);
            s.slices.append(sl);
        }
        p.screens.append(s);
    }

    if (root.contains("deck")) {
        const QJsonObject deck = root["deck"].toObject();
        p.columns = deck["columns"].toInt(8);
        for (const QJsonValue &v : deck["layers"].toArray()) {
            const QJsonObject o = v.toObject();
            Layer l;
            l.id = o["id"].toString();
            l.name = o["name"].toString(l.name);
            l.opacity = o["opacity"].toDouble(1.0);
            l.blend = BlendMode(o["blend"].toInt(0));
            l.bypass = o["bypass"].toBool(false);
            l.solo = o["solo"].toBool(false);
            l.targetWall = o["targetWall"].toInt(-1);
            l.transitionMs = qMax(0, o["transitionMs"].toInt(0));
            l.activeColumn = o["activeColumn"].toInt(-1);
            for (const QJsonValue &cv : o["clips"].toArray())
                l.clips.append(clipFromJson(cv.toObject()));
            p.layers.append(l);
        }
    } else {
        // Version 1 files had a single content source: it becomes the first clip of one layer.
        const QJsonObject c = root["content"].toObject();
        Clip clip;
        clip.id = newId();
        clip.source = ContentSource(c["source"].toInt(0));
        clip.pattern = TestPattern(c["pattern"].toInt(0));
        clip.mediaPath = c["mediaPath"].toString();
        clip.scale = ScaleMode(c["scale"].toInt(0));
        clip.loop = c["loop"].toBool(true);
        clip.solidColor = QColor(c["solidColor"].toString("#ffffff"));
        clip.gridSpacing = qMax(4, c["gridSpacing"].toInt(64));
        Layer l;
        l.name = QStringLiteral("Layer 1");
        l.opacity = c["opacity"].toDouble(1.0);
        l.targetWall = c["targetWall"].toInt(-1);
        l.clips.append(clip);
        l.activeColumn = 0;
        p.layers.append(l);
    }
    if (p.layers.isEmpty())
        p.layers.append(Layer());
    p.normalizeDeck();
    return p;
}

Project Project::makeDefault()
{
    Project p;
    LedWall w;
    w.name = QStringLiteral("Main Wall");
    w.cols = 10;
    w.rows = 5;
    p.walls.append(w);

    Screen s;
    s.name = QStringLiteral("Output 1");
    Slice sl;
    sl.name = w.name;
    sl.input = w.rect();
    sl.output = QRect(0, 0, w.pixelWidth(), w.pixelHeight());
    s.slices.append(sl);
    p.screens.append(s);

    Layer bottom;
    bottom.name = QStringLiteral("Layer 1");
    bottom.clips = {Clip::fromPattern(TestPattern::PanelMap), Clip::fromPattern(TestPattern::Grid),
                    Clip::fromPattern(TestPattern::ColorBars), Clip::fromPattern(TestPattern::Gradient)};
    bottom.activeColumn = 0;
    Layer top;
    top.name = QStringLiteral("Layer 2");
    p.layers = {bottom, top};
    p.normalizeDeck();
    return p;
}
