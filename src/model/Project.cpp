#include "model/Project.h"

#include <QJsonArray>

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

QRect Project::contentTarget() const
{
    if (content.targetWall >= 0 && content.targetWall < walls.size())
        return walls[content.targetWall].rect();
    return QRect(0, 0, compWidth, compHeight);
}

static QJsonObject rectToJson(const QRect &r)
{
    return QJsonObject{{"x", r.x()}, {"y", r.y()}, {"w", r.width()}, {"h", r.height()}};
}

static QRect rectFromJson(const QJsonObject &o)
{
    return QRect(o["x"].toInt(), o["y"].toInt(), o["w"].toInt(), o["h"].toInt());
}

QJsonObject Project::toJson() const
{
    QJsonObject root;
    root["format"] = "display-hawk";
    root["version"] = 1;

    QJsonObject comp{{"width", compWidth},
                     {"height", compHeight},
                     {"background", background.name()},
                     {"panelOverlay", panelOverlay},
                     {"overlayOpacity", overlayOpacity}};
    root["composition"] = comp;

    QJsonObject c{{"source", int(content.source)},
                  {"pattern", int(content.pattern)},
                  {"mediaPath", content.mediaPath},
                  {"scale", int(content.scale)},
                  {"targetWall", content.targetWall},
                  {"loop", content.loop},
                  {"opacity", content.opacity},
                  {"solidColor", content.solidColor.name()},
                  {"gridSpacing", content.gridSpacing}};
    root["content"] = c;

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

    const QJsonObject c = root["content"].toObject();
    p.content.source = ContentSource(c["source"].toInt(0));
    p.content.pattern = TestPattern(c["pattern"].toInt(0));
    p.content.mediaPath = c["mediaPath"].toString();
    p.content.scale = ScaleMode(c["scale"].toInt(0));
    p.content.targetWall = c["targetWall"].toInt(-1);
    p.content.loop = c["loop"].toBool(true);
    p.content.opacity = c["opacity"].toDouble(1.0);
    p.content.solidColor = QColor(c["solidColor"].toString("#ffffff"));
    p.content.gridSpacing = qMax(4, c["gridSpacing"].toInt(64));

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
    if (p.content.targetWall >= p.walls.size())
        p.content.targetWall = -1;
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
    return p;
}
