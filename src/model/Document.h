#pragma once

#include "model/Project.h"

#include <QObject>

class QTimer;

struct Selection {
    enum Kind { None, Composition, Wall, Screen, Slice, Layer, Clip };
    Kind kind = Composition;
    int wall = -1;
    int screen = -1;
    int slice = -1;
    int layer = -1;
    int column = -1;

    static Selection composition() { return {}; }
    static Selection ofWall(int w) { Selection s; s.kind = Wall; s.wall = w; return s; }
    static Selection ofScreen(int sc) { Selection s; s.kind = Screen; s.screen = sc; return s; }
    static Selection ofSlice(int sc, int sl) { Selection s; s.kind = Slice; s.screen = sc; s.slice = sl; return s; }
    static Selection ofLayer(int l) { Selection s; s.kind = Layer; s.layer = l; return s; }
    // A clip slot (possibly empty).
    static Selection ofClip(int l, int c) { Selection s; s.kind = Clip; s.layer = l; s.column = c; return s; }

    bool operator==(const Selection &o) const
    {
        return kind == o.kind && wall == o.wall && screen == o.screen && slice == o.slice && layer == o.layer
               && column == o.column;
    }
    bool operator!=(const Selection &o) const { return !(*this == o); }
};

// Owns the project, the current selection and the file state; every edit goes through here.
class Document : public QObject
{
    Q_OBJECT
public:
    explicit Document(QObject *parent = nullptr);

    Project &project() { return m_project; }
    const Project &project() const { return m_project; }

    // Call after mutating project(). structural = items added/removed/renamed.
    void notifyChanged(bool structural = false);
    void notifyMediaChanged();

    Selection selection() const { return m_selection; }
    void select(const Selection &sel);

    LedWall *selectedWall();
    Screen *selectedScreen();
    Slice *selectedSlice();
    ::Layer *selectedLayer(); // the layer of a selected layer or clip slot
    ::Clip *selectedClip();   // nullptr unless a non-empty clip slot is selected

    // Replaces a wall; slices that sampled exactly the old wall area follow it (and keep 1:1 output size).
    void applyWall(int index, const LedWall &wall);

    int addWall();
    void removeWall(int index);
    int addScreen();
    void removeScreen(int index);
    int addSlice(int screen);
    void removeSlice(int screen, int slice);
    void duplicateSelection();
    void deleteSelection();

    // Adds a 1:1 slice that maps the wall's composition area onto the next free spot of a screen.
    int createSliceForWall(int wall, int screen);
    // Resizes the composition to the bounding box of all walls and moves the walls to start at 0,0.
    void fitCompositionToWalls();

    // Clip deck editing (undoable).
    int addLayer();
    void removeLayer(int index);
    void addColumn();
    void setClip(int layer, int column, const ::Clip &clip);
    void moveClip(int fromLayer, int fromColumn, int toLayer, int toColumn); // swaps the two slots

    // Playback: changes what is playing without creating undo steps or marking the file modified.
    void triggerClip(int layer, int column);
    void triggerColumn(int column);
    void clearLayer(int layer);
    void setMaster(double master);
    void setBlackout(bool on);
    void setLayerPlayback(int layer, double opacity, bool bypass, bool solo);
    void setLayerVolume(int layer, double volume);
    void setAudioMaster(double volume);
    void setAudioMuted(bool muted);

    // Undo works on whole-project snapshots. Rapid edits (drags, typing) are grouped: a step is
    // committed when commitUndo() is called (e.g. on mouse release) or after a short pause.
    bool canUndo() const { return !m_undo.isEmpty() || m_pendingCommit; }
    bool canRedo() const { return !m_redo.isEmpty(); }
    void undo();
    void redo();
    void commitUndo();

    void newProject();
    // recovery = the file is an autosave: reopen it under its original path, marked modified.
    bool load(const QString &path, QString *error, bool recovery = false);
    bool save(const QString &path, QString *error);
    // Writes a crash-recovery copy without touching the document's path or modified state.
    bool writeAutosave(const QString &path) const;
    QString filePath() const { return m_filePath; }
    bool isModified() const { return m_modified; }

signals:
    void changed();
    void structureChanged();
    void selectionChanged();
    void mediaChanged();
    void fileStateChanged();
    void undoStateChanged();
    // A clip was (re)triggered; media clips restart from the beginning.
    void clipTriggered(const QString &clipId);

private:
    void setModified(bool m);
    void validateSelection();
    void resetHistory();
    void restoreSnapshot(const Project &snapshot);
    void notifyPlaybackChanged();
    static void copyPlayback(const Project &from, Project &to);

    Project m_project;
    Selection m_selection;
    QString m_filePath;
    bool m_modified = false;
    QJsonObject m_savedJson;

    Project m_committed;
    QVector<Project> m_undo;
    QVector<Project> m_redo;
    bool m_pendingCommit = false;
    QTimer *m_commitTimer;
};
