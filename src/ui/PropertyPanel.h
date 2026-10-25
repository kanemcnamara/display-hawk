#pragma once

#include "model/Document.h"
#include "model/Project.h"

#include <QWidget>

#include <functional>

class Document;
class CabinetLayoutEditor;
class MaskEditor;
class EffectChainEditor;
class PanelGridEditor;
class RenderEngine;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QSlider;
class QPlainTextEdit;
class QFontComboBox;
class QFormLayout;
class QStackedWidget;
class QTabBar;
class QVBoxLayout;

// Inspector for whatever is selected. Two of them: the main window's (Show: composition effects
// and groups, layer, clip, as tabs) and the output setup window's (Setup: composition size and
// overlay, LED wall, screen, slice). Each keeps the last selection in its scope, so picking a clip
// does not take the setup window off the slice being edited.
class PropertyPanel : public QWidget
{
    Q_OBJECT
public:
    enum class Scope { Show, Setup };
    PropertyPanel(Document *doc, RenderEngine *engine, Scope scope, QWidget *parent = nullptr);

signals:
    void identifyRequested();
    void midiLearnRequested(int action, int layer, int column, int param);

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

    // The selection this panel shows (the document's, when it is in this panel's scope).
    bool inScope(const Selection &s) const;
    bool isValid(const Selection &s) const;
    Layer *selLayer();
    Clip *selClip();
    LedWall *selWall();
    Screen *selScreen();
    Slice *selSlice();
    void showPage(int page);

    Document *m_doc;
    RenderEngine *m_engine;
    Scope m_scope;
    Selection m_sel;
    QTabBar *m_tabs = nullptr; // Show: Composition / Layer / Clip
    QVector<QWidget *> m_setupOnly, m_showOnly;
    QStackedWidget *m_stack;
    bool m_refreshing = false;

    // Composition
    QComboBox *m_compPreset;
    QSpinBox *m_compW, *m_compH;
    QPushButton *m_compBg;
    QCheckBox *m_overlay;
    QDoubleSpinBox *m_overlayOpacity;
    QCheckBox *m_maskToPanels;
    EffectChainEditor *m_compFx;

    // Layer
    QLineEdit *m_layerName;
    QDoubleSpinBox *m_layerOpacity, *m_layerVolume;
    QComboBox *m_layerBlend, *m_layerTarget;
    QSpinBox *m_layerTransition;
    QCheckBox *m_layerBypass, *m_layerSolo;
    EffectChainEditor *m_layerFx;

    // Clip
    QStackedWidget *m_clipStack;
    QLineEdit *m_clipName;
    QComboBox *m_source, *m_pattern, *m_scale, *m_input;
    QSpinBox *m_gridSpacing;
    QPushButton *m_solidColor;
    QLineEdit *m_mediaPath;
    QLabel *m_mediaStatus;
    QComboBox *m_playMode, *m_trigger;
    QDoubleSpinBox *m_speed, *m_inPoint, *m_outPoint;
    QSpinBox *m_beatSync;
    // Text clips
    QWidget *m_textBox;
    QPlainTextEdit *m_text;
    QFontComboBox *m_font;
    QSpinBox *m_fontSize;
    QCheckBox *m_bold;
    QPushButton *m_textColor, *m_outlineColor, *m_textBackground;
    QDoubleSpinBox *m_outlineWidth, *m_textSpeed;
    QComboBox *m_textAlign, *m_textMotion;
    // Generator clips
    QWidget *m_genBox;
    QComboBox *m_generator;
    QPushButton *m_genColor1, *m_genColor2;
    QFormLayout *m_genParams;
    QString m_builtGenerator;
    QVector<std::function<void(double)>> m_genSetters;
    void rebuildGeneratorParams(const Clip &clip);
    void fillGenerators(QComboBox *combo);
    QComboBox *m_newGenerator;
    QCheckBox *m_reverse;
    QSlider *m_position;
    QLabel *m_positionText;
    QDoubleSpinBox *m_clipX, *m_clipY, *m_clipScale, *m_clipRotation, *m_clipOpacity;
    EffectChainEditor *m_clipFx;
    QComboBox *m_transition, *m_autopilot, *m_layerSide, *m_layerGroup;
    QVBoxLayout *m_groupList;
    void rebuildGroups();
    int m_builtGroups = -1;
    QDoubleSpinBox *m_autopilotSeconds;
    void refreshPosition();
    QDoubleSpinBox *m_clipVolume;

    // Wall
    QLineEdit *m_wallName;
    QComboBox *m_wallPreset;
    QSpinBox *m_panelPxW, *m_panelPxH, *m_cols, *m_rows, *m_wallX, *m_wallY;
    QDoubleSpinBox *m_wallAngle;
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
    CabinetLayoutEditor *m_cabinets;
    MaskEditor *m_masks;
    MaskEditor *m_compMasks;

    // Slice
    QLineEdit *m_sliceName;
    QCheckBox *m_sliceEnabled;
    QSpinBox *m_inX, *m_inY, *m_inW, *m_inH, *m_outX, *m_outY, *m_outW, *m_outH;
    QComboBox *m_rotation;
    QCheckBox *m_flipH, *m_flipV;
    QDoubleSpinBox *m_brightness;
    QDoubleSpinBox *m_gainR, *m_gainG, *m_gainB, *m_gamma, *m_blackLevel;
    QComboBox *m_sliceWall;
    QDoubleSpinBox *m_angle;
    QComboBox *m_warpMode;
    QSpinBox *m_meshW, *m_meshH;
    QPushButton *m_meshReset;
    QDoubleSpinBox *m_blendL, *m_blendR, *m_blendT, *m_blendB, *m_blendCurve, *m_blendGamma;
    QLabel *m_cabinetInfo;
};
