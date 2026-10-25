#include "model/Project.h"

#include <QTransform>

#include <QFileInfo>
#include <QRegularExpression>
#include <QJsonArray>
#include <QtMath>
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
    case BlendMode::Subtract: return QStringLiteral("Subtract");
    case BlendMode::Difference: return QStringLiteral("Difference");
    case BlendMode::Exclusion: return QStringLiteral("Exclusion");
    case BlendMode::Overlay: return QStringLiteral("Overlay");
    case BlendMode::SoftLight: return QStringLiteral("Soft light");
    case BlendMode::HardLight: return QStringLiteral("Hard light");
    case BlendMode::ColorDodge: return QStringLiteral("Colour dodge");
    case BlendMode::ColorBurn: return QStringLiteral("Colour burn");
    case BlendMode::Hue: return QStringLiteral("Hue");
    case BlendMode::Saturation: return QStringLiteral("Saturation");
    case BlendMode::Color: return QStringLiteral("Colour");
    case BlendMode::Luminosity: return QStringLiteral("Luminosity");
    case BlendMode::AlphaMask: return QStringLiteral("Mask: alpha");
    case BlendMode::LumaMask: return QStringLiteral("Mask: luma");
    case BlendMode::InverseLumaMask: return QStringLiteral("Mask: inverse luma");
    }
    return {};
}

QString audioSignalName(AudioSignal s)
{
    switch (s) {
    case AudioSignal::Level: return QStringLiteral("Level");
    case AudioSignal::Bass: return QStringLiteral("Bass");
    case AudioSignal::Mid: return QStringLiteral("Mid");
    case AudioSignal::Treble: return QStringLiteral("Treble");
    case AudioSignal::Beat: return QStringLiteral("Beat");
    }
    return {};
}

QTransform LedWall::transform() const
{
    QTransform t;
    if (angle == 0.0)
        return t;
    const QPointF c = QRectF(rect()).center();
    t.translate(c.x(), c.y());
    t.rotate(angle); // clockwise on screen (y points down)
    t.translate(-c.x(), -c.y());
    return t;
}

QPolygonF LedWall::outline() const
{
    return transform().map(QPolygonF(QRectF(rect())));
}

QPolygonF LedWall::panelPolygon(int col, int row) const
{
    return transform().map(QPolygonF(QRectF(panelRect(col, row))));
}

QPointF LedWall::panelCentre(int col, int row) const
{
    return transform().map(QRectF(panelRect(col, row)).center());
}

QRect LedWall::bounds() const
{
    return outline().boundingRect().toAlignedRect();
}

namespace {

// Catmull-Rom through p0..p3 at f in [0, 1] between p1 and p2.
QPointF catmullRom(const QPointF &p0, const QPointF &p1, const QPointF &p2, const QPointF &p3, double f)
{
    const double f2 = f * f, f3 = f2 * f;
    return 0.5 * ((2 * p1) + (-p0 + p2) * f + (2 * p0 - 5 * p1 + 4 * p2 - p3) * f2 + (-p0 + 3 * p1 - 3 * p2 + p3) * f3);
}

QJsonArray masksToJson(const QVector<ProjectionMask> &masks)
{
    QJsonArray arr;
    for (const ProjectionMask &m : masks) {
        QJsonArray pts;
        for (const QPointF &pt : m.points)
            pts << pt.x() << pt.y();
        arr.append(QJsonObject{{"name", m.name}, {"enabled", m.enabled}, {"invert", m.invert}, {"smooth", m.smooth},
                               {"feather", m.feather}, {"points", pts}});
    }
    return arr;
}

QVector<ProjectionMask> masksFromJson(const QJsonArray &arr)
{
    QVector<ProjectionMask> out;
    for (const QJsonValue &mv : arr) {
        const QJsonObject mo = mv.toObject();
        ProjectionMask m;
        m.name = mo["name"].toString(m.name);
        m.enabled = mo["enabled"].toBool(true);
        m.invert = mo["invert"].toBool(false);
        m.smooth = mo["smooth"].toBool(false);
        m.feather = qMax(0.0, mo["feather"].toDouble(20.0));
        const QJsonArray pts = mo["points"].toArray();
        for (int k = 0; k + 1 < pts.size(); k += 2)
            m.points << QPointF(pts[k].toDouble(), pts[k + 1].toDouble());
        if (m.points.size() >= 3)
            out.append(m);
    }
    return out;
}

} // namespace

QPolygonF ProjectionMask::outline() const
{
    const int n = int(points.size());
    if (!smooth || n < 3)
        return points;
    // Closed Catmull-Rom: the curve passes through every point.
    constexpr int kSteps = 16;
    QPolygonF out;
    out.reserve(n * kSteps);
    for (int i = 0; i < n; ++i) {
        const QPointF &p0 = points[(i + n - 1) % n], &p1 = points[i], &p2 = points[(i + 1) % n], &p3 = points[(i + 2) % n];
        for (int k = 0; k < kSteps; ++k)
            out << catmullRom(p0, p1, p2, p3, double(k) / kSteps);
    }
    return out;
}

ProjectionMask ProjectionMask::rectangle(const QRectF &r)
{
    ProjectionMask m;
    m.points = QPolygonF({r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()});
    return m;
}

ProjectionMask ProjectionMask::ellipse(const QRectF &r)
{
    // Catmull-Rom through 8 points on the ellipse stays within about 1% of it.
    ProjectionMask m;
    m.smooth = true;
    for (int k = 0; k < 8; ++k) {
        const double a = k * M_PI / 4.0;
        m.points << QPointF(qRound(r.center().x() + r.width() / 2 * qCos(a)), qRound(r.center().y() + r.height() / 2 * qSin(a)));
    }
    return m;
}

QPointF Slice::outputPoint(double s, double t) const
{
    if (isMeshed()) {
        // Bicubic Catmull-Rom over the control grid; the edge rows/columns are repeated.
        const double u = qBound(0.0, s, 1.0) * (meshW - 1), v = qBound(0.0, t, 1.0) * (meshH - 1);
        const int i = qMin(int(u), meshW - 2), j = qMin(int(v), meshH - 2);
        const double fu = u - i, fv = v - j;
        auto at = [this](int x, int y) {
            return mesh[qBound(0, y, meshH - 1) * meshW + qBound(0, x, meshW - 1)];
        };
        QPointF rows[4];
        for (int k = 0; k < 4; ++k) {
            const int y = j - 1 + k;
            rows[k] = catmullRom(at(i - 1, y), at(i, y), at(i + 1, y), at(i + 2, y), fu);
        }
        return catmullRom(rows[0], rows[1], rows[2], rows[3], fv);
    }
    if (isWarped()) {
        QTransform t2;
        if (QTransform::quadToQuad(QPolygonF({QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)}), warp, t2))
            return t2.map(QPointF(s, t));
    }
    return QPointF(output.x() + s * output.width(), output.y() + t * output.height());
}

void Slice::resetMesh(int w, int h)
{
    w = qBound(2, w, 33);
    h = qBound(2, h, 33);
    QVector<QPointF> pts;
    pts.reserve(w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            pts << outputPointFlat(double(x) / (w - 1), double(y) / (h - 1));
    meshW = w;
    meshH = h;
    mesh = pts;
}

void Slice::resizeMesh(int w, int h)
{
    w = qBound(2, w, 33);
    h = qBound(2, h, 33);
    QVector<QPointF> pts;
    pts.reserve(w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            pts << outputPoint(double(x) / (w - 1), double(y) / (h - 1));
    meshW = w;
    meshH = h;
    mesh = pts;
}

QPointF Slice::outputPointFlat(double s, double t) const
{
    Slice flat = *this;
    flat.mesh.clear();
    flat.meshW = flat.meshH = 0;
    return flat.outputPoint(s, t);
}

QPolygonF Slice::outputQuad() const
{
    if (isMeshed()) // the mesh's outer corners
        return QPolygonF({mesh[0], mesh[meshW - 1], mesh[meshW * meshH - 1], mesh[meshW * (meshH - 1)]});
    if (isWarped())
        return warp;
    const QRectF r(output);
    return QPolygonF({r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()});
}

QPolygonF Slice::inputPolygon() const
{
    const QRectF r(input);
    QPolygonF poly({r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()});
    if (angle == 0.0)
        return poly;
    QTransform t;
    t.translate(r.center().x(), r.center().y());
    t.rotate(angle); // clockwise on screen (y points down)
    t.translate(-r.center().x(), -r.center().y());
    return t.map(poly);
}

QString playModeName(PlayMode m)
{
    switch (m) {
    case PlayMode::Loop: return QStringLiteral("Loop");
    case PlayMode::Bounce: return QStringLiteral("Bounce (back and forth)");
    case PlayMode::OnceHold: return QStringLiteral("Play once and hold");
    case PlayMode::OnceClear: return QStringLiteral("Play once and clear");
    }
    return {};
}

QString triggerModeName(TriggerMode m)
{
    switch (m) {
    case TriggerMode::Retrigger: return QStringLiteral("Retrigger (restart)");
    case TriggerMode::Toggle: return QStringLiteral("Toggle (trigger again stops)");
    case TriggerMode::Piano: return QStringLiteral("Piano (plays while held)");
    }
    return {};
}

QString transitionStyleName(TransitionStyle t)
{
    switch (t) {
    case TransitionStyle::Crossfade: return QStringLiteral("Crossfade");
    case TransitionStyle::WipeLeft: return QStringLiteral("Wipe left");
    case TransitionStyle::WipeRight: return QStringLiteral("Wipe right");
    case TransitionStyle::WipeUp: return QStringLiteral("Wipe up");
    case TransitionStyle::WipeDown: return QStringLiteral("Wipe down");
    case TransitionStyle::PushLeft: return QStringLiteral("Push left");
    case TransitionStyle::PushRight: return QStringLiteral("Push right");
    case TransitionStyle::PushUp: return QStringLiteral("Push up");
    case TransitionStyle::PushDown: return QStringLiteral("Push down");
    case TransitionStyle::Zoom: return QStringLiteral("Zoom in");
    }
    return {};
}

QString autopilotName(Autopilot a)
{
    switch (a) {
    case Autopilot::Off: return QStringLiteral("Off");
    case Autopilot::Next: return QStringLiteral("Next clip");
    case Autopilot::Random: return QStringLiteral("Random clip");
    }
    return {};
}

const LayerGroup *Project::groupById(const QString &id) const
{
    for (const LayerGroup &g : groups)
        if (g.id == id)
            return &g;
    return nullptr;
}

double Project::crossfadeGain(CrossfadeSide side) const
{
    if (side == CrossfadeSide::A)
        return qBound(0.0, (1.0 - crossfader) * 2.0, 1.0);
    if (side == CrossfadeSide::B)
        return qBound(0.0, crossfader * 2.0, 1.0);
    return 1.0;
}

QString fixtureKindName(FixtureKind k)
{
    switch (k) {
    case FixtureKind::Strip: return QStringLiteral("LED strip");
    case FixtureKind::Matrix: return QStringLiteral("LED matrix");
    case FixtureKind::Point: return QStringLiteral("Single fixture");
    }
    return {};
}

QString colorOrderName(ColorOrder o)
{
    static const char *const names[] = {"RGB", "GRB", "BRG", "RBG", "GBR", "BGR", "RGBW", "White"};
    return QString::fromLatin1(names[int(o)]);
}

int channelsPerPixel(ColorOrder o)
{
    return o == ColorOrder::RGBW ? 4 : o == ColorOrder::White ? 1 : 3;
}

int PixelFixture::pixelCount() const
{
    switch (kind) {
    case FixtureKind::Strip: return qMax(1, pixels);
    case FixtureKind::Matrix: return qMax(1, columns) * qMax(1, rows);
    case FixtureKind::Point: return 1;
    }
    return 1;
}

QVector<QPointF> PixelFixture::samplePoints() const
{
    QVector<QPointF> pts;
    if (kind == FixtureKind::Point) {
        pts << start;
    } else if (kind == FixtureKind::Strip) {
        const int n = qMax(1, pixels);
        for (int i = 0; i < n; ++i)
            pts << (n == 1 ? start : start + (end - start) * (double(i) / (n - 1)));
    } else {
        // Pixel centres, row by row from the top-left; serpentine rows run back the other way.
        const int c = qMax(1, columns), r = qMax(1, rows);
        for (int y = 0; y < r; ++y)
            for (int i = 0; i < c; ++i) {
                const int x = serpentine && y % 2 == 1 ? c - 1 - i : i;
                pts << QPointF(rect.x() + (x + 0.5) * rect.width() / c, rect.y() + (y + 0.5) * rect.height() / r);
            }
    }
    return pts;
}

QString cueActionName(CueAction a)
{
    switch (a) {
    case CueAction::TriggerColumn: return QStringLiteral("Trigger column");
    case CueAction::TriggerClip: return QStringLiteral("Trigger clip");
    case CueAction::ClearLayer: return QStringLiteral("Stop layer");
    case CueAction::ClearAll: return QStringLiteral("Stop all layers");
    case CueAction::Master: return QStringLiteral("Master to");
    case CueAction::Blackout: return QStringLiteral("Blackout");
    case CueAction::Crossfader: return QStringLiteral("Crossfader to");
    case CueAction::Bpm: return QStringLiteral("Tempo to");
    }
    return {};
}

QString Cue::description() const
{
    switch (action) {
    case CueAction::TriggerColumn: return QStringLiteral("Column %1").arg(column + 1);
    case CueAction::TriggerClip: return QStringLiteral("Layer %1, clip %2").arg(layer + 1).arg(column + 1);
    case CueAction::ClearLayer: return QStringLiteral("Stop layer %1").arg(layer + 1);
    case CueAction::ClearAll: return QStringLiteral("Stop everything");
    case CueAction::Master: return QStringLiteral("Master %1%").arg(qRound(value * 100));
    case CueAction::Blackout: return value >= 0.5 ? QStringLiteral("Blackout on") : QStringLiteral("Blackout off");
    case CueAction::Crossfader: return QStringLiteral("Crossfader %1%").arg(qRound(value * 100));
    case CueAction::Bpm: return QStringLiteral("%1 BPM").arg(value, 0, 'f', 1);
    }
    return {};
}

QString timecodeText(double seconds, double fps)
{
    if (seconds < 0)
        return QStringLiteral("--:--:--:--");
    const double rate = qMax(1.0, fps);
    const qint64 frames = qint64(seconds * rate + 1e-6); // the frame in progress, as timecode counts
    const int perSecond = qRound(rate);
    const qint64 total = frames / perSecond;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(total / 3600, 2, 10, QLatin1Char('0'))
        .arg((total / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'))
        .arg(frames % perSecond, 2, 10, QLatin1Char('0'));
}

double timecodeFromText(const QString &text, double fps, bool *ok)
{
    const QStringList parts = text.trimmed().split(QRegularExpression(QStringLiteral("[:;.]")));
    bool good = parts.size() >= 3 && parts.size() <= 4;
    double seconds = 0;
    if (good) {
        const int n = int(parts.size());
        int fields[4] = {0, 0, 0, 0};
        for (int i = 0; i < n && good; ++i)
            fields[i] = parts[i].toInt(&good);
        // HH:MM:SS[:FF]
        seconds = fields[0] * 3600.0 + fields[1] * 60.0 + fields[2] + (n == 4 ? fields[3] / qMax(1.0, fps) : 0.0);
    }
    if (ok)
        *ok = good;
    return good ? seconds : -1.0;
}

QString tempoSourceName(TempoSource s)
{
    switch (s) {
    case TempoSource::Internal: return QStringLiteral("Internal (tap)");
    case TempoSource::MidiClock: return QStringLiteral("MIDI clock");
    case TempoSource::AudioBeats: return QStringLiteral("Audio beats");
    }
    return {};
}

QString triggerQuantizeName(TriggerQuantize q)
{
    switch (q) {
    case TriggerQuantize::Off: return QStringLiteral("Trigger now");
    case TriggerQuantize::Beat: return QStringLiteral("On the next beat");
    case TriggerQuantize::Bar: return QStringLiteral("On the next bar");
    }
    return {};
}

QString lfoShapeName(LfoShape s)
{
    switch (s) {
    case LfoShape::Sine: return QStringLiteral("Sine");
    case LfoShape::Triangle: return QStringLiteral("Triangle");
    case LfoShape::SawUp: return QStringLiteral("Saw up");
    case LfoShape::SawDown: return QStringLiteral("Saw down");
    case LfoShape::Square: return QStringLiteral("Square");
    case LfoShape::Random: return QStringLiteral("Random (stepped)");
    }
    return {};
}

QString cabinetArrangeName(CabinetArrange a)
{
    switch (a) {
    case CabinetArrange::WallShape: return QStringLiteral("Same shape as the wall");
    case CabinetArrange::PackRows: return QStringLiteral("Packed in rows");
    case CabinetArrange::PackColumns: return QStringLiteral("Packed in columns");
    }
    return {};
}

QString cabinetOrderName(CabinetOrder o)
{
    switch (o) {
    case CabinetOrder::Rows: return QStringLiteral("Rows");
    case CabinetOrder::RowsSerpentine: return QStringLiteral("Rows, serpentine");
    case CabinetOrder::Columns: return QStringLiteral("Columns");
    case CabinetOrder::ColumnsSerpentine: return QStringLiteral("Columns, serpentine");
    }
    return {};
}

QString audioTargetName(AudioTarget t)
{
    switch (t) {
    case AudioTarget::LayerOpacity: return QStringLiteral("Layer opacity");
    case AudioTarget::Master: return QStringLiteral("Master");
    case AudioTarget::NextColumn: return QStringLiteral("Next column (on beat)");
    case AudioTarget::NextClip: return QStringLiteral("Layer next clip (on beat)");
    case AudioTarget::EffectParam: return QStringLiteral("Effect parameter");
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
    case ControlAction::LayerVolume: return QStringLiteral("Layer volume");
    case ControlAction::AudioMaster: return QStringLiteral("Audio master");
    case ControlAction::AudioMute: return QStringLiteral("Audio mute");
    case ControlAction::EffectParam: return QStringLiteral("Effect parameter");
    case ControlAction::TapTempo: return QStringLiteral("Tap tempo");
    case ControlAction::TempoResync: return QStringLiteral("Tempo resync (bar start)");
    case ControlAction::Crossfader: return QStringLiteral("Crossfader (A/B)");
    case ControlAction::CueGo: return QStringLiteral("Cue GO");
    case ControlAction::CueBack: return QStringLiteral("Cue back");
    }
    return {};
}

bool controlActionUsesLayer(ControlAction a)
{
    return a == ControlAction::TriggerClip || a == ControlAction::LayerOpacity || a == ControlAction::LayerBypass
           || a == ControlAction::LayerSolo || a == ControlAction::ClearLayer || a == ControlAction::LayerVolume
           || a == ControlAction::EffectParam;
}

bool controlActionUsesColumn(ControlAction a)
{
    return a == ControlAction::TriggerColumn || a == ControlAction::TriggerClip;
}

static QJsonArray effectsToJson(const QVector<EffectInstance> &chain);
static QVector<EffectInstance> effectsFromJson(const QJsonArray &arr);

QJsonArray effectChainToJson(const QVector<EffectInstance> &chain)
{
    return effectsToJson(chain);
}

QVector<EffectInstance> effectChainFromJson(const QJsonArray &arr)
{
    return effectsFromJson(arr);
}

static QJsonArray effectsToJson(const QVector<EffectInstance> &chain)
{
    QJsonArray arr;
    for (const EffectInstance &e : chain) {
        QJsonArray params;
        for (double v : e.params)
            params.append(v);
        arr.append(QJsonObject{{"type", e.type}, {"enabled", e.enabled}, {"params", params}});
    }
    return arr;
}

static QVector<EffectInstance> effectsFromJson(const QJsonArray &arr)
{
    QVector<EffectInstance> chain;
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        EffectInstance e;
        e.type = o["type"].toString();
        e.enabled = o["enabled"].toBool(true);
        for (const QJsonValue &p : o["params"].toArray())
            e.params.append(p.toDouble());
        if (!e.type.isEmpty())
            chain.append(e);
    }
    return chain;
}

QString ControlMapping::targetText() const
{
    QStringList parts;
    if (action == ControlAction::EffectParam) {
        return QStringLiteral("%1, effect %2, parameter %3")
            .arg(layer < 0 ? QStringLiteral("composition") : QStringLiteral("layer %1").arg(layer + 1))
            .arg(column + 1)
            .arg(param + 1);
    }
    if (controlActionUsesLayer(action))
        parts << QStringLiteral("layer %1").arg(layer + 1);
    if (controlActionUsesColumn(action))
        parts << QStringLiteral("column %1").arg(column + 1);
    return parts.join(QStringLiteral(", "));
}

QString ControlMapping::midiText() const
{
    QStringList parts;
    if (midiSet)
        parts << QStringLiteral("%1 %2, ch %3").arg(midiIsCC ? QStringLiteral("CC") : QStringLiteral("Note")).arg(midiNumber).arg(midiChannel + 1);
    if (!key.isEmpty())
        parts << QStringLiteral("key %1").arg(key);
    return parts.isEmpty() ? QStringLiteral("not learned") : parts.join(QStringLiteral(" / "));
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
    case ContentSource::Ndi:
    case ContentSource::Share: return inputId;
    case ContentSource::Text: return text.section(QLatin1Char('\n'), 0, 0).left(40);
    case ContentSource::Generator: return generator;
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

Clip Clip::fromShare(const QString &sourceName)
{
    Clip c;
    c.id = newId();
    c.source = ContentSource::Share;
    c.inputId = sourceName;
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

Clip Clip::fromText(const QString &text)
{
    Clip c;
    c.id = newId();
    c.source = ContentSource::Text;
    c.text = text;
    c.scale = ScaleMode::Stretch;
    return c;
}

Clip Clip::fromGenerator(const QString &generatorId, const QVector<double> &params)
{
    Clip c;
    c.id = newId();
    c.source = ContentSource::Generator;
    c.generator = generatorId;
    c.genParams = params;
    c.solidColor = QColor(0, 200, 255);
    c.color2 = Qt::black;
    return c;
}

QString textMotionName(TextMotion m)
{
    switch (m) {
    case TextMotion::Still: return QStringLiteral("Still");
    case TextMotion::ScrollLeft: return QStringLiteral("Scroll left (ticker)");
    case TextMotion::ScrollRight: return QStringLiteral("Scroll right");
    case TextMotion::ScrollUp: return QStringLiteral("Scroll up (credits)");
    case TextMotion::ScrollDown: return QStringLiteral("Scroll down");
    case TextMotion::Blink: return QStringLiteral("Blink");
    }
    return {};
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
                       {"playMode", int(c.playMode)},
                       {"speed", c.speed},
                       {"reverse", c.reverse},
                       {"in", c.inPoint},
                       {"out", c.outPoint},
                       {"trigger", int(c.trigger)},
                       {"transform", QJsonArray{c.posX, c.posY, c.scaleBy, c.rotation, c.opacity}},
                       {"effects", effectsToJson(c.effects)},
                       {"beatSync", c.beatSync},
                       {"text", c.text},
                       {"font", c.fontFamily},
                       {"fontSize", c.fontSize},
                       {"bold", c.bold},
                       {"outlineColor", c.outlineColor.name(QColor::HexArgb)},
                       {"outlineWidth", c.outlineWidth},
                       {"textBackground", c.textBackground.name(QColor::HexArgb)},
                       {"textAlign", c.textAlign},
                       {"textMotion", int(c.textMotion)},
                       {"textSpeed", c.textSpeed},
                       {"generator", c.generator},
                       {"genParams", [&c] {
                            QJsonArray a;
                            for (double v : c.genParams)
                                a << v;
                            return a;
                        }()},
                       {"color2", c.color2.name(QColor::HexArgb)},
                       {"volume", c.volume},
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
    // Older files: "loop" (true = loop, false = play once and hold).
    c.playMode = o.contains("playMode") ? PlayMode(qBound(0, o["playMode"].toInt(), kPlayModeCount - 1))
                                        : (o["loop"].toBool(true) ? PlayMode::Loop : PlayMode::OnceHold);
    c.speed = qBound(0.05, o["speed"].toDouble(1.0), 8.0);
    c.reverse = o["reverse"].toBool(false);
    c.inPoint = qBound(0.0, o["in"].toDouble(0.0), 1.0);
    c.outPoint = qBound(c.inPoint, o["out"].toDouble(1.0), 1.0);
    c.trigger = TriggerMode(qBound(0, o["trigger"].toInt(0), kTriggerModeCount - 1));
    const QJsonArray t = o["transform"].toArray();
    if (t.size() == 5) {
        c.posX = t[0].toDouble();
        c.posY = t[1].toDouble();
        c.scaleBy = qBound(0.01, t[2].toDouble(1.0), 20.0);
        c.rotation = t[3].toDouble();
        c.opacity = qBound(0.0, t[4].toDouble(1.0), 1.0);
    }
    c.effects = effectsFromJson(o["effects"].toArray());
    c.beatSync = qBound(0, o["beatSync"].toInt(0), 1024);
    c.text = o["text"].toString();
    c.fontFamily = o["font"].toString();
    c.fontSize = qBound(0, o["fontSize"].toInt(0), 4000);
    c.bold = o["bold"].toBool(true);
    c.outlineColor = QColor(o["outlineColor"].toString(QStringLiteral("#ff000000")));
    c.outlineWidth = qBound(0.0, o["outlineWidth"].toDouble(0.0), 200.0);
    c.textBackground = QColor(o["textBackground"].toString(QStringLiteral("#00000000")));
    c.textAlign = qBound(0, o["textAlign"].toInt(1), 2);
    c.textMotion = TextMotion(qBound(0, o["textMotion"].toInt(0), kTextMotionCount - 1));
    c.textSpeed = qBound(0.0, o["textSpeed"].toDouble(200.0), 10000.0);
    c.generator = o["generator"].toString();
    for (const QJsonValue &v : o["genParams"].toArray())
        c.genParams << v.toDouble();
    c.color2 = QColor(o["color2"].toString(QStringLiteral("#ff000000")));
    c.volume = qBound(0.0, o["volume"].toDouble(1.0), 2.0);
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
                                      {"blackout", blackout},
                                      {"audioMaster", audioMaster},
                                      {"audioMuted", audioMuted}};

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
                                   {"angle", w.angle},
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
                                        {"angle", sl.angle},
                                        {"meshW", sl.meshW},
                                        {"meshH", sl.meshH},
                                        {"mesh", [&sl] {
                                             QJsonArray a;
                                             for (const QPointF &pt : sl.mesh)
                                                 a << pt.x() << pt.y();
                                             return a;
                                         }()},
                                        {"blend", QJsonArray{sl.blendLeft, sl.blendRight, sl.blendTop, sl.blendBottom}},
                                        {"blendCurve", sl.blendCurve},
                                        {"blendGamma", sl.blendGamma},
                                        {"warp", [&sl] {
                                             QJsonArray a;
                                             for (const QPointF &pt : sl.warp)
                                                 a << pt.x() << pt.y();
                                             return a;
                                         }()},
                                        {"wall", sl.wall},
                                        {"cabinet", QJsonArray{sl.cabinet.x(), sl.cabinet.y()}},
                                        {"port", sl.port},
                                        {"brightness", sl.brightness},
                                        {"gainR", sl.gainR},
                                        {"gainG", sl.gainG},
                                        {"gainB", sl.gainB},
                                        {"gamma", sl.gamma},
                                        {"blackLevel", sl.blackLevel}});
        }
        QJsonArray layoutArr;
        for (const CabinetLayout &c : s.cabinetLayouts)
            layoutArr.append(QJsonObject{{"wall", c.wall},
                                         {"arrange", int(c.arrange)},
                                         {"order", int(c.order)},
                                         {"startCorner", c.startCorner},
                                         {"x", c.x},
                                         {"y", c.y},
                                         {"gap", c.gap},
                                         {"rotation", c.rotation},
                                         {"flipH", c.flipH},
                                         {"flipV", c.flipV},
                                         {"portCapacity", c.portCapacity},
                                         {"firstPort", c.firstPort},
                                         {"newLinePerPort", c.newLinePerPort}});
        const QJsonArray maskArr = masksToJson(s.masks);
        screenArr.append(QJsonObject{{"name", s.name},
                                     {"width", s.width},
                                     {"height", s.height},
                                     {"display", s.display},
                                     {"enabled", s.enabled},
                                     {"slices", sliceArr},
                                     {"cabinetLayouts", layoutArr},
                                     {"masks", maskArr}});
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
                                    {"volume", l.volume},
                                    {"blend", int(l.blend)},
                                    {"bypass", l.bypass},
                                    {"solo", l.solo},
                                    {"targetWall", l.targetWall},
                                    {"transitionMs", l.transitionMs},
                                    {"transition", int(l.transition)},
                                    {"crossfade", int(l.crossfade)},
                                    {"group", l.group},
                                    {"autopilot", int(l.autopilot)},
                                    {"autopilotSeconds", l.autopilotSeconds},
                                    {"activeColumn", l.activeColumn},
                                    {"effects", effectsToJson(l.effects)},
                                    {"clips", clipArr}});
    }
    root["deck"] = QJsonObject{{"columns", columns}, {"layers", layerArr}};

    QJsonArray controlArr;
    for (const ControlMapping &m : controls) {
        controlArr.append(QJsonObject{{"action", int(m.action)},
                                      {"layer", m.layer},
                                      {"column", m.column},
                                      {"param", m.param},
                                      {"midiSet", m.midiSet},
                                      {"midiCC", m.midiIsCC},
                                      {"midiChannel", m.midiChannel},
                                      {"midiNumber", m.midiNumber},
                                      {"key", m.key}});
    }
    root["controls"] = controlArr;
    root["midiFeedback"] = QJsonObject{{"enabled", midiFeedback.enabled},
                                       {"off", midiFeedback.offValue},
                                       {"loaded", midiFeedback.loadedValue},
                                       {"active", midiFeedback.activeValue}};
    root["compEffects"] = effectsToJson(compEffects);
    root["tempo"] = QJsonObject{{"bpm", bpm}, {"source", int(tempoSource)}, {"quantize", int(quantize)}};
    QJsonArray lfoArr;
    for (const Lfo &l : lfos)
        lfoArr.append(QJsonObject{{"enabled", l.enabled}, {"target", int(l.target)}, {"layer", l.layer},
                                  {"effect", l.effect}, {"param", l.param}, {"shape", int(l.shape)},
                                  {"beats", l.beats}, {"amount", l.amount}, {"phase", l.phase}});
    root["lfos"] = lfoArr;
    root["crossfader"] = crossfader;
    QJsonArray groupArr;
    for (const LayerGroup &g : groups)
        groupArr.append(QJsonObject{{"id", g.id}, {"name", g.name}, {"opacity", g.opacity}, {"blend", int(g.blend)},
                                    {"bypass", g.bypass}, {"effects", effectsToJson(g.effects)}});
    root["groups"] = groupArr;
    QJsonArray deckArr;
    for (const Deck &d : decks)
        deckArr.append(QJsonObject{{"name", d.name}, {"first", d.firstColumn}, {"count", d.columnCount}});
    root["decks"] = deckArr;
    QJsonArray cueArr;
    for (const Cue &c : cues)
        cueArr.append(QJsonObject{{"name", c.name}, {"action", int(c.action)}, {"layer", c.layer}, {"column", c.column},
                                  {"value", c.value}, {"time", c.time}});
    root["cues"] = cueArr;
    QJsonArray fixArr;
    for (const PixelFixture &f : fixtures)
        fixArr.append(QJsonObject{{"name", f.name}, {"enabled", f.enabled}, {"kind", int(f.kind)},
                                  {"start", QJsonArray{f.start.x(), f.start.y()}}, {"end", QJsonArray{f.end.x(), f.end.y()}},
                                  {"rect", QJsonArray{f.rect.x(), f.rect.y(), f.rect.width(), f.rect.height()}},
                                  {"pixels", f.pixels}, {"columns", f.columns}, {"rows", f.rows},
                                  {"serpentine", f.serpentine}, {"universe", f.universe}, {"address", f.address},
                                  {"order", int(f.order)}});
    root["fixtures"] = fixArr;
    root["compMasks"] = masksToJson(compMasks);
    root["dmxOut"] = QJsonObject{{"enabled", dmxOut.enabled}, {"protocol", int(dmxOut.protocol)},
                                 {"target", dmxOut.target}, {"rate", dmxOut.rate}};
    root["timecode"] = QJsonObject{{"source", int(timecodeSource)}, {"fps", timecodeFps}};
    QJsonArray linkArr;
    for (const AudioLink &l : audioReactive.links)
        linkArr.append(QJsonObject{{"signal", int(l.signal)}, {"target", int(l.target)}, {"layer", l.layer},
                                   {"effect", l.effect}, {"param", l.param}, {"amount", l.amount}});
    root["audioReactive"] = QJsonObject{{"enabled", audioReactive.enabled},
                                        {"sensitivity", audioReactive.sensitivity},
                                        {"links", linkArr}};
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
    p.audioMaster = qBound(0.0, comp["audioMaster"].toDouble(1.0), 1.0);
    p.audioMuted = comp["audioMuted"].toBool(false);

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
        w.angle = o["angle"].toDouble(0.0);
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
            sl.angle = so["angle"].toDouble(0.0);
            sl.meshW = so["meshW"].toInt(0);
            sl.meshH = so["meshH"].toInt(0);
            const QJsonArray mesh = so["mesh"].toArray();
            for (int k = 0; k + 1 < mesh.size(); k += 2)
                sl.mesh << QPointF(mesh[k].toDouble(), mesh[k + 1].toDouble());
            if (!sl.isMeshed()) {
                sl.mesh.clear();
                sl.meshW = sl.meshH = 0;
            }
            const QJsonArray blend = so["blend"].toArray();
            if (blend.size() == 4) {
                sl.blendLeft = qMax(0.0, blend[0].toDouble());
                sl.blendRight = qMax(0.0, blend[1].toDouble());
                sl.blendTop = qMax(0.0, blend[2].toDouble());
                sl.blendBottom = qMax(0.0, blend[3].toDouble());
            }
            sl.blendCurve = qBound(0.5, so["blendCurve"].toDouble(2.0), 8.0);
            sl.blendGamma = qBound(1.0, so["blendGamma"].toDouble(2.2), 3.5);
            const QJsonArray warp = so["warp"].toArray();
            if (warp.size() == 8)
                for (int k = 0; k < 8; k += 2)
                    sl.warp << QPointF(warp[k].toDouble(), warp[k + 1].toDouble());
            sl.wall = so["wall"].toInt(-1);
            const QJsonArray cab = so["cabinet"].toArray();
            sl.cabinet = cab.size() == 2 ? QPoint(cab[0].toInt(), cab[1].toInt()) : QPoint(-1, -1);
            sl.port = qMax(0, so["port"].toInt(0));
            sl.brightness = so["brightness"].toDouble(1.0);
            sl.gainR = so["gainR"].toDouble(1.0);
            sl.gainG = so["gainG"].toDouble(1.0);
            sl.gainB = so["gainB"].toDouble(1.0);
            sl.gamma = qBound(0.1, so["gamma"].toDouble(1.0), 5.0);
            sl.blackLevel = qBound(0.0, so["blackLevel"].toDouble(0.0), 0.5);
            s.slices.append(sl);
        }
        for (const QJsonValue &lv : o["cabinetLayouts"].toArray()) {
            const QJsonObject lo = lv.toObject();
            CabinetLayout c;
            c.wall = lo["wall"].toInt(0);
            c.arrange = CabinetArrange(qBound(0, lo["arrange"].toInt(0), 2));
            c.order = CabinetOrder(qBound(0, lo["order"].toInt(1), 3));
            c.startCorner = qBound(0, lo["startCorner"].toInt(0), 3);
            c.x = lo["x"].toInt(0);
            c.y = lo["y"].toInt(0);
            c.gap = qMax(0, lo["gap"].toInt(0));
            c.rotation = lo["rotation"].toInt(0) & 3;
            c.flipH = lo["flipH"].toBool(false);
            c.flipV = lo["flipV"].toBool(false);
            c.portCapacity = qMax(0, lo["portCapacity"].toInt(655360));
            c.firstPort = qMax(1, lo["firstPort"].toInt(1));
            c.newLinePerPort = lo["newLinePerPort"].toBool(false);
            s.cabinetLayouts.append(c);
        }
        s.masks = masksFromJson(o["masks"].toArray());
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
            l.volume = qBound(0.0, o["volume"].toDouble(1.0), 1.0);
            l.blend = BlendMode(qBound(0, o["blend"].toInt(0), kBlendModeCount - 1));
            l.crossfade = CrossfadeSide(qBound(0, o["crossfade"].toInt(0), 2));
            l.group = o["group"].toString();
            l.bypass = o["bypass"].toBool(false);
            l.solo = o["solo"].toBool(false);
            l.targetWall = o["targetWall"].toInt(-1);
            l.transitionMs = qMax(0, o["transitionMs"].toInt(0));
            l.transition = TransitionStyle(qBound(0, o["transition"].toInt(0), kTransitionStyleCount - 1));
            l.autopilot = Autopilot(qBound(0, o["autopilot"].toInt(0), 2));
            l.autopilotSeconds = qBound(0.5, o["autopilotSeconds"].toDouble(10.0), 3600.0);
            l.activeColumn = o["activeColumn"].toInt(-1);
            l.effects = effectsFromJson(o["effects"].toArray());
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
        clip.playMode = c["loop"].toBool(true) ? PlayMode::Loop : PlayMode::OnceHold;
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
    p.compEffects = effectsFromJson(root["compEffects"].toArray());
    const QJsonObject tempo = root["tempo"].toObject();
    p.bpm = qBound(20.0, tempo["bpm"].toDouble(120.0), 300.0);
    p.tempoSource = TempoSource(qBound(0, tempo["source"].toInt(0), 2));
    p.quantize = TriggerQuantize(qBound(0, tempo["quantize"].toInt(0), 2));
    p.crossfader = qBound(0.0, root["crossfader"].toDouble(0.5), 1.0);
    for (const QJsonValue &v : root["groups"].toArray()) {
        const QJsonObject o = v.toObject();
        LayerGroup g;
        g.id = o["id"].toString();
        g.name = o["name"].toString(g.name);
        g.opacity = qBound(0.0, o["opacity"].toDouble(1.0), 1.0);
        g.blend = BlendMode(qBound(0, o["blend"].toInt(0), kBlendModeCount - 1));
        g.bypass = o["bypass"].toBool(false);
        g.effects = effectsFromJson(o["effects"].toArray());
        if (!g.id.isEmpty())
            p.groups.append(g);
    }
    for (const QJsonValue &v : root["cues"].toArray()) {
        const QJsonObject o = v.toObject();
        Cue c;
        c.name = o["name"].toString();
        c.action = CueAction(qBound(0, o["action"].toInt(0), kCueActionCount - 1));
        c.layer = qMax(0, o["layer"].toInt(0));
        c.column = qMax(0, o["column"].toInt(0));
        c.value = o["value"].toDouble(1.0);
        c.time = o["time"].toDouble(-1.0);
        p.cues.append(c);
    }
    p.compMasks = masksFromJson(root["compMasks"].toArray());
    for (const QJsonValue &v : root["fixtures"].toArray()) {
        const QJsonObject o = v.toObject();
        PixelFixture f;
        f.name = o["name"].toString(f.name);
        f.enabled = o["enabled"].toBool(true);
        f.kind = FixtureKind(qBound(0, o["kind"].toInt(0), 2));
        const QJsonArray st = o["start"].toArray(), en = o["end"].toArray(), rc = o["rect"].toArray();
        if (st.size() == 2)
            f.start = QPointF(st[0].toDouble(), st[1].toDouble());
        if (en.size() == 2)
            f.end = QPointF(en[0].toDouble(), en[1].toDouble());
        if (rc.size() == 4)
            f.rect = QRectF(rc[0].toDouble(), rc[1].toDouble(), rc[2].toDouble(), rc[3].toDouble());
        f.pixels = qBound(1, o["pixels"].toInt(60), 100000);
        f.columns = qBound(1, o["columns"].toInt(16), 4096);
        f.rows = qBound(1, o["rows"].toInt(9), 4096);
        f.serpentine = o["serpentine"].toBool(true);
        f.universe = qBound(0, o["universe"].toInt(1), 63999);
        f.address = qBound(1, o["address"].toInt(1), 512);
        f.order = ColorOrder(qBound(0, o["order"].toInt(0), kColorOrderCount - 1));
        p.fixtures.append(f);
    }
    const QJsonObject dout = root["dmxOut"].toObject();
    p.dmxOut.enabled = dout["enabled"].toBool(false);
    p.dmxOut.protocol = DmxProtocol(qBound(0, dout["protocol"].toInt(0), 1));
    p.dmxOut.target = dout["target"].toString();
    p.dmxOut.rate = qBound(1, dout["rate"].toInt(40), 60);
    const QJsonObject tc = root["timecode"].toObject();
    p.timecodeSource = TimecodeSource(qBound(0, tc["source"].toInt(0), 2));
    p.timecodeFps = qBound(1.0, tc["fps"].toDouble(25.0), 120.0);
    for (const QJsonValue &v : root["decks"].toArray()) {
        const QJsonObject o = v.toObject();
        Deck d;
        d.name = o["name"].toString(d.name);
        d.firstColumn = qMax(0, o["first"].toInt(0));
        d.columnCount = qMax(1, o["count"].toInt(8));
        p.decks.append(d);
    }
    for (const QJsonValue &v : root["lfos"].toArray()) {
        const QJsonObject o = v.toObject();
        Lfo l;
        l.enabled = o["enabled"].toBool(true);
        l.target = AudioTarget(qBound(0, o["target"].toInt(0), kAudioTargetCount - 1));
        l.layer = qMax(-1, o["layer"].toInt(0));
        l.effect = qMax(0, o["effect"].toInt(0));
        l.param = qMax(0, o["param"].toInt(0));
        l.shape = LfoShape(qBound(0, o["shape"].toInt(0), kLfoShapeCount - 1));
        l.beats = qBound(0.0625, o["beats"].toDouble(4.0), 256.0);
        l.amount = qBound(0.0, o["amount"].toDouble(1.0), 1.0);
        l.phase = qBound(0.0, o["phase"].toDouble(0.0), 1.0);
        p.lfos.append(l);
    }
    {
        const QJsonObject o = root["midiFeedback"].toObject();
        p.midiFeedback.enabled = o["enabled"].toBool(true);
        p.midiFeedback.offValue = qBound(0, o["off"].toInt(0), 127);
        p.midiFeedback.loadedValue = qBound(0, o["loaded"].toInt(1), 127);
        p.midiFeedback.activeValue = qBound(0, o["active"].toInt(127), 127);
    }
    for (const QJsonValue &v : root["controls"].toArray()) {
        const QJsonObject o = v.toObject();
        ControlMapping m;
        m.action = ControlAction(qBound(0, o["action"].toInt(), kControlActionCount - 1));
        m.layer = qMax(-1, o["layer"].toInt());
        m.column = qMax(0, o["column"].toInt());
        m.param = qMax(0, o["param"].toInt());
        m.midiSet = o["midiSet"].toBool(false);
        m.midiIsCC = o["midiCC"].toBool(false);
        m.midiChannel = qBound(0, o["midiChannel"].toInt(), 15);
        m.midiNumber = qBound(0, o["midiNumber"].toInt(), 127);
        m.key = o["key"].toString();
        p.controls.append(m);
    }

    const QJsonObject ar = root["audioReactive"].toObject();
    p.audioReactive.enabled = ar["enabled"].toBool(false);
    p.audioReactive.sensitivity = qBound(0.1, ar["sensitivity"].toDouble(1.0), 10.0);
    for (const QJsonValue &v : ar["links"].toArray()) {
        const QJsonObject o = v.toObject();
        AudioLink l;
        l.signal = AudioSignal(qBound(0, o["signal"].toInt(), kAudioSignalCount - 1));
        l.target = AudioTarget(qBound(0, o["target"].toInt(), kAudioTargetCount - 1));
        l.layer = qMax(-1, o["layer"].toInt());
        l.effect = qMax(0, o["effect"].toInt());
        l.param = qMax(0, o["param"].toInt());
        l.amount = qBound(0.0, o["amount"].toDouble(1.0), 1.0);
        p.audioReactive.links.append(l);
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
