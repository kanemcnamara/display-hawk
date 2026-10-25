#pragma once

#include <QColor>
#include <QJsonObject>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QVector>

enum class ContentSource { TestPattern = 0, Media = 1 };
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
    ScaleMode scale = ScaleMode::Fit;
    bool loop = true;
    QColor solidColor = Qt::white;
    int gridSpacing = 64;

    bool isEmpty() const { return id.isEmpty(); }
    QString displayName() const;
    static Clip fromPattern(TestPattern p);
    static Clip fromMedia(const QString &path);
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
    bool blackout = false; // playback state

    QSize compSize() const { return QSize(compWidth, compHeight); }
    QRect targetRect(int wall) const;
    // Pads every layer to `columns` slots.
    void normalizeDeck();

    QJsonObject toJson() const;
    static Project fromJson(const QJsonObject &obj);
    static Project makeDefault();
};
