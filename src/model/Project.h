#pragma once

#include <QColor>
#include <QJsonObject>
#include <QPoint>
#include <QPolygonF>
#include <QTransform>
#include <QRect>
#include <QString>
#include <QVector>

enum class ContentSource { TestPattern = 0, Media = 1, Capture = 2, Ndi = 3, Share = 4 }; // Share: Spout / Syphon
enum class TestPattern { Grid = 0, ColorBars, Gradient, Checker, PanelMap, Solid, MovingBars };
constexpr int kTestPatternCount = int(TestPattern::MovingBars) + 1;
enum class ScaleMode { Fit = 0, Fill, Stretch, Native };
enum class BlendMode { Normal = 0, Add, Screen, Multiply, Lighten, Darken };
// What a media clip does at its out point.
enum class PlayMode { Loop = 0, Bounce, OnceHold, OnceClear };
constexpr int kPlayModeCount = int(PlayMode::OnceClear) + 1;
// What triggering a clip does: restart it; stop it if it is already playing; play only while held.
enum class TriggerMode { Retrigger = 0, Toggle, Piano };
constexpr int kTriggerModeCount = int(TriggerMode::Piano) + 1;
// How a layer changes from one clip to the next (over its transition time).
enum class TransitionStyle { Crossfade = 0, WipeLeft, WipeRight, WipeUp, WipeDown, PushLeft, PushRight, PushUp, PushDown, Zoom };
constexpr int kTransitionStyleCount = int(TransitionStyle::Zoom) + 1;
// Autopilot: what a layer plays when its clip finishes (media at its out point, anything else
// after the layer's autopilot time).
enum class Autopilot { Off = 0, Next, Random };
QString playModeName(PlayMode m);
QString triggerModeName(TriggerMode m);
QString transitionStyleName(TransitionStyle t);
QString autopilotName(Autopilot a);
// Tempo: where the beat comes from, and whether clip triggers wait for the next beat or bar.
enum class TempoSource { Internal = 0, MidiClock, AudioBeats };
enum class TriggerQuantize { Off = 0, Beat, Bar };
QString tempoSourceName(TempoSource s);
QString triggerQuantizeName(TriggerQuantize q);

QString testPatternName(TestPattern p);
QString scaleModeName(ScaleMode m);
QString blendModeName(BlendMode m);
QString newId();

// A physical LED wall built from a grid of identical panels (cabinets).
struct LedWall {
    QString name = QStringLiteral("LED Wall");
    int panelPxW = 128;
    int panelPxH = 128;
    int cols = 8;
    int rows = 4;
    double panelMmW = 500.0;
    double panelMmH = 500.0;
    int x = 0; // top-left position in the composition (pixels)
    int y = 0;
    QColor color = QColor(0, 190, 255);
    // Rotation of the whole wall about its centre, clockwise, in degrees (diamond or tilted walls).
    // x, y and the panel rects describe the wall before rotation.
    double angle = 0.0;
    // Cabinets that are not fitted (stepped or irregular walls), as (column, row).
    QVector<QPoint> disabledPanels;

    bool panelEnabled(int col, int row) const { return !disabledPanels.contains(QPoint(col, row)); }
    QRect panelRect(int col, int row) const { return QRect(x + col * panelPxW, y + row * panelPxH, panelPxW, panelPxH); }
    int enabledPanelCount() const;
    int pixelWidth() const { return panelPxW * cols; }
    int pixelHeight() const { return panelPxH * rows; }
    QRect rect() const { return QRect(x, y, pixelWidth(), pixelHeight()); } // unrotated
    bool rotated() const { return angle != 0.0; }
    QTransform transform() const; // unrotated wall -> composition
    QPolygonF outline() const;
    QPolygonF panelPolygon(int col, int row) const;
    QPointF panelCentre(int col, int row) const;
    QRect bounds() const; // bounding box in the composition
    double pitchMm() const { return panelPxW > 0 ? panelMmW / panelPxW : 0.0; }
    double widthM() const { return panelMmW * cols / 1000.0; }
    double heightM() const { return panelMmH * rows / 1000.0; }
    // Pixels actually driven: enabled panels only.
    qint64 totalPixels() const { return qint64(enabledPanelCount()) * panelPxW * panelPxH; }
};

// Copies a rectangle of the composition onto a rectangle of a screen.
struct Slice {
    QString name = QStringLiteral("Slice");
    bool enabled = true;
    QRect input;  // composition pixels
    QRect output; // screen pixels
    // Corner pin: the output as a free quad (top-left, top-right, bottom-right, bottom-left, screen
    // pixels), mapped with perspective. Empty = the output rect.
    QPolygonF warp;
    // Mesh warp (projection on uneven surfaces): meshW x meshH control points, row by row from the
    // top-left, in screen pixels, interpolated smoothly (Catmull-Rom). Empty = no mesh. A mesh
    // takes precedence over the corner pin.
    int meshW = 0;
    int meshH = 0;
    QVector<QPointF> mesh;
    // Edge blending for overlapping projectors: soft-edge widths in output pixels on each side,
    // the blend curve's steepness (1 = linear) and the projector gamma it compensates.
    double blendLeft = 0.0, blendRight = 0.0, blendTop = 0.0, blendBottom = 0.0;
    double blendCurve = 2.0;
    double blendGamma = 2.2;
    int rotation = 0; // quarter turns clockwise, 0..3
    bool flipH = false;
    bool flipV = false;
    // Rotates the sampled area of the composition about its centre, clockwise, in degrees: for
    // walls hung at an angle (diamond layouts). The output stays an upright rectangle.
    double angle = 0.0;
    // Cabinet slices (generated by a cabinet layout): the wall and cabinet they show, and the
    // processor port they are chained on (1-based; 0 = none).
    int wall = -1;
    QPoint cabinet = QPoint(-1, -1);
    int port = 0;
    double brightness = 1.0;
    // LED colour correction, for matching cabinets or batches.
    double gainR = 1.0, gainG = 1.0, gainB = 1.0;
    double gamma = 1.0;      // exponent applied after the gains; > 1 darkens the mid-tones
    double blackLevel = 0.0; // lifts black, 0..0.5

    bool hasColorCorrection() const
    {
        return gainR != 1.0 || gainG != 1.0 || gainB != 1.0 || gamma != 1.0 || blackLevel != 0.0;
    }
    bool isCabinet() const { return wall >= 0 && cabinet.x() >= 0; }
    bool isWarped() const { return warp.size() == 4; }
    bool isMeshed() const { return meshW >= 2 && meshH >= 2 && mesh.size() == meshW * meshH; }
    bool hasBlend() const { return blendLeft > 0 || blendRight > 0 || blendTop > 0 || blendBottom > 0; }
    // Where the point (s, t) of the upright output rect (0..1 from its top-left) lands on the screen:
    // through the mesh, the corner pin (with perspective) or the output rect.
    QPointF outputPoint(double s, double t) const;
    // A flat mesh of w x h points over the current output shape.
    void resetMesh(int w, int h);
    QPointF outputPointFlat(double s, double t) const; // ignoring the mesh
    // Re-samples the mesh to w x h points, keeping its current shape.
    void resizeMesh(int w, int h);
    // Where the slice lands on the screen: the corner pin, or the output rect's corners.
    QPolygonF outputQuad() const;
    // The sampled area: the input rect turned by `angle`, corners clockwise from the top-left.
    QPolygonF inputPolygon() const;
};

// A projection mask on a screen: a polygon (screen pixels) whose inside is blacked out, or with
// `invert` everything outside it, with a soft edge `feather` pixels wide.
struct ProjectionMask {
    QString name = QStringLiteral("Mask");
    bool enabled = true;
    bool invert = false;
    double feather = 20.0;
    QPolygonF points;
};

// How a wall's cabinets are laid out on a processor's output canvas, one slice per fitted cabinet.
enum class CabinetArrange { WallShape = 0, PackRows, PackColumns };
// The order cabinets are chained in (processor ports run through them in this order).
enum class CabinetOrder { Rows = 0, RowsSerpentine, Columns, ColumnsSerpentine };
QString cabinetArrangeName(CabinetArrange a);
QString cabinetOrderName(CabinetOrder o);

struct CabinetLayout {
    int wall = 0;
    CabinetArrange arrange = CabinetArrange::WallShape;
    CabinetOrder order = CabinetOrder::RowsSerpentine;
    int startCorner = 0; // where the chain starts on the wall: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
    int x = 0;           // top-left of the layout on the screen
    int y = 0;
    int gap = 0;         // pixels between cabinets on the output
    int rotation = 0;    // quarter turns clockwise, for each cabinet and the layout as a whole
    bool flipH = false;
    bool flipV = false;
    int portCapacity = 655360; // pixels per processor port; 0 = everything on one port
    int firstPort = 1;
    bool newLinePerPort = false; // packed layouts: each port starts a new row (column)
};

// A physical video output (graphics card output feeding an LED processor or projector).
struct Screen {
    QString name = QStringLiteral("Screen");
    int width = 1920;
    int height = 1080;
    QString display; // QScreen::name(), "window" for a windowed output, empty for none
    bool enabled = true;
    QVector<Slice> slices;
    QVector<CabinetLayout> cabinetLayouts; // regenerated into cabinet slices when walls change
    QVector<ProjectionMask> masks;         // drawn over the output, after every slice
};

// One effect in a chain (see render/Effects.h for the catalogue): type id, on/off and its parameter
// values in the order the effect defines them.
struct EffectInstance {
    QString type;
    bool enabled = true;
    QVector<double> params;
};

// One cell of the clip deck. An empty id means an empty slot.
struct Clip {
    QString id;
    QString name;
    ContentSource source = ContentSource::TestPattern;
    TestPattern pattern = TestPattern::Grid;
    QString mediaPath;
    QString inputId; // Capture: camera/capture device id; Ndi: NDI source name; Share: Spout sender / Syphon server
    ScaleMode scale = ScaleMode::Fit;
    // Media playback.
    PlayMode playMode = PlayMode::Loop;
    double speed = 1.0;      // 0.05 .. 8
    bool reverse = false;    // HAP movies only
    double inPoint = 0.0;    // 0..1 of the movie
    double outPoint = 1.0;
    TriggerMode trigger = TriggerMode::Retrigger;
    // Transform within the layer's target area: offset in pixels, scale, clockwise rotation, opacity.
    double posX = 0.0, posY = 0.0;
    double scaleBy = 1.0;
    double rotation = 0.0;
    double opacity = 1.0;
    QVector<EffectInstance> effects; // applied to the clip before the layer mixes it in
    int beatSync = 0; // > 0: the movie (in to out) is stretched to last this many beats
    double volume = 1.0; // the clip's own audio level
    QColor solidColor = Qt::white;
    int gridSpacing = 64;

    bool isEmpty() const { return id.isEmpty(); }
    QString displayName() const;
    static Clip fromPattern(TestPattern p);
    static Clip fromMedia(const QString &path);
    static Clip fromCapture(const QString &deviceId, const QString &description);
    static Clip fromNdi(const QString &sourceName);
    static Clip fromShare(const QString &sourceName);
    bool isLive() const
    {
        return source == ContentSource::Capture || source == ContentSource::Ndi || source == ContentSource::Share;
    }
    // What a media decoder must open for this clip: file path or live input. Empty for test patterns
    // and Spout/Syphon, which the render thread receives itself.
    QString mediaKey() const;
};


// A row of the clip deck. Layers are composited bottom (index 0) to top.
struct Layer {
    QString id;
    QString name = QStringLiteral("Layer");
    double opacity = 1.0;
    double volume = 1.0; // playback state, like opacity
    BlendMode blend = BlendMode::Normal;
    bool bypass = false;
    bool solo = false;
    int targetWall = -1;   // clips are fitted into this wall; -1 = whole composition
    int transitionMs = 0;  // crossfade when the playing clip changes
    TransitionStyle transition = TransitionStyle::Crossfade;
    Autopilot autopilot = Autopilot::Off;
    double autopilotSeconds = 10.0; // clips without an end (stills, patterns, live inputs)
    int activeColumn = -1; // playback state: which clip is playing, -1 = none
    QVector<Clip> clips;   // one slot per column
    QVector<EffectInstance> effects; // applied to the layer before it is blended

    const Clip *activeClip() const;
};

// Something a controller can do. Layer and column are 0-based indices (layer 0 = bottom).
enum class ControlAction {
    Master = 0,     // continuous
    Blackout,       // toggle
    OutputsLive,    // toggle
    TriggerColumn,  // trigger (column)
    TriggerClip,    // trigger (layer, column)
    LayerOpacity,   // continuous (layer)
    LayerBypass,    // toggle (layer)
    LayerSolo,      // toggle (layer)
    ClearLayer,     // trigger (layer)
    LayerVolume,    // continuous (layer)
    AudioMaster,    // continuous
    AudioMute,      // toggle
    EffectParam,    // continuous (layer, column = effect index, param); layer -1 = composition
    TapTempo,       // trigger
    TempoResync,    // trigger: now is the first beat of a bar
};
constexpr int kControlActionCount = int(ControlAction::TempoResync) + 1;
QString controlActionName(ControlAction a);
bool controlActionUsesLayer(ControlAction a);
bool controlActionUsesColumn(ControlAction a);

// A MIDI control (note or CC on a channel) mapped to an action. Learned from the controller.
struct ControlMapping {
    ControlAction action = ControlAction::TriggerColumn;
    int layer = 0;
    int column = 0;
    int param = 0;         // EffectParam
    bool midiSet = false;
    bool midiIsCC = false; // false = note
    int midiChannel = 0;   // 0..15
    int midiNumber = 0;    // note or controller number

    QString targetText() const;
    QString midiText() const;
};

enum class DmxProtocol { ArtNet = 0, Sacn = 1 };

// Where Display Hawk sits in a lighting desk's DMX patch (Art-Net or sACN). Channels from
// `address`: 1 master, 2 blackout, 3 column trigger, then per layer (bottom first): clip, opacity, bypass.
struct DmxPatch {
    bool enabled = false;
    DmxProtocol protocol = DmxProtocol::ArtNet;
    int universe = 0; // Art-Net Port-Address 0..32767, or sACN universe 1..63999
    int address = 1;  // first channel, 1..512
    int layers = 4;   // how many layers the desk controls

    int channelCount() const { return 3 + 3 * layers; }
};

// Audio-reactive links: an analysed audio input signal driving something in the show.
enum class AudioSignal { Level = 0, Bass, Mid, Treble, Beat };
enum class AudioTarget {
    LayerOpacity = 0, // modulates the layer's opacity (multiplier, the fader stays put)
    Master,           // modulates the master
    NextColumn,       // on each beat: trigger the next column that has clips
    NextClip,         // on each beat: the layer plays its next clip
    EffectParam,      // pushes an effect parameter from its set value towards its maximum
};
constexpr int kAudioSignalCount = int(AudioSignal::Beat) + 1;
constexpr int kAudioTargetCount = int(AudioTarget::EffectParam) + 1;
QString audioSignalName(AudioSignal s);
QString audioTargetName(AudioTarget t);
inline bool audioTargetIsBeat(AudioTarget t) { return t == AudioTarget::NextColumn || t == AudioTarget::NextClip; }
inline bool audioTargetUsesLayer(AudioTarget t)
{
    return t == AudioTarget::LayerOpacity || t == AudioTarget::NextClip || t == AudioTarget::EffectParam;
}

struct AudioLink {
    AudioSignal signal = AudioSignal::Bass;
    AudioTarget target = AudioTarget::LayerOpacity;
    int layer = 0;       // 0-based; -1 = composition (effect parameters)
    int effect = 0;      // EffectParam: index in the chain
    int param = 0;       // EffectParam: parameter index
    double amount = 1.0; // 0..1: how deep the modulation goes (ignored for beat targets)
};

// A tempo-synced low-frequency oscillator driving something, like an audio link but from a wave.
enum class LfoShape { Sine = 0, Triangle, SawUp, SawDown, Square, Random };
constexpr int kLfoShapeCount = int(LfoShape::Random) + 1;
QString lfoShapeName(LfoShape s);
struct Lfo {
    bool enabled = true;
    AudioTarget target = AudioTarget::LayerOpacity; // LayerOpacity, Master or EffectParam
    int layer = 0;   // -1 = composition (effect parameters)
    int effect = 0;
    int param = 0;
    LfoShape shape = LfoShape::Sine;
    double beats = 4.0;  // one cycle lasts this many beats
    double amount = 1.0; // 0..1
    double phase = 0.0;  // 0..1 of a cycle
};

struct AudioReactiveSettings {
    bool enabled = false;
    double sensitivity = 1.0; // input gain before analysis
    QVector<AudioLink> links;
};

struct Project {
    int compWidth = 1920;
    int compHeight = 1080;
    QColor background = Qt::black;
    bool panelOverlay = false;
    bool maskToPanels = false; // black out everything that is not an enabled LED panel
    double overlayOpacity = 0.6;
    QVector<LedWall> walls;
    QVector<Screen> screens;

    int columns = 8;
    QVector<Layer> layers;
    double master = 1.0; // playback state
    QVector<ControlMapping> controls; // MIDI mappings for this show
    QVector<EffectInstance> compEffects; // applied to the whole composition, before the master
    DmxPatch dmx;
    AudioReactiveSettings audioReactive;
    QVector<Lfo> lfos;
    double bpm = 120.0; // playback state, like the master: tapping does not make undo steps
    TempoSource tempoSource = TempoSource::Internal;
    TriggerQuantize quantize = TriggerQuantize::Off;
    bool blackout = false; // playback state
    double audioMaster = 1.0; // playback state
    bool audioMuted = false;  // playback state

    QSize compSize() const { return QSize(compWidth, compHeight); }
    QRect targetRect(int wall) const;
    // Pads every layer to `columns` slots.
    void normalizeDeck();

    QJsonObject toJson() const;
    static Project fromJson(const QJsonObject &obj);
    static Project makeDefault();
};
