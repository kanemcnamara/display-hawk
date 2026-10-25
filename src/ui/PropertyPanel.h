#pragma once

#include "model/Project.h"

#include <QWidget>

#include <functional>

class Document;
class MediaSource;
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
    PropertyPanel(Document *doc, MediaSource *media, QWidget *parent = nullptr);

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
    void refreshComposition();
    void refreshWall();
    void refreshScreen();
    void refreshSlice();
    void editWall(const std::function<void(LedWall &)> &fn);
    void editSlice(const std::function<void(Slice &)> &fn);
    void editScreen(const std::function<void(Screen &)> &fn, bool structural = false);
    void browseMedia();

    Document *m_doc;
    MediaSource *m_media;
    QStackedWidget *m_stack;
    bool m_refreshing = false;

    // Composition / content
    QComboBox *m_compPreset;
    QSpinBox *m_compW, *m_compH;
    QPushButton *m_compBg;
    QComboBox *m_source, *m_pattern, *m_scale, *m_target;
    QSpinBox *m_gridSpacing;
    QPushButton *m_solidColor;
    QLineEdit *m_mediaPath;
    QLabel *m_mediaStatus;
    QCheckBox *m_loop;
    QDoubleSpinBox *m_opacity;
    QCheckBox *m_overlay;
    QDoubleSpinBox *m_overlayOpacity;

    // Wall
    QLineEdit *m_wallName;
    QComboBox *m_wallPreset;
    QSpinBox *m_panelPxW, *m_panelPxH, *m_cols, *m_rows, *m_wallX, *m_wallY;
    QDoubleSpinBox *m_panelMmW, *m_panelMmH;
    QPushButton *m_wallColor;
    QLabel *m_wallInfo;
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
    QComboBox *m_sliceWall;
};
