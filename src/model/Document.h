#pragma once

#include "model/Project.h"

#include <QObject>

class QTimer;

struct Selection {
    enum Kind { None, Composition, Wall, Screen, Slice };
    Kind kind = Composition;
    int wall = -1;
    int screen = -1;
    int slice = -1;

    static Selection composition() { return {}; }
    static Selection ofWall(int w) { Selection s; s.kind = Wall; s.wall = w; return s; }
    static Selection ofScreen(int sc) { Selection s; s.kind = Screen; s.screen = sc; return s; }
    static Selection ofSlice(int sc, int sl) { Selection s; s.kind = Slice; s.screen = sc; s.slice = sl; return s; }

    bool operator==(const Selection &o) const
    {
        return kind == o.kind && wall == o.wall && screen == o.screen && slice == o.slice;
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

private:
    void setModified(bool m);
    void validateSelection();
    void resetHistory();
    void restoreSnapshot(const Project &snapshot);

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
