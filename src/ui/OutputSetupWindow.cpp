#include "ui/OutputSetupWindow.h"

#include "model/Document.h"
#include "render/RenderEngine.h"
#include "ui/CanvasView.h"
#include "ui/PropertyPanel.h"

#include <QComboBox>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QToolBar>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

namespace {

constexpr int kKindRole = Qt::UserRole;
constexpr int kARole = Qt::UserRole + 1;
constexpr int kBRole = Qt::UserRole + 2;
constexpr int kGroupRole = Qt::UserRole + 3; // cabinet group nodes: "screen:wall"

Selection selectionOf(const QTreeWidgetItem *item)
{
    Selection s;
    s.kind = Selection::Kind(item->data(0, kKindRole).toInt());
    const int a = item->data(0, kARole).toInt();
    const int b = item->data(0, kBRole).toInt();
    if (s.kind == Selection::Wall)
        s.wall = a;
    else if (s.kind == Selection::Screen)
        s.screen = a;
    else if (s.kind == Selection::Slice) {
        s.screen = a;
        s.slice = b;
    }
    return s;
}

QTreeWidgetItem *makeItem(QTreeWidgetItem *parent, const QString &text, const QString &detail, Selection::Kind kind,
                          int a = -1, int b = -1)
{
    auto *item = new QTreeWidgetItem(parent, {text, detail});
    item->setData(0, kKindRole, int(kind));
    item->setData(0, kARole, a);
    item->setData(0, kBRole, b);
    return item;
}

} // namespace

OutputSetupWindow::OutputSetupWindow(Document *doc, RenderEngine *engine, QWidget *parent)
    : QMainWindow(parent)
    , m_doc(doc)
    , m_engine(engine)
{
    setObjectName(QStringLiteral("outputSetup"));
    setWindowTitle(tr("Output Setup - Display Hawk"));
    resize(1500, 900);

    m_inputView = new CanvasView(doc, engine, CanvasView::InputMap);
    m_outputView = new CanvasView(doc, engine, CanvasView::OutputMap);

    auto *inputPane = new QWidget;
    auto *il = new QVBoxLayout(inputPane);
    il->setContentsMargins(0, 0, 0, 0);
    il->setSpacing(2);
    auto *inputTitle = new QLabel(tr("<b>Input map</b> &nbsp; composition, LED walls and the area each slice samples"));
    inputTitle->setContentsMargins(6, 4, 6, 0);
    il->addWidget(inputTitle);
    il->addWidget(m_inputView, 1);

    auto *outputPane = new QWidget;
    auto *ol = new QVBoxLayout(outputPane);
    ol->setContentsMargins(0, 0, 0, 0);
    ol->setSpacing(2);
    auto *outHeader = new QHBoxLayout;
    outHeader->setContentsMargins(6, 4, 6, 0);
    outHeader->addWidget(new QLabel(tr("<b>Output map</b> &nbsp; where each slice lands on")));
    m_outputScreen = new QComboBox;
    m_outputScreen->setMinimumWidth(180);
    outHeader->addWidget(m_outputScreen);
    outHeader->addStretch();
    ol->addLayout(outHeader);
    ol->addWidget(m_outputView, 1);

    auto *splitter = new QSplitter(Qt::Vertical);
    splitter->addWidget(inputPane);
    splitter->addWidget(outputPane);
    splitter->setSizes({500, 400});
    setCentralWidget(splitter);

    buildTree();

    m_props = new PropertyPanel(doc, engine, PropertyPanel::Scope::Setup);
    auto *propDock = new QDockWidget(tr("Properties"));
    propDock->setObjectName(QStringLiteral("setupProperties"));
    propDock->setWidget(m_props);
    propDock->setMinimumWidth(360);
    addDockWidget(Qt::RightDockWidgetArea, propDock);
    connect(m_props, &PropertyPanel::identifyRequested, this, &OutputSetupWindow::identifyRequested);
    connect(m_props, &PropertyPanel::midiLearnRequested, this, &OutputSetupWindow::midiLearnRequested);

    m_toolbar = addToolBar(tr("Setup"));
    m_toolbar->setObjectName(QStringLiteral("setupToolbar"));
    m_toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);

    m_cursorLabel = new QLabel;
    statusBar()->addWidget(m_cursorLabel, 1);
    connect(m_inputView, &CanvasView::statusMessage, m_cursorLabel, &QLabel::setText);
    connect(m_outputView, &CanvasView::statusMessage, m_cursorLabel, &QLabel::setText);

    connect(m_doc, &Document::structureChanged, this, [this] {
        rebuildTree();
        refreshOutputScreenCombo();
    });
    connect(m_doc, &Document::selectionChanged, this, [this] {
        syncTreeSelection();
        const Selection s = m_doc->selection();
        if (s.screen >= 0) {
            m_outputView->setScreenIndex(s.screen);
            QSignalBlocker block(m_outputScreen);
            m_outputScreen->setCurrentIndex(s.screen);
        }
    });
    connect(m_outputScreen, &QComboBox::activated, this, [this](int i) {
        m_outputView->setScreenIndex(i);
        m_doc->select(Selection::ofScreen(i));
    });

    rebuildTree();
    refreshOutputScreenCombo();
}

void OutputSetupWindow::addSharedActions(const QList<QAction *> &actions, const QList<QAction *> &toolbarActions)
{
    addActions(actions); // their shortcuts work while this window is active
    for (QAction *a : toolbarActions) {
        if (a)
            m_toolbar->addAction(a);
        else
            m_toolbar->addSeparator();
    }
    m_toolbar->addSeparator();
    m_toolbar->addAction(tr("Reset views"), this, [this] {
        m_inputView->resetView();
        m_outputView->resetView();
    });
}

int OutputSetupWindow::currentScreen() const
{
    return m_outputView->screenIndex();
}

void OutputSetupWindow::showEvent(QShowEvent *e)
{
    if (!m_restored) {
        m_restored = true;
        QSettings s;
        restoreGeometry(s.value(QStringLiteral("ui/setupGeometry")).toByteArray());
        restoreState(s.value(QStringLiteral("ui/setupState")).toByteArray());
    }
    QMainWindow::showEvent(e);
    statusBar()->showMessage(tr("Tip: Shift disables snapping, Alt+click selects items underneath, "
                                "arrow keys nudge, wheel zooms, drag empty space to pan."), 15000);
    emit visibilityChanged(true);
}

void OutputSetupWindow::hideEvent(QHideEvent *e)
{
    QSettings s;
    s.setValue(QStringLiteral("ui/setupGeometry"), saveGeometry());
    s.setValue(QStringLiteral("ui/setupState"), saveState());
    QMainWindow::hideEvent(e);
    emit visibilityChanged(false);
}

void OutputSetupWindow::buildTree()
{
    m_tree = new QTreeWidget;
    m_tree->setHeaderLabels({tr("Item"), tr("Size")});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setStretchLastSection(false);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, &OutputSetupWindow::onTreeSelection);

    auto *dock = new QDockWidget(tr("Setup"));
    dock->setObjectName(QStringLiteral("setupTree"));
    dock->setWidget(m_tree);
    dock->setMinimumWidth(260);
    addDockWidget(Qt::LeftDockWidgetArea, dock);
}

void OutputSetupWindow::rebuildTree()
{
    m_syncingTree = true;
    const Project &p = m_doc->project();
    // Cabinet groups stay as the user left them (collapsed by default).
    QSet<QString> expandedGroups;
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it)
        if ((*it)->isExpanded() && !(*it)->data(0, kGroupRole).toString().isEmpty())
            expandedGroups.insert((*it)->data(0, kGroupRole).toString());
    m_tree->clear();

    auto *comp = makeItem(nullptr, tr("Composition"), QStringLiteral("%1 x %2").arg(p.compWidth).arg(p.compHeight),
                          Selection::Composition);
    m_tree->addTopLevelItem(comp);

    auto *walls = makeItem(nullptr, tr("LED Walls"), QString::number(p.walls.size()), Selection::None);
    m_tree->addTopLevelItem(walls);
    for (int i = 0; i < p.walls.size(); ++i) {
        const LedWall &w = p.walls[i];
        auto *it = makeItem(walls, w.name, QStringLiteral("%1 x %2").arg(w.pixelWidth()).arg(w.pixelHeight()),
                            Selection::Wall, i);
        it->setForeground(0, w.color);
    }

    QVector<QTreeWidgetItem *> collapsed;
    auto *screens = makeItem(nullptr, tr("Screens"), QString::number(p.screens.size()), Selection::None);
    m_tree->addTopLevelItem(screens);
    for (int s = 0; s < p.screens.size(); ++s) {
        const Screen &sc = p.screens[s];
        auto *si = makeItem(screens, sc.name, QStringLiteral("%1 x %2").arg(sc.width).arg(sc.height),
                            Selection::Screen, s);
        QHash<int, QTreeWidgetItem *> groups; // wall -> its cabinet slices' node
        for (int i = 0; i < sc.slices.size(); ++i) {
            const Slice &sl = sc.slices[i];
            QTreeWidgetItem *parent = si;
            if (sl.isCabinet()) {
                QTreeWidgetItem *&g = groups[sl.wall];
                if (!g) {
                    const QString wallName = sl.wall < p.walls.size() ? p.walls[sl.wall].name : QString();
                    g = makeItem(si, tr("%1 cabinets").arg(wallName), QString(), Selection::Screen, s);
                    const QString key = QStringLiteral("%1:%2").arg(s).arg(sl.wall);
                    g->setData(0, kGroupRole, key);
                    if (!expandedGroups.contains(key))
                        collapsed << g;
                }
                parent = g;
            }
            const QString label = sl.isCabinet() && sl.port > 0 ? tr("%1  (port %2)").arg(sl.name).arg(sl.port) : sl.name;
            auto *it = makeItem(parent, label, QStringLiteral("%1 x %2").arg(sl.output.width()).arg(sl.output.height()),
                                Selection::Slice, s, i);
            if (!sl.enabled)
                it->setForeground(0, QColor(130, 130, 130));
        }
        for (auto g = groups.cbegin(); g != groups.cend(); ++g)
            g.value()->setText(1, QString::number(g.value()->childCount()));
    }
    m_tree->expandAll();
    for (QTreeWidgetItem *g : std::as_const(collapsed))
        g->setExpanded(false);
    m_syncingTree = false;
    syncTreeSelection();
}

void OutputSetupWindow::syncTreeSelection()
{
    m_syncingTree = true;
    const Selection sel = m_doc->selection();
    QTreeWidgetItemIterator it(m_tree);
    while (*it) {
        // Group nodes stand for their screen; the screen's own row wins.
        if (selectionOf(*it) == sel && (*it)->data(0, kGroupRole).toString().isEmpty()) {
            m_tree->setCurrentItem(*it);
            break;
        }
        ++it;
    }
    m_syncingTree = false;
}

void OutputSetupWindow::onTreeSelection()
{
    if (m_syncingTree || !m_tree->currentItem())
        return;
    const Selection s = selectionOf(m_tree->currentItem());
    if (s.kind != Selection::None)
        m_doc->select(s);
}

void OutputSetupWindow::refreshOutputScreenCombo()
{
    const Project &p = m_doc->project();
    QSignalBlocker block(m_outputScreen);
    m_outputScreen->clear();
    for (const Screen &s : p.screens)
        m_outputScreen->addItem(s.name);
    const int idx = qBound(0, m_outputView->screenIndex(), int(p.screens.size()) - 1);
    m_outputScreen->setCurrentIndex(idx);
    m_outputView->setScreenIndex(idx);
}
