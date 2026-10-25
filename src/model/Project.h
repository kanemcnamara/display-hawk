#pragma once

#include <QColor>
#include <QJsonObject>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QVector>

enum class ContentSource { TestPattern = 0, Media = 1, Capture = 2, Ndi = 3 };
enum class TestPattern { Grid = 0, ColorBars, Gradient, Checker, PanelMap, Solid, MovingBars };
constexpr int kTestPatternCount = int(TestPattern::MovingBars) + 1;
enum class ScaleMode { Fit = 0, Fill, Stretch, Native };
enum class BlendMode { Normal = 0, Add, Screen, Multiply, Lighten, Darken };

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
    // Cabinets that are not fitted (stepped or irregular walls), as (column, row).
    QVector<QPoint> disabledPanels;

    bool panelEnabled(int col, int row) const { return !disabledPanels.contains(QPoint(col, row)); }
    QRect panelRect(int col, int row) const { return QRect(x + col * panelPxW, y + row * panelPxH, panelPxW, panelPxH); }
    int enabledPanelCount() const;
    int pixelWidth() const { return panelPxW * cols; }
    int pixelHeight() const { return panelPxH * rows; }
    QRect rect() const { return QRect(x, y, pixelWidth(), pixelHeight()); }
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
    int rotation = 0; // quarter turns clockwise, 0..3
    bool flipH = false;
    bool flipV = false;
    double brightness = 1.0;
    // LED colour correction, for matching cabinets or batches.
    double gainR = 1.0, gainG = 1.0, gainB = 1.0;
    double gamma = 1.0;      // exponent applied after the gains; > 1 darkens the mid-tones
    double blackLevel = 0.0; // lifts black, 0..0.5

    bool hasColorCorrection() const
    {
        return gainR != 1.0 || gainG != 1.0 || gainB != 1.0 || gamma != 1.0 || blackLevel != 0.0;
    }
};

// A physical video output (graphics card output feeding an LED processor or projector).
struct Screen {
    QString name = QStringLiteral("Screen");
    int width = 1920;
    int height = 1080;
    QString display; // QScreen::name(), "window" for a windowed output, empty for none
    bool enabled = true;
    QVector<Slice> slices;
};

// One cell of the clip deck. An empty id means an empty slot.
struct Clip {
    QString id;
    QString name;
    ContentSource source = ContentSource::TestPattern;
    TestPattern pattern = TestPattern::Grid;
    QString mediaPath;
    QString inputId; // Capture: camera/capture device id; Ndi: NDI source name
    ScaleMode scale = ScaleMode::Fit;
    bool loop = true;
    QColor solidColor = Qt::white;
    int gridSpacing = 64;

    bool isEmpty() const { return id.isEmpty(); }
    QString displayName() const;
    static Clip fromPattern(TestPattern p);
    static Clip fromMedia(const QString &path);
    static Clip fromCapture(const QString &deviceId, const QString &description);
    static Clip fromNdi(const QString &sourceName);
    bool isLive() const { return source == ContentSource::Capture || source == ContentSource::Ndi; }
    // What a media decoder must open for this clip ("" for test patterns): file path or live input.
    QString mediaKey() const;
};

// A row of the clip deck. Layers are composited bottom (index 0) to top.
struct Layer {
    QString id;
    QString name = QStringLiteral("Layer");
    double opacity = 1.0;
    BlendMode blend = BlendMode::Normal;
    bool bypass = false;
    bool solo = false;
    int targetWall = -1;   // clips are fitted into this wall; -1 = whole composition
    int transitionMs = 0;  // crossfade when the playing clip changes
    int activeColumn = -1; // playback state: which clip is playing, -1 = none
    QVector<Clip> clips;   // one slot per column

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
};
constexpr int kControlActionCount = int(ControlAction::ClearLayer) + 1;
QString controlActionName(ControlAction a);
bool controlActionUsesLayer(ControlAction a);
bool controlActionUsesColumn(ControlAction a);

// A MIDI control (note or CC on a channel) mapped to an action. Learned from the controller.
struct ControlMapping {
    ControlAction action = ControlAction::TriggerColumn;
    int layer = 0;
    int column = 0;
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
    DmxPatch dmx;
    bool blackout = false; // playback state

    QSize compSize() const { return QSize(compWidth, compHeight); }
    QRect targetRect(int wall) const;
    // Pads every layer to `columns` slots.
    void normalizeDeck();

    QJsonObject toJson() const;
    static Project fromJson(const QJsonObject &obj);
    static Project makeDefault();
};
