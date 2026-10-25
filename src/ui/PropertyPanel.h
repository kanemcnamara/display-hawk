#pragma once

#include "model/Project.h"

#include <QWidget>

#include <functional>

class Document;
class PanelGridEditor;
class RenderEngine;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;

// Inspector for whatever is selected: composition/content, LED wall, screen or slice.
class PropertyPanel : public QWidget
{
    Q_OBJECT
public:
    PropertyPanel(Document *doc, RenderEngine *engine, QWidget *parent = nullptr);

signals:
    void identifyRequested();

public slots:
    void refresh();
    void refreshDisplays();

private:
    QWidget *buildCompositionPage();
    QWidget *buildWallPage();
    QWidget *buildScreenPage();
    QWidget *buildSlicePage();
    QWidget *buildLayerPage();
    QWidget *buildClipPage();
    void refreshComposition();
    void refreshWall();
    void refreshScreen();
    void refreshSlice();
    void refreshLayer();
    void refreshClip();
    void editWall(const std::function<void(LedWall &)> &fn);
    void editSlice(const std::function<void(Slice &)> &fn);
    void editScreen(const std::function<void(Screen &)> &fn, bool structural = false);
    void editLayer(const std::function<void(Layer &)> &fn, bool structural = false);
    void editClip(const std::function<void(Clip &)> &fn);
    void browseMedia();

    Document *m_doc;
    RenderEngine *m_engine;
    QStackedWidget *m_stack;
    bool m_refreshing = false;

    // Composition
    QComboBox *m_compPreset;
    QSpinBox *m_compW, *m_compH;
    QPushButton *m_compBg;
    QCheckBox *m_overlay;
    QDoubleSpinBox *m_overlayOpacity;
    QCheckBox *m_maskToPanels;

    // Layer
    QLineEdit *m_layerName;
    QDoubleSpinBox *m_layerOpacity;
    QComboBox *m_layerBlend, *m_layerTarget;
    QSpinBox *m_layerTransition;
    QCheckBox *m_layerBypass, *m_layerSolo;

    // Clip
    QStackedWidget *m_clipStack;
    QLineEdit *m_clipName;
    QComboBox *m_source, *m_pattern, *m_scale, *m_input;
    QSpinBox *m_gridSpacing;
    QPushButton *m_solidColor;
    QLineEdit *m_mediaPath;
    QLabel *m_mediaStatus;
    QCheckBox *m_loop;

    // Wall
    QLineEdit *m_wallName;
    QComboBox *m_wallPreset;
    QSpinBox *m_panelPxW, *m_panelPxH, *m_cols, *m_rows, *m_wallX, *m_wallY;
    QDoubleSpinBox *m_panelMmW, *m_panelMmH;
    QPushButton *m_wallColor;
    QLabel *m_wallInfo;
    PanelGridEditor *m_panelGrid;
    QLabel *m_panelCount;
    QComboBox *m_wallScreen;

    // Screen
    QLineEdit *m_screenName;
    QCheckBox *m_screenEnabled;
    QComboBox *m_screenPreset;
    QSpinBox *m_screenW, *m_screenH;
    QComboBox *m_display;
    QLabel *m_screenInfo;

    // Slice
    QLineEdit *m_sliceName;
    QCheckBox *m_sliceEnabled;
    QSpinBox *m_inX, *m_inY, *m_inW, *m_inH, *m_outX, *m_outY, *m_outW, *m_outH;
    QComboBox *m_rotation;
    QCheckBox *m_flipH, *m_flipV;
    QDoubleSpinBox *m_brightness;
    QDoubleSpinBox *m_gainR, *m_gainG, *m_gainB, *m_gamma, *m_blackLevel;
    QComboBox *m_sliceWall;
};
