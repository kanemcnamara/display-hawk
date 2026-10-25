#pragma once

#include <QMainWindow>

class CanvasView;
class Document;
class PropertyPanel;
class QComboBox;
class QLabel;
class QToolBar;
class QTreeWidget;
class RenderEngine;

// Output setup ("advanced output" in Resolume): LED walls, screens and slices, the input and output
// maps and their properties, in a window of its own, so the main window is left for running the
// show. Set it up once, close it, and reopen it from Output > Output Setup (Ctrl/Cmd+Shift+O).
class OutputSetupWindow : public QMainWindow
{
    Q_OBJECT
public:
    OutputSetupWindow(Document *doc, RenderEngine *engine, QWidget *parent = nullptr);

    // The main window's actions that should work here too (undo, save, live, add/delete...).
    void addSharedActions(const QList<QAction *> &actions, const QList<QAction *> &toolbarActions);
    CanvasView *inputView() const { return m_inputView; }
    CanvasView *outputView() const { return m_outputView; }
    PropertyPanel *properties() const { return m_props; }
    int currentScreen() const;

signals:
    void identifyRequested();
    void midiLearnRequested(int action, int layer, int column, int param);
    void visibilityChanged(bool visible);

protected:
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private:
    void buildTree();
    void rebuildTree();
    void syncTreeSelection();
    void onTreeSelection();
    void refreshOutputScreenCombo();

    Document *m_doc;
    RenderEngine *m_engine;
    CanvasView *m_inputView;
    CanvasView *m_outputView;
    QComboBox *m_outputScreen;
    PropertyPanel *m_props;
    QTreeWidget *m_tree;
    QToolBar *m_toolbar;
    QLabel *m_cursorLabel;
    bool m_syncingTree = false;
    bool m_restored = false;
};
