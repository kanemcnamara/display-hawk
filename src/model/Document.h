#pragma once

#include "model/Project.h"

#include <QObject>

#include <functional>

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
    // Masks are edited on a target: a screen (index) or the composition (kCompositionMasks).
    QVector<ProjectionMask> *maskList(int target);
    // The mask whose points the map edits (target -1 = none).
    int activeMaskScreen() const { return m_maskScreen; }
    int activeMask() const { return m_mask; }
    void setActiveMask(int target, int mask);
    // Drawing a new mask point by point on the target's map (-1 = not drawing).
    int maskDrawTarget() const { return m_drawTarget; }
    void setMaskDrawTarget(int target);
    // Adds a finished mask to the target and makes it the one being edited. Returns its index.
    int addMask(int target, const ProjectionMask &mask);
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

    // Cabinet layouts: one generated slice per fitted cabinet of a wall (see CabinetMapping).
    // Adding a layout for a wall that already has one on that screen replaces it.
    int addCabinetLayout(int screen, const CabinetLayout &layout);
    void setCabinetLayout(int screen, int index, const CabinetLayout &layout);
    void removeCabinetLayout(int screen, int index);
    bool hasCabinetLayout(int wall) const;

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
    void triggerClip(int layer, int column); // follows the clip's trigger mode
    void releaseClip(int layer, int column); // piano clips stop when released
    void triggerColumn(int column);
    void clearLayer(int layer);
    void setMaster(double master);
    void setBlackout(bool on);
    void setLayerPlayback(int layer, double opacity, bool bypass, bool solo);
    void setLayerVolume(int layer, double volume);
    void setAudioMaster(double volume);
    void setAudioMuted(bool muted);
    void setBpm(double bpm); // playback state: no undo step
    void setCrossfader(double value); // playback state
    // Quantized triggering: returns how long until the next beat (beats = 1) or bar (4); set by
    // the tempo clock. Without one, quantizing is off.
    void setQuantizer(const std::function<int(int beats)> &msUntil) { m_quantizer = msUntil; }
    // Live control of an effect parameter (OSC/MIDI): no undo step per message. layer -1 = composition.
    void setEffectParam(int layer, int effect, int param, double value);

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
    // A template: loaded as a new, unsaved setup.
    bool loadTemplate(const QString &path, QString *error);
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
    static void removeCabinetSlices(::Screen &screen, int wall);
    static void copyPlayback(const Project &from, Project &to);

    Project m_project;
    Selection m_selection;
    std::function<int(int)> m_quantizer;
    void startClip(int layer, int column);
    void startColumn(int column);
    int m_maskScreen = -1;
    int m_mask = -1;
    int m_drawTarget = -1;
    QString m_filePath;
    bool m_modified = false;
    QJsonObject m_savedJson;

    Project m_committed;
    QVector<Project> m_undo;
    QVector<Project> m_redo;
    bool m_pendingCommit = false;
    QTimer *m_commitTimer;
};
