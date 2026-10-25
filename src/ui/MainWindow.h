#pragma once

#include <QMainWindow>
#include <QPointer>

class CanvasView;
class AudioReactive;
class LfoDialog;
class ClipPreviewPanel;
class OutputRecorder;
class CueList;
class TimecodeInput;
class WebRemote;
class PixelMapper;
class QDialog;
class ModulationHub;
class TempoClock;
class AudioReactiveDialog;
class ControlDialog;
class ControlRouter;
class Document;
class OutputWindow;
class PropertyPanel;
class QAction;
class QComboBox;
class QLabel;
class QTimer;
class QTreeWidget;
class RenderEngine;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow(Document *doc, RenderEngine *engine, QWidget *parent = nullptr);
    ~MainWindow() override;

    // Crash recovery, reopening the last setup and go-live-on-launch. Call once after show().
    void startup(const QStringList &args);
    void setLive(bool live);
    ControlRouter *controlRouter() const { return m_router; }
    AudioReactive *audioReactive() const { return m_audioReactive; }
    TempoClock *tempoClock() const { return m_tempo; }
    ModulationHub *modulationHub() const { return m_modulation; }
    ClipPreviewPanel *clipPreview() const { return m_clipPreview; }
    OutputRecorder *recorder() const { return m_recorder; }
    CueList *cueList() const { return m_cueList; }
    TimecodeInput *timecodeInput() const { return m_timecode; }
    WebRemote *webRemote() const { return m_webRemote; }
    PixelMapper *pixelMapper() const { return m_pixelMapper; }
    void showCueList();
    void showControls();
    void showAudioReactive();
    void showLfos();

protected:
    void closeEvent(QCloseEvent *e) override;

private:
    void buildActions();
    void buildTree();
    void rebuildTree();
    void syncTreeSelection();
    void onTreeSelection();
    void refreshOutputScreenCombo();
    void syncOutputs();
    void closeOutputs();
    void identifyDisplays();
    void autosave();
    void updateTitle();
    bool confirmDiscard();
    bool saveTo(const QString &path);
    void openProject();
    bool openPath(const QString &path);
    bool saveProject();
    bool saveProjectAs();
    void exportReport();
    void rememberProject(const QString &path);
    static QString autosavePath();

    Document *m_doc;
    RenderEngine *m_engine;
    CanvasView *m_inputView;
    CanvasView *m_outputView;
    QComboBox *m_outputScreen;
    PropertyPanel *m_props;
    QTreeWidget *m_tree;
    ControlRouter *m_router;
    ControlDialog *m_controlDialog = nullptr;
    AudioReactive *m_audioReactive;
    TempoClock *m_tempo;
    ClipPreviewPanel *m_clipPreview;
    OutputRecorder *m_recorder;
    TimecodeInput *m_timecode;
    CueList *m_cueList;
    WebRemote *m_webRemote;
    PixelMapper *m_pixelMapper;
    QDialog *m_pixelDialog = nullptr;
    QDialog *m_cueDialog = nullptr;
    QAction *m_recordAction;
    ModulationHub *m_modulation;
    AudioReactiveDialog *m_audioReactiveDialog = nullptr;
    LfoDialog *m_lfoDialog = nullptr;
    QTimer *m_autosaveTimer;
    QLabel *m_cursorLabel;
    QLabel *m_fpsLabel;
    QAction *m_liveAction;
    QAction *m_undoAction;
    QAction *m_redoAction;
    QAction *m_liveOnLaunchAction;
    QAction *m_reopenLastAction;
    QVector<QPointer<OutputWindow>> m_outputs;
    QVector<QPair<int, QString>> m_ndiOutputs;
    QVector<QPair<int, QString>> m_shareOutputs;
    bool m_live = false;
    bool m_syncingTree = false;
    bool m_autosaveDirty = false;
};
