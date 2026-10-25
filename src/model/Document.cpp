#include "model/Document.h"

#include "model/CabinetMapping.h"
#include "render/Effects.h"

#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTimer>

namespace {
constexpr int kUndoLimit = 200;
constexpr int kCommitDelayMs = 600;
} // namespace

Document::Document(QObject *parent)
    : QObject(parent)
    , m_project(Project::makeDefault())
    , m_commitTimer(new QTimer(this))
{
    m_commitTimer->setSingleShot(true);
    m_commitTimer->setInterval(kCommitDelayMs);
    connect(m_commitTimer, &QTimer::timeout, this, &Document::commitUndo);
    resetHistory();
}

void Document::notifyChanged(bool structural)
{
    validateSelection();
    setModified(true);
    const bool wasPending = m_pendingCommit;
    m_pendingCommit = true;
    m_commitTimer->start();
    if (!wasPending)
        emit undoStateChanged();
    if (structural)
        emit structureChanged();
    emit changed();
}

void Document::commitUndo()
{
    m_commitTimer->stop();
    if (!m_pendingCommit)
        return;
    m_pendingCommit = false;
    if (m_project.toJson() != m_committed.toJson()) {
        m_undo.append(m_committed);
        if (m_undo.size() > kUndoLimit)
            m_undo.removeFirst();
        m_redo.clear();
        m_committed = m_project;
    }
    emit undoStateChanged();
}

void Document::undo()
{
    commitUndo();
    if (m_undo.isEmpty())
        return;
    m_redo.append(m_project);
    restoreSnapshot(m_undo.takeLast());
}

void Document::redo()
{
    commitUndo();
    if (m_redo.isEmpty())
        return;
    m_undo.append(m_project);
    restoreSnapshot(m_redo.takeLast());
}

void Document::copyPlayback(const Project &from, Project &to)
{
    to.master = from.master;
    to.blackout = from.blackout;
    to.audioMaster = from.audioMaster;
    to.audioMuted = from.audioMuted;
    to.bpm = from.bpm;
    to.crossfader = from.crossfader;
    for (Layer &l : to.layers) {
        for (const Layer &f : from.layers) {
            if (f.id != l.id)
                continue;
            l.activeColumn = f.activeColumn < l.clips.size() ? f.activeColumn : -1;
            l.opacity = f.opacity;
            l.volume = f.volume;
            l.bypass = f.bypass;
            l.solo = f.solo;
        }
    }
}

void Document::restoreSnapshot(const Project &snapshot)
{
    // Undo restores the setup, not the show: whatever is playing keeps playing.
    Project restored = snapshot;
    copyPlayback(m_project, restored);
    m_project = restored;
    m_committed = restored;
    validateSelection();
    setModified(m_project.toJson() != m_savedJson);
    emit mediaChanged();
    emit structureChanged();
    emit changed();
    emit selectionChanged();
    emit undoStateChanged();
}

void Document::resetHistory()
{
    m_commitTimer->stop();
    m_undo.clear();
    m_redo.clear();
    m_committed = m_project;
    m_pendingCommit = false;
    m_savedJson = m_project.toJson();
    emit undoStateChanged();
}

void Document::notifyMediaChanged()
{
    emit mediaChanged();
    notifyChanged();
}

void Document::select(const Selection &sel)
{
    if (sel == m_selection)
        return;
    m_selection = sel;
    validateSelection();
    emit selectionChanged();
}

QVector<ProjectionMask> *Document::maskList(int target)
{
    if (target == kCompositionMasks)
        return &m_project.compMasks;
    if (target >= 0 && target < m_project.screens.size())
        return &m_project.screens[target].masks;
    return nullptr;
}

void Document::setMaskDrawTarget(int target)
{
    if (target == m_drawTarget)
        return;
    m_drawTarget = target;
    emit selectionChanged();
}

int Document::addMask(int target, const ProjectionMask &mask)
{
    QVector<ProjectionMask> *list = maskList(target);
    if (!list)
        return -1;
    ProjectionMask m = mask;
    if (m.name == QLatin1String("Mask"))
        m.name = tr("Mask %1").arg(list->size() + 1);
    list->append(m);
    notifyChanged(true);
    commitUndo();
    setActiveMask(target, int(list->size()) - 1);
    return int(list->size()) - 1;
}

void Document::setActiveMask(int screen, int mask)
{
    if (screen == m_maskScreen && mask == m_mask)
        return;
    m_maskScreen = screen;
    m_mask = mask;
    emit selectionChanged();
}

void Document::validateSelection()
{
    if (const QVector<ProjectionMask> *masks = maskList(m_maskScreen); !masks || m_mask < 0 || m_mask >= masks->size())
        m_maskScreen = m_mask = -1;
    if (m_drawTarget != -1 && !maskList(m_drawTarget))
        m_drawTarget = -1;
    Selection &s = m_selection;
    bool ok = true;
    switch (s.kind) {
    case Selection::None:
    case Selection::Composition:
        break;
    case Selection::Wall:
        ok = s.wall >= 0 && s.wall < m_project.walls.size();
        break;
    case Selection::Screen:
        ok = s.screen >= 0 && s.screen < m_project.screens.size();
        break;
    case Selection::Slice:
        ok = s.screen >= 0 && s.screen < m_project.screens.size() && s.slice >= 0
             && s.slice < m_project.screens[s.screen].slices.size();
        break;
    case Selection::Layer:
        ok = s.layer >= 0 && s.layer < m_project.layers.size();
        break;
    case Selection::Clip:
        ok = s.layer >= 0 && s.layer < m_project.layers.size() && s.column >= 0 && s.column < m_project.columns;
        break;
    }
    if (!ok)
        s = Selection::composition();
}

Layer *Document::selectedLayer()
{
    if (m_selection.kind == Selection::Layer || m_selection.kind == Selection::Clip)
        return &m_project.layers[m_selection.layer];
    return nullptr;
}

Clip *Document::selectedClip()
{
    if (m_selection.kind != Selection::Clip)
        return nullptr;
    Clip &c = m_project.layers[m_selection.layer].clips[m_selection.column];
    return c.isEmpty() ? nullptr : &c;
}

// ---------------------------------------------------------------- clip deck

int Document::addLayer()
{
    Layer l;
    l.id = newId();
    l.name = QStringLiteral("Layer %1").arg(m_project.layers.size() + 1);
    l.clips.resize(m_project.columns);
    m_project.layers.append(l);
    const int idx = m_project.layers.size() - 1;
    notifyChanged(true);
    select(Selection::ofLayer(idx));
    return idx;
}

void Document::removeLayer(int index)
{
    if (index < 0 || index >= m_project.layers.size() || m_project.layers.size() <= 1)
        return; // always keep one layer
    m_project.layers.removeAt(index);
    notifyChanged(true);
}

void Document::addColumn()
{
    ++m_project.columns;
    m_project.normalizeDeck();
    notifyChanged(true);
}

void Document::setClip(int layer, int column, const Clip &clip)
{
    if (layer < 0 || layer >= m_project.layers.size() || column < 0)
        return;
    if (column >= m_project.columns) {
        m_project.columns = column + 1;
        m_project.normalizeDeck();
    }
    Layer &l = m_project.layers[layer];
    l.clips[column] = clip;
    if (clip.isEmpty() && l.activeColumn == column)
        l.activeColumn = -1;
    notifyChanged(true);
    emit mediaChanged();
}

void Document::moveClip(int fromLayer, int fromColumn, int toLayer, int toColumn)
{
    const int nl = m_project.layers.size();
    if (fromLayer < 0 || fromLayer >= nl || toLayer < 0 || toLayer >= nl || fromColumn < 0
        || fromColumn >= m_project.columns || toColumn < 0 || toColumn >= m_project.columns)
        return;
    if (fromLayer == toLayer && fromColumn == toColumn)
        return;
    Layer &a = m_project.layers[fromLayer];
    Layer &b = m_project.layers[toLayer];
    std::swap(a.clips[fromColumn], b.clips[toColumn]);
    // A playing clip keeps playing from its new slot when it stays on the same layer.
    if (fromLayer == toLayer) {
        if (a.activeColumn == fromColumn)
            a.activeColumn = toColumn;
        else if (a.activeColumn == toColumn)
            a.activeColumn = fromColumn;
    } else {
        if (a.activeColumn == fromColumn)
            a.activeColumn = -1;
        if (b.activeColumn == toColumn)
            b.activeColumn = -1;
    }
    notifyChanged(true);
    select(Selection::ofClip(toLayer, toColumn));
}

void Document::notifyPlaybackChanged()
{
    // Keep the undo baseline in step so playback never shows up as an undo step.
    copyPlayback(m_project, m_committed);
    validateSelection();
    emit changed();
}

void Document::triggerClip(int layer, int column)
{
    if (layer < 0 || layer >= m_project.layers.size())
        return;
    Layer &l = m_project.layers[layer];
    if (column < 0 || column >= l.clips.size() || l.clips[column].isEmpty())
        return;
    if (l.clips[column].trigger == TriggerMode::Toggle && l.activeColumn == column) {
        clearLayer(layer); // a toggle clip that is playing stops
        return;
    }
    // Quantized: wait for the next beat or bar (the clip is identified by id, in case it moves).
    if (m_project.quantize != TriggerQuantize::Off && m_quantizer) {
        const int ms = m_quantizer(m_project.quantize == TriggerQuantize::Bar ? 4 : 1);
        if (ms > 5) {
            const QString id = l.clips[column].id;
            QTimer::singleShot(ms, Qt::PreciseTimer, this, [this, layer, id] {
                if (layer >= m_project.layers.size())
                    return;
                const QVector<::Clip> &clips = m_project.layers[layer].clips;
                for (int c = 0; c < clips.size(); ++c)
                    if (clips[c].id == id)
                        startClip(layer, c);
            });
            return;
        }
    }
    startClip(layer, column);
}

void Document::startClip(int layer, int column)
{
    Layer &l = m_project.layers[layer];
    l.activeColumn = column;
    notifyPlaybackChanged();
    emit clipTriggered(l.clips[column].id);
}

void Document::triggerColumn(int column)
{
    if (column < 0 || column >= m_project.columns)
        return;
    if (m_project.quantize != TriggerQuantize::Off && m_quantizer) {
        const int ms = m_quantizer(m_project.quantize == TriggerQuantize::Bar ? 4 : 1);
        if (ms > 5) {
            QTimer::singleShot(ms, Qt::PreciseTimer, this, [this, column] { startColumn(column); });
            return;
        }
    }
    startColumn(column);
}

void Document::startColumn(int column)
{
    if (column < 0 || column >= m_project.columns)
        return;
    // A column is a scene: layers with an empty slot in it are cleared.
    QStringList triggered;
    for (Layer &l : m_project.layers) {
        const bool has = column < l.clips.size() && !l.clips[column].isEmpty();
        l.activeColumn = has ? column : -1;
        if (has)
            triggered << l.clips[column].id;
    }
    notifyPlaybackChanged();
    for (const QString &id : std::as_const(triggered))
        emit clipTriggered(id);
}

void Document::releaseClip(int layer, int column)
{
    if (layer < 0 || layer >= m_project.layers.size())
        return;
    const Layer &l = m_project.layers[layer];
    if (l.activeColumn == column && column < l.clips.size() && l.clips[column].trigger == TriggerMode::Piano)
        clearLayer(layer);
}

void Document::clearLayer(int layer)
{
    if (layer < 0 || layer >= m_project.layers.size())
        return;
    m_project.layers[layer].activeColumn = -1;
    notifyPlaybackChanged();
}

void Document::setBpm(double bpm)
{
    bpm = qBound(20.0, bpm, 300.0);
    if (qAbs(bpm - m_project.bpm) < 1e-6)
        return;
    m_project.bpm = bpm;
    notifyPlaybackChanged();
}

void Document::setCrossfader(double value)
{
    m_project.crossfader = qBound(0.0, value, 1.0);
    notifyPlaybackChanged();
}

void Document::setMaster(double master)
{
    m_project.master = qBound(0.0, master, 1.0);
    notifyPlaybackChanged();
}

void Document::setBlackout(bool on)
{
    m_project.blackout = on;
    notifyPlaybackChanged();
}

void Document::setLayerVolume(int layer, double volume)
{
    if (layer < 0 || layer >= m_project.layers.size())
        return;
    m_project.layers[layer].volume = qBound(0.0, volume, 1.0);
    notifyPlaybackChanged();
}

void Document::setAudioMaster(double volume)
{
    m_project.audioMaster = qBound(0.0, volume, 1.0);
    notifyPlaybackChanged();
}

void Document::setAudioMuted(bool muted)
{
    m_project.audioMuted = muted;
    notifyPlaybackChanged();
}

void Document::setEffectParam(int layer, int effect, int param, double value)
{
    auto chainOf = [layer](Project &p) -> QVector<EffectInstance> * {
        if (layer < 0)
            return &p.compEffects;
        return layer < p.layers.size() ? &p.layers[layer].effects : nullptr;
    };
    QVector<EffectInstance> *chain = chainOf(m_project);
    if (!chain || effect < 0 || effect >= chain->size() || param < 0 || param >= 64)
        return;
    auto set = [&](EffectInstance &e) {
        while (e.params.size() <= param)
            e.params.append(effectParam(e, int(e.params.size())));
        e.params[param] = value;
    };
    set((*chain)[effect]);
    // Like playback, the undo baseline follows so a fader move is not an undo step; the file
    // does change, though.
    QVector<EffectInstance> *committed = chainOf(m_committed);
    if (committed && effect < committed->size() && (*committed)[effect].type == (*chain)[effect].type)
        set((*committed)[effect]);
    setModified(true);
    emit changed();
}

void Document::setLayerPlayback(int layer, double opacity, bool bypass, bool solo)
{
    if (layer < 0 || layer >= m_project.layers.size())
        return;
    Layer &l = m_project.layers[layer];
    l.opacity = qBound(0.0, opacity, 1.0);
    l.bypass = bypass;
    l.solo = solo;
    notifyPlaybackChanged();
}

LedWall *Document::selectedWall()
{
    return m_selection.kind == Selection::Wall ? &m_project.walls[m_selection.wall] : nullptr;
}

Screen *Document::selectedScreen()
{
    if (m_selection.kind == Selection::Screen || m_selection.kind == Selection::Slice)
        return &m_project.screens[m_selection.screen];
    return nullptr;
}

Slice *Document::selectedSlice()
{
    if (m_selection.kind == Selection::Slice)
        return &m_project.screens[m_selection.screen].slices[m_selection.slice];
    return nullptr;
}

void Document::applyWall(int index, const LedWall &wall)
{
    if (index < 0 || index >= m_project.walls.size())
        return;
    const QRect oldRect = m_project.walls[index].rect();
    const double oldAngle = m_project.walls[index].angle;
    const QRect newRect = wall.rect();
    const bool renamed = m_project.walls[index].name != wall.name;
    if (oldRect != newRect || oldAngle != wall.angle) {
        for (Screen &s : m_project.screens) {
            for (Slice &sl : s.slices) {
                if (sl.isCabinet() || sl.input != oldRect || sl.angle != oldAngle)
                    continue;
                if (sl.output.size() == oldRect.size())
                    sl.output.setSize(newRect.size());
                sl.input = newRect;
                sl.angle = wall.angle;
            }
        }
    }
    m_project.walls[index] = wall;
    CabinetMapping::regenerate(m_project, index); // cabinet slices follow the wall
    notifyChanged(renamed || hasCabinetLayout(index));
}

bool Document::hasCabinetLayout(int wall) const
{
    for (const Screen &s : m_project.screens)
        for (const CabinetLayout &c : s.cabinetLayouts)
            if (c.wall == wall)
                return true;
    return false;
}

int Document::addCabinetLayout(int screen, const CabinetLayout &layout)
{
    if (screen < 0 || screen >= m_project.screens.size())
        return -1;
    Screen &s = m_project.screens[screen];
    for (int i = 0; i < s.cabinetLayouts.size(); ++i) {
        if (s.cabinetLayouts[i].wall == layout.wall) { // one layout per wall and screen
            setCabinetLayout(screen, i, layout);
            return i;
        }
    }
    s.cabinetLayouts.append(layout);
    CabinetMapping::regenerate(m_project, layout.wall);
    notifyChanged(true);
    return int(s.cabinetLayouts.size()) - 1;
}

void Document::setCabinetLayout(int screen, int index, const CabinetLayout &layout)
{
    if (screen < 0 || screen >= m_project.screens.size() || index < 0
        || index >= m_project.screens[screen].cabinetLayouts.size())
        return;
    Screen &s = m_project.screens[screen];
    const int oldWall = s.cabinetLayouts[index].wall;
    if (oldWall != layout.wall)
        removeCabinetSlices(s, oldWall);
    s.cabinetLayouts[index] = layout;
    CabinetMapping::regenerate(m_project, layout.wall);
    validateSelection();
    notifyChanged(true);
}

void Document::removeCabinetLayout(int screen, int index)
{
    if (screen < 0 || screen >= m_project.screens.size() || index < 0
        || index >= m_project.screens[screen].cabinetLayouts.size())
        return;
    Screen &s = m_project.screens[screen];
    removeCabinetSlices(s, s.cabinetLayouts[index].wall);
    s.cabinetLayouts.removeAt(index);
    validateSelection();
    notifyChanged(true);
}

void Document::removeCabinetSlices(Screen &screen, int wall)
{
    for (int i = 0; i < screen.slices.size();) {
        if (screen.slices[i].isCabinet() && screen.slices[i].wall == wall)
            screen.slices.removeAt(i);
        else
            ++i;
    }
}

int Document::addWall()
{
    LedWall w;
    w.name = QStringLiteral("LED Wall %1").arg(m_project.walls.size() + 1);
    // Place new walls below the existing ones so they don't overlap.
    int bottom = 0;
    for (const LedWall &o : m_project.walls)
        bottom = qMax(bottom, o.y + o.pixelHeight());
    w.y = bottom;
    static const QColor palette[] = {QColor(0, 190, 255), QColor(255, 140, 0), QColor(120, 220, 60),
                                     QColor(230, 70, 200), QColor(255, 220, 0), QColor(150, 110, 255)};
    w.color = palette[m_project.walls.size() % 6];
    m_project.walls.append(w);
    const int idx = m_project.walls.size() - 1;
    notifyChanged(true);
    select(Selection::ofWall(idx));
    return idx;
}

void Document::removeWall(int index)
{
    if (index < 0 || index >= m_project.walls.size())
        return;
    m_project.walls.removeAt(index);
    // Its cabinet slices and layouts go with it; later walls move down one index.
    for (Screen &s : m_project.screens) {
        removeCabinetSlices(s, index);
        for (int i = 0; i < s.cabinetLayouts.size();) {
            if (s.cabinetLayouts[i].wall == index) {
                s.cabinetLayouts.removeAt(i);
                continue;
            }
            if (s.cabinetLayouts[i].wall > index)
                --s.cabinetLayouts[i].wall;
            ++i;
        }
        for (Slice &sl : s.slices)
            if (sl.wall > index)
                --sl.wall;
    }
    validateSelection();
    for (Layer &l : m_project.layers) {
        if (l.targetWall == index)
            l.targetWall = -1;
        else if (l.targetWall > index)
            --l.targetWall;
    }
    notifyChanged(true);
}

int Document::addScreen()
{
    Screen s;
    s.name = QStringLiteral("Output %1").arg(m_project.screens.size() + 1);
    m_project.screens.append(s);
    const int idx = m_project.screens.size() - 1;
    notifyChanged(true);
    select(Selection::ofScreen(idx));
    return idx;
}

void Document::removeScreen(int index)
{
    if (index < 0 || index >= m_project.screens.size())
        return;
    m_project.screens.removeAt(index);
    notifyChanged(true);
}

int Document::addSlice(int screen)
{
    if (m_project.screens.isEmpty())
        screen = addScreen();
    screen = qBound(0, screen, int(m_project.screens.size()) - 1);
    Screen &s = m_project.screens[screen];
    Slice sl;
    sl.name = QStringLiteral("Slice %1").arg(s.slices.size() + 1);
    const int w = qMin(m_project.compWidth, s.width) / 2;
    const int h = qMin(m_project.compHeight, s.height) / 2;
    sl.input = QRect(0, 0, qMax(1, w), qMax(1, h));
    sl.output = QRect(0, 0, qMax(1, w), qMax(1, h));
    s.slices.append(sl);
    const int idx = s.slices.size() - 1;
    notifyChanged(true);
    select(Selection::ofSlice(screen, idx));
    return idx;
}

void Document::removeSlice(int screen, int slice)
{
    if (screen < 0 || screen >= m_project.screens.size())
        return;
    QVector<Slice> &slices = m_project.screens[screen].slices;
    if (slice < 0 || slice >= slices.size())
        return;
    slices.removeAt(slice);
    notifyChanged(true);
}

void Document::duplicateSelection()
{
    const Selection s = m_selection;
    if (s.kind == Selection::Wall) {
        LedWall w = m_project.walls[s.wall];
        w.name += QStringLiteral(" copy");
        w.x += w.pixelWidth();
        m_project.walls.append(w);
        notifyChanged(true);
        select(Selection::ofWall(m_project.walls.size() - 1));
    } else if (s.kind == Selection::Screen) {
        Screen sc = m_project.screens[s.screen];
        sc.name += QStringLiteral(" copy");
        sc.display.clear();
        m_project.screens.append(sc);
        notifyChanged(true);
        select(Selection::ofScreen(m_project.screens.size() - 1));
    } else if (s.kind == Selection::Slice) {
        QVector<Slice> &slices = m_project.screens[s.screen].slices;
        Slice sl = slices[s.slice];
        sl.name += QStringLiteral(" copy");
        sl.output.translate(sl.output.width(), 0);
        slices.append(sl);
        notifyChanged(true);
        select(Selection::ofSlice(s.screen, slices.size() - 1));
    } else if (s.kind == Selection::Clip && selectedClip()) {
        // Copy into the next empty slot on the same layer.
        Clip c = *selectedClip();
        c.id = newId();
        const Layer &l = m_project.layers[s.layer];
        int target = s.column + 1;
        while (target < l.clips.size() && !l.clips[target].isEmpty())
            ++target;
        setClip(s.layer, target, c);
        select(Selection::ofClip(s.layer, target));
    } else if (s.kind == Selection::Layer) {
        Layer l = m_project.layers[s.layer];
        l.id = newId();
        l.name += QStringLiteral(" copy");
        l.activeColumn = -1;
        for (Clip &c : l.clips)
            if (!c.isEmpty())
                c.id = newId();
        m_project.layers.insert(s.layer + 1, l);
        notifyChanged(true);
        select(Selection::ofLayer(s.layer + 1));
    }
}

void Document::deleteSelection()
{
    const Selection s = m_selection;
    switch (s.kind) {
    case Selection::Wall: removeWall(s.wall); break;
    case Selection::Screen: removeScreen(s.screen); break;
    case Selection::Slice:
        removeSlice(s.screen, s.slice);
        select(Selection::ofScreen(s.screen));
        break;
    case Selection::Clip:
        setClip(s.layer, s.column, Clip());
        break;
    case Selection::Layer: removeLayer(s.layer); break;
    default: break;
    }
}

int Document::createSliceForWall(int wall, int screen)
{
    if (wall < 0 || wall >= m_project.walls.size())
        return -1;
    if (m_project.screens.isEmpty())
        screen = addScreen();
    if (screen < 0 || screen >= m_project.screens.size())
        return -1;

    const LedWall &w = m_project.walls[wall];
    Screen &s = m_project.screens[screen];

    // Pack after the right-most slice on the bottom row; wrap to a new row when out of room.
    int x = 0, y = 0, rowBottom = 0;
    for (const Slice &o : s.slices) {
        if (o.output.y() > y) {
            y = o.output.y();
            x = 0;
        }
        if (o.output.y() == y)
            x = qMax(x, o.output.x() + o.output.width());
        rowBottom = qMax(rowBottom, o.output.y() + o.output.height());
    }
    if (x + w.pixelWidth() > s.width && x > 0) {
        x = 0;
        y = rowBottom;
    }

    Slice sl;
    sl.name = w.name;
    sl.input = w.rect();
    sl.angle = w.angle; // a rotated wall is sampled at its angle; the output stays upright
    sl.output = QRect(x, y, w.pixelWidth(), w.pixelHeight());
    s.slices.append(sl);
    const int idx = s.slices.size() - 1;
    notifyChanged(true);
    select(Selection::ofSlice(screen, idx));
    return idx;
}

void Document::fitCompositionToWalls()
{
    if (m_project.walls.isEmpty())
        return;
    QRect bounds;
    for (const LedWall &w : m_project.walls)
        bounds = bounds.united(w.bounds());
    const QPoint shift = -bounds.topLeft();
    for (LedWall &w : m_project.walls) {
        w.x += shift.x();
        w.y += shift.y();
    }
    // Keep slices that sample the walls pointing at the same pixels.
    for (Screen &s : m_project.screens)
        for (Slice &sl : s.slices)
            sl.input.translate(shift);
    m_project.compWidth = bounds.width();
    m_project.compHeight = bounds.height();
    notifyChanged();
}

void Document::newProject()
{
    m_project = Project::makeDefault();
    m_filePath.clear();
    m_selection = Selection::composition();
    resetHistory();
    emit mediaChanged();
    emit structureChanged();
    emit changed();
    emit selectionChanged();
    setModified(false);
    emit fileStateChanged();
}

bool Document::loadTemplate(const QString &path, QString *error)
{
    if (!load(path, error))
        return false;
    m_filePath.clear();
    m_savedJson = QJsonObject(); // never saved under a name of its own
    setModified(true);
    emit fileStateChanged();
    return true;
}

bool Document::load(const QString &path, QString *error, bool recovery)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = pe.errorString();
        return false;
    }
    m_project = Project::fromJson(doc.object());
    m_filePath = recovery ? doc.object()["autosaveOriginalPath"].toString() : path;
    m_selection = Selection::composition();
    resetHistory();
    if (recovery)
        m_savedJson = QJsonObject(); // the recovered state has never been saved by the user
    emit mediaChanged();
    emit structureChanged();
    emit changed();
    emit selectionChanged();
    setModified(recovery);
    emit fileStateChanged();
    return true;
}

static bool writeJson(const QString &path, const QJsonObject &obj, QString *error)
{
    // QSaveFile writes to a temp file and renames, so a crash mid-save never leaves a truncated setup.
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

bool Document::writeAutosave(const QString &path) const
{
    QJsonObject obj = m_project.toJson();
    obj["autosaveOriginalPath"] = m_filePath;
    return writeJson(path, obj, nullptr);
}

bool Document::save(const QString &path, QString *error)
{
    if (!writeJson(path, m_project.toJson(), error))
        return false;
    m_savedJson = m_project.toJson();
    m_filePath = path;
    setModified(false);
    emit fileStateChanged();
    return true;
}

void Document::setModified(bool m)
{
    if (m_modified == m)
        return;
    m_modified = m;
    emit fileStateChanged();
}
