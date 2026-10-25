#include "model/Document.h"

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

void Document::restoreSnapshot(const Project &snapshot)
{
    const bool mediaDiffers = snapshot.content.source != m_project.content.source
                              || snapshot.content.mediaPath != m_project.content.mediaPath;
    m_project = snapshot;
    m_committed = snapshot;
    validateSelection();
    setModified(m_project.toJson() != m_savedJson);
    if (mediaDiffers)
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

void Document::validateSelection()
{
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
    }
    if (!ok)
        s = Selection::composition();
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
    const QRect newRect = wall.rect();
    const bool renamed = m_project.walls[index].name != wall.name;
    if (oldRect != newRect) {
        for (Screen &s : m_project.screens) {
            for (Slice &sl : s.slices) {
                if (sl.input != oldRect)
                    continue;
                if (sl.output.size() == oldRect.size())
                    sl.output.setSize(newRect.size());
                sl.input = newRect;
            }
        }
    }
    m_project.walls[index] = wall;
    notifyChanged(renamed);
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
    int &tw = m_project.content.targetWall;
    if (tw == index)
        tw = -1;
    else if (tw > index)
        --tw;
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
        bounds = bounds.united(w.rect());
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
