#pragma once

#include <QColor>
#include <QJsonObject>
#include <QRect>
#include <QString>
#include <QVector>

enum class ContentSource { TestPattern = 0, Media = 1 };
enum class TestPattern { Grid = 0, ColorBars, Gradient, Checker, PanelMap, Solid };
enum class ScaleMode { Fit = 0, Fill, Stretch, Native };

QString testPatternName(TestPattern p);
QString scaleModeName(ScaleMode m);

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

    int pixelWidth() const { return panelPxW * cols; }
    int pixelHeight() const { return panelPxH * rows; }
    QRect rect() const { return QRect(x, y, pixelWidth(), pixelHeight()); }
    double pitchMm() const { return panelPxW > 0 ? panelMmW / panelPxW : 0.0; }
    double widthM() const { return panelMmW * cols / 1000.0; }
    double heightM() const { return panelMmH * rows / 1000.0; }
    qint64 totalPixels() const { return qint64(pixelWidth()) * pixelHeight(); }
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

struct Content {
    ContentSource source = ContentSource::TestPattern;
    TestPattern pattern = TestPattern::PanelMap;
    QString mediaPath;
    ScaleMode scale = ScaleMode::Fit;
    int targetWall = -1; // -1 = whole composition
    bool loop = true;
    double opacity = 1.0;
    QColor solidColor = Qt::white;
    int gridSpacing = 64;
};

struct Project {
    int compWidth = 1920;
    int compHeight = 1080;
    QColor background = Qt::black;
    Content content;
    bool panelOverlay = false;
    double overlayOpacity = 0.6;
    QVector<LedWall> walls;
    QVector<Screen> screens;

    QSize compSize() const { return QSize(compWidth, compHeight); }
    QRect contentTarget() const;

    QJsonObject toJson() const;
    static Project fromJson(const QJsonObject &obj);
    static Project makeDefault();
};
