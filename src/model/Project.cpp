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
    case TestPattern::MovingBars: return QStringLiteral("Moving Bars (sync check)");
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

QString controlActionName(ControlAction a)
{
    switch (a) {
    case ControlAction::Master: return QStringLiteral("Master fader");
    case ControlAction::Blackout: return QStringLiteral("Blackout");
    case ControlAction::OutputsLive: return QStringLiteral("Outputs live");
    case ControlAction::TriggerColumn: return QStringLiteral("Trigger column");
    case ControlAction::TriggerClip: return QStringLiteral("Trigger clip");
    case ControlAction::LayerOpacity: return QStringLiteral("Layer opacity");
    case ControlAction::LayerBypass: return QStringLiteral("Layer bypass");
    case ControlAction::LayerSolo: return QStringLiteral("Layer solo");
    case ControlAction::ClearLayer: return QStringLiteral("Stop layer");
    }
    return {};
}

bool controlActionUsesLayer(ControlAction a)
{
    return a == ControlAction::TriggerClip || a == ControlAction::LayerOpacity || a == ControlAction::LayerBypass
           || a == ControlAction::LayerSolo || a == ControlAction::ClearLayer;
}

bool controlActionUsesColumn(ControlAction a)
{
    return a == ControlAction::TriggerColumn || a == ControlAction::TriggerClip;
}

QString ControlMapping::targetText() const
{
    QStringList parts;
    if (controlActionUsesLayer(action))
        parts << QStringLiteral("layer %1").arg(layer + 1);
    if (controlActionUsesColumn(action))
        parts << QStringLiteral("column %1").arg(column + 1);
    return parts.join(QStringLiteral(", "));
}

QString ControlMapping::midiText() const
{
    if (!midiSet)
        return QStringLiteral("not learned");
    return QStringLiteral("%1 %2, ch %3").arg(midiIsCC ? QStringLiteral("CC") : QStringLiteral("Note")).arg(midiNumber).arg(midiChannel + 1);
}

QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString Clip::displayName() const
{
    if (!name.isEmpty())
        return name;
    switch (source) {
    case ContentSource::Media: return QFileInfo(mediaPath).completeBaseName();
    case ContentSource::Capture: return QStringLiteral("Capture");
    case ContentSource::Ndi: return inputId;
    default: return testPatternName(pattern);
    }
}

QString Clip::mediaKey() const
{
    switch (source) {
    case ContentSource::Media: return mediaPath;
    case ContentSource::Capture: return inputId.isEmpty() ? QString() : QStringLiteral("capture:") + inputId;
    case ContentSource::Ndi: return inputId.isEmpty() ? QString() : QStringLiteral("ndi:") + inputId;
    default: return QString();
    }
}

Clip Clip::fromCapture(const QString &deviceId, const QString &description)
{
    Clip c;
    c.id = newId();
    c.source = ContentSource::Capture;
    c.inputId = deviceId;
    c.name = description;
    return c;
}

Clip Clip::fromNdi(const QString &sourceName)
{
    Clip c;
    c.id = newId();
    c.source = ContentSource::Ndi;
    c.inputId = sourceName;
    return c;
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

int LedWall::enabledPanelCount() const
{
    int off = 0;
    for (const QPoint &p : disabledPanels)
        if (p.x() >= 0 && p.x() < cols && p.y() >= 0 && p.y() < rows)
            ++off;
    return cols * rows - off;
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
                       {"inputId", c.inputId},
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
    c.inputId = o["inputId"].toString();
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
                                      {"maskToPanels", maskToPanels},
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
                                   {"color", w.color.name()},
                                   {"disabledPanels", [&w] {
                                        QJsonArray a;
                                        for (const QPoint &p : w.disabledPanels)
                                            a.append(QJsonArray{p.x(), p.y()});
                                        return a;
                                    }()}});
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
                                        {"brightness", sl.brightness},
                                        {"gainR", sl.gainR},
                                        {"gainG", sl.gainG},
                                        {"gainB", sl.gainB},
                                        {"gamma", sl.gamma},
                                        {"blackLevel", sl.blackLevel}});
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

    QJsonArray controlArr;
    for (const ControlMapping &m : controls) {
        controlArr.append(QJsonObject{{"action", int(m.action)},
                                      {"layer", m.layer},
                                      {"column", m.column},
                                      {"midiSet", m.midiSet},
                                      {"midiCC", m.midiIsCC},
                                      {"midiChannel", m.midiChannel},
                                      {"midiNumber", m.midiNumber}});
    }
    root["controls"] = controlArr;
    root["dmx"] = QJsonObject{{"enabled", dmx.enabled},
                              {"protocol", dmx.protocol == DmxProtocol::Sacn ? "sacn" : "artnet"},
                              {"universe", dmx.universe},
                              {"address", dmx.address},
                              {"layers", dmx.layers}};
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
    p.maskToPanels = comp["maskToPanels"].toBool(false);
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
        for (const QJsonValue &pv : o["disabledPanels"].toArray()) {
            const QJsonArray xy = pv.toArray();
            w.disabledPanels.append(QPoint(xy.at(0).toInt(), xy.at(1).toInt()));
        }
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
            sl.gainR = so["gainR"].toDouble(1.0);
            sl.gainG = so["gainG"].toDouble(1.0);
            sl.gainB = so["gainB"].toDouble(1.0);
            sl.gamma = qBound(0.1, so["gamma"].toDouble(1.0), 5.0);
            sl.blackLevel = qBound(0.0, so["blackLevel"].toDouble(0.0), 0.5);
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
    for (const QJsonValue &v : root["controls"].toArray()) {
        const QJsonObject o = v.toObject();
        ControlMapping m;
        m.action = ControlAction(qBound(0, o["action"].toInt(), kControlActionCount - 1));
        m.layer = qMax(0, o["layer"].toInt());
        m.column = qMax(0, o["column"].toInt());
        m.midiSet = o["midiSet"].toBool(false);
        m.midiIsCC = o["midiCC"].toBool(false);
        m.midiChannel = qBound(0, o["midiChannel"].toInt(), 15);
        m.midiNumber = qBound(0, o["midiNumber"].toInt(), 127);
        p.controls.append(m);
    }

    const QJsonObject dmx = root["dmx"].toObject();
    p.dmx.enabled = dmx["enabled"].toBool(false);
    p.dmx.protocol = dmx["protocol"].toString() == QLatin1String("sacn") ? DmxProtocol::Sacn : DmxProtocol::ArtNet;
    p.dmx.universe = p.dmx.protocol == DmxProtocol::Sacn ? qBound(1, dmx["universe"].toInt(1), 63999)
                                                         : qBound(0, dmx["universe"].toInt(0), 32767);
    p.dmx.address = qBound(1, dmx["address"].toInt(1), 512);
    p.dmx.layers = qBound(1, dmx["layers"].toInt(4), 32);

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
