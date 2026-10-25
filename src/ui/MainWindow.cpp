#include "ui/MainWindow.h"

#include <QSet>

#include "model/Document.h"
#include "render/RenderEngine.h"
#include "audio/AudioReactive.h"
#include "control/ControlRouter.h"
#include "ui/AudioReactiveDialog.h"
#include "ui/CanvasView.h"
#include "ui/ControlDialog.h"
#include "ui/ClipDeck.h"
#include "ui/IdentifyOverlay.h"
#include "ui/OutputWindow.h"
#include "ui/PropertyPanel.h"
#include "ui/ReportExporter.h"

#include <QAction>
#include <QActionGroup>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QPushButton>
#include <QUrl>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QScreen>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

constexpr int kKindRole = Qt::UserRole;
constexpr int kARole = Qt::UserRole + 1;
constexpr int kBRole = Qt::UserRole + 2;
constexpr int kGroupRole = Qt::UserRole + 3; // cabinet group nodes: "screen:wall"
constexpr int kAutosaveMs = 15000;

const char *kSettingLastProject = "lastProject";
const char *kSettingReopenLast = "reopenLastOnLaunch";
const char *kSettingLiveOnLaunch = "goLiveOnLaunch";

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

MainWindow::MainWindow(Document *doc, RenderEngine *engine, QWidget *parent)
    : QMainWindow(parent)
    , m_doc(doc)
    , m_engine(engine)
{
    resize(1600, 950);

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

    m_props = new PropertyPanel(doc, engine);
    auto *propDock = new QDockWidget(tr("Properties"));
    propDock->setObjectName(QStringLiteral("properties"));
    propDock->setWidget(m_props);
    propDock->setMinimumWidth(360);
    addDockWidget(Qt::RightDockWidgetArea, propDock);
    connect(m_props, &PropertyPanel::identifyRequested, this, &MainWindow::identifyDisplays);
    connect(m_props, &PropertyPanel::midiLearnRequested, this, [this](int action, int layer, int column, int param) {
        showControls();
        m_controlDialog->learnNew(action, layer, column, param);
    });

    m_router = new ControlRouter(doc, this);
    m_audioReactive = new AudioReactive(doc, engine, this);
    connect(m_router, &ControlRouter::liveRequested, this, [this](int state) { setLive(state < 0 ? !m_live : state == 1); });

    auto *deck = new ClipDeck(doc);
    connect(deck, &ClipDeck::midiLearnRequested, this, [this](int action, int layer, int column) {
        showControls();
        m_controlDialog->learnNew(action, layer, column);
    });
    auto *deckDock = new QDockWidget(tr("Clip Deck"));
    deckDock->setObjectName(QStringLiteral("deck"));
    deckDock->setWidget(deck);
    addDockWidget(Qt::BottomDockWidgetArea, deckDock);
    resizeDocks({deckDock}, {280}, Qt::Vertical);

    m_cursorLabel = new QLabel;
    m_fpsLabel = new QLabel;
    statusBar()->addWidget(m_cursorLabel, 1);
    statusBar()->addPermanentWidget(m_fpsLabel);
    connect(m_inputView, &CanvasView::statusMessage, m_cursorLabel, &QLabel::setText);
    connect(m_outputView, &CanvasView::statusMessage, m_cursorLabel, &QLabel::setText);
    connect(m_engine, &RenderEngine::statsUpdated, this, [this](int fps, const QSize &comp) {
        m_fpsLabel->setText(tr("%1 fps  |  %2 x %3  |  %4  |  %5")
                                .arg(fps)
                                .arg(comp.width())
                                .arg(comp.height())
                                .arg(m_engine->backendName())
                                .arg(m_live ? tr("LIVE") : tr("preview")));
    });

    buildActions();

    connect(m_doc, &Document::structureChanged, this, [this] {
        rebuildTree();
        refreshOutputScreenCombo();
    });
    connect(m_doc, &Document::changed, this, [this] {
        m_autosaveDirty = true;
        syncOutputs();
    });
    connect(m_doc, &Document::fileStateChanged, this, &MainWindow::updateTitle);
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
    connect(qApp, &QGuiApplication::screenAdded, this, &MainWindow::syncOutputs);
    connect(qApp, &QGuiApplication::screenRemoved, this, &MainWindow::syncOutputs);

    rebuildTree();
    refreshOutputScreenCombo();
    updateTitle();

    m_autosaveTimer = new QTimer(this);
    m_autosaveTimer->setInterval(kAutosaveMs);
    connect(m_autosaveTimer, &QTimer::timeout, this, &MainWindow::autosave);
    m_autosaveTimer->start();

    statusBar()->showMessage(tr("Tip: Shift disables snapping, Alt+click selects items underneath, "
                                "arrow keys nudge, wheel zooms, drag empty space to pan."), 15000);
}

MainWindow::~MainWindow()
{
    closeOutputs();
}

void MainWindow::buildActions()
{
    QSettings settings;

    QMenu *file = menuBar()->addMenu(tr("&File"));
    QAction *newAct = file->addAction(tr("&New"), QKeySequence::New, this, [this] {
        if (confirmDiscard())
            m_doc->newProject();
    });
    QAction *openAct = file->addAction(tr("&Open..."), QKeySequence::Open, this, &MainWindow::openProject);
    QAction *saveAct = file->addAction(tr("&Save"), QKeySequence::Save, this, &MainWindow::saveProject);
    file->addAction(tr("Save &As..."), QKeySequence::SaveAs, this, &MainWindow::saveProjectAs);
    file->addAction(tr("Export setup &report..."), this, &MainWindow::exportReport);
    file->addSeparator();
    m_reopenLastAction = file->addAction(tr("Reopen last setup on launch"));
    m_reopenLastAction->setCheckable(true);
    m_reopenLastAction->setChecked(settings.value(kSettingReopenLast, true).toBool());
    connect(m_reopenLastAction, &QAction::toggled, this, [](bool on) { QSettings().setValue(kSettingReopenLast, on); });
    file->addSeparator();
    file->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    m_undoAction = edit->addAction(tr("&Undo"), QKeySequence::Undo, m_doc, &Document::undo);
    m_redoAction = edit->addAction(tr("&Redo"), QKeySequence::Redo, m_doc, &Document::redo);
    auto syncUndo = [this] {
        m_undoAction->setEnabled(m_doc->canUndo());
        m_redoAction->setEnabled(m_doc->canRedo());
    };
    connect(m_doc, &Document::undoStateChanged, this, syncUndo);
    syncUndo();
    edit->addSeparator();
    QAction *addWall = edit->addAction(tr("Add LED &Wall"), QKeySequence(tr("Ctrl+Shift+W")), m_doc, &Document::addWall);
    QAction *addScreen = edit->addAction(tr("Add &Screen"), QKeySequence(tr("Ctrl+Shift+S")), m_doc, &Document::addScreen);
    QAction *addSlice = edit->addAction(tr("Add S&lice"), QKeySequence(tr("Ctrl+Shift+L")), this, [this] {
        const Selection s = m_doc->selection();
        m_doc->addSlice(s.screen >= 0 ? s.screen : m_outputView->screenIndex());
    });
    edit->addSeparator();
    QAction *dup = edit->addAction(tr("&Duplicate"), QKeySequence(tr("Ctrl+D")), m_doc, &Document::duplicateSelection);
    QAction *del = edit->addAction(tr("De&lete"), QKeySequence::Delete, m_doc, &Document::deleteSelection);
#ifdef Q_OS_MACOS
    // Mac keyboards label Backspace "delete"; text fields still get it first via ShortcutOverride.
    del->setShortcuts({QKeySequence::Delete, QKeySequence(Qt::Key_Backspace)});
#endif

    QMenu *output = menuBar()->addMenu(tr("&Output"));
    m_liveAction = output->addAction(tr("Outputs &Live"));
    m_liveAction->setCheckable(true);
    m_liveAction->setShortcut(QKeySequence(Qt::Key_F9));
    m_liveAction->setToolTip(tr("Open every screen on its assigned display (F9). Press Esc on an output to stop."));
    connect(m_liveAction, &QAction::toggled, this, &MainWindow::setLive);
    QAction *identify = output->addAction(tr("&Identify Displays"), QKeySequence(tr("Ctrl+I")), this,
                                          &MainWindow::identifyDisplays);
    output->addSeparator();
    m_liveOnLaunchAction = output->addAction(tr("Go live on launch"));
    m_liveOnLaunchAction->setCheckable(true);
    m_liveOnLaunchAction->setToolTip(tr("For unattended installs: open outputs automatically at startup "
                                        "and recover from a crash without asking."));
    m_liveOnLaunchAction->setChecked(settings.value(kSettingLiveOnLaunch, false).toBool());
    connect(m_liveOnLaunchAction, &QAction::toggled, this, [](bool on) { QSettings().setValue(kSettingLiveOnLaunch, on); });

    QMenu *audio = menuBar()->addMenu(tr("&Audio"));
    QMenu *devices = audio->addMenu(tr("Output device"));
    // Rebuilt when opened, so newly plugged-in interfaces show up.
    connect(devices, &QMenu::aboutToShow, this, [this, devices] {
        devices->clear();
        auto *group = new QActionGroup(devices);
        auto add = [&](const QString &label, const QByteArray &id) {
            QAction *a = devices->addAction(label, this, [this, id] { m_engine->setAudioDevice(id); });
            a->setCheckable(true);
            a->setChecked(m_engine->audioDeviceId() == id);
            group->addAction(a);
        };
        add(tr("System default (%1)").arg(QMediaDevices::defaultAudioOutput().description()), QByteArray());
        devices->addSeparator();
        for (const QAudioDevice &d : QMediaDevices::audioOutputs())
            add(d.description(), d.id());
    });
    audio->addAction(tr("Mute all"), QKeySequence(tr("Ctrl+Shift+M")), this, [this] {
        m_doc->setAudioMuted(!m_doc->project().audioMuted);
    });
    audio->addSeparator();
    audio->addAction(tr("Audio reactive..."), QKeySequence(tr("Ctrl+Shift+A")), this, &MainWindow::showAudioReactive);

    QMenu *control = menuBar()->addMenu(tr("&Control"));
    control->addAction(tr("OSC, MIDI && DMX..."), QKeySequence(tr("Ctrl+M")), this, &MainWindow::showControls);

    QMenu *view = menuBar()->addMenu(tr("&View"));
    view->addAction(tr("Reset input view"), this, [this] { m_inputView->resetView(); });
    view->addAction(tr("Reset output view"), this, [this] { m_outputView->resetView(); });

    QToolBar *tb = addToolBar(tr("Main"));
    tb->setObjectName(QStringLiteral("main"));
    tb->setToolButtonStyle(Qt::ToolButtonTextOnly);
    tb->addAction(newAct);
    tb->addAction(openAct);
    tb->addAction(saveAct);
    tb->addSeparator();
    tb->addAction(m_undoAction);
    tb->addAction(m_redoAction);
    tb->addSeparator();
    tb->addAction(addWall);
    tb->addAction(addScreen);
    tb->addAction(addSlice);
    tb->addAction(dup);
    tb->addAction(del);
    tb->addSeparator();
    tb->addAction(identify);
    tb->addAction(m_liveAction);
    if (auto *btn = qobject_cast<QToolButton *>(tb->widgetForAction(m_liveAction)))
        btn->setStyleSheet(QStringLiteral("QToolButton:checked { background: #c62828; color: white; font-weight: bold; }"));
}

void MainWindow::buildTree()
{
    m_tree = new QTreeWidget;
    m_tree->setHeaderLabels({tr("Item"), tr("Size")});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setStretchLastSection(false);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, &MainWindow::onTreeSelection);

    auto *dock = new QDockWidget(tr("Setup"));
    dock->setObjectName(QStringLiteral("setup"));
    dock->setWidget(m_tree);
    dock->setMinimumWidth(260);
    addDockWidget(Qt::LeftDockWidgetArea, dock);
}

void MainWindow::rebuildTree()
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

void MainWindow::syncTreeSelection()
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

void MainWindow::onTreeSelection()
{
    if (m_syncingTree || !m_tree->currentItem())
        return;
    const Selection s = selectionOf(m_tree->currentItem());
    if (s.kind != Selection::None)
        m_doc->select(s);
}

void MainWindow::refreshOutputScreenCombo()
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

void MainWindow::setLive(bool live)
{
    m_live = live;
    {
        QSignalBlocker block(m_liveAction);
        m_liveAction->setChecked(live);
    }
    syncOutputs();
}

void MainWindow::syncOutputs()
{
    const Project &p = m_doc->project();
    const QList<QScreen *> displays = QGuiApplication::screens();
    const int count = qMax(int(p.screens.size()), int(m_outputs.size()));
    m_outputs.resize(count);

    int opened = 0;
    for (int i = 0; i < count; ++i) {
        QString key;
        QScreen *target = nullptr;
        if (m_live && i < p.screens.size() && p.screens[i].enabled && !p.screens[i].display.isEmpty()) {
            const Screen &s = p.screens[i];
            if (s.display == QLatin1String("ndi") || s.display == QLatin1String("share")) {
                // Published by the render thread (NDI / Spout / Syphon); no window.
            } else if (s.display == QLatin1String("window")) {
                key = QStringLiteral("window");
            } else {
                for (QScreen *d : displays)
                    if (d->name() == s.display)
                        target = d;
                if (target)
                    key = s.display;
            }
        }

        OutputWindow *w = m_outputs[i].data();
        if (w && w->objectName() != key) {
            m_engine->removeOutput(w); // the render thread lets go of the swapchain first
            delete w;
            w = nullptr;
            m_outputs[i] = nullptr;
        }
        if (!key.isEmpty() && !w) {
            const Screen &s = p.screens[i];
            w = new OutputWindow(m_engine->surfaceType(), i);
            w->setObjectName(key);
            connect(w, &OutputWindow::stopRequested, this, [this] { setLive(false); }, Qt::QueuedConnection);
            if (target) {
                w->setFlags(Qt::Window | Qt::FramelessWindowHint);
                w->setScreen(target);
                w->setGeometry(target->geometry());
                w->showFullScreen();
            } else {
                w->resize(qMax(320, s.width / 2), qMax(180, s.height / 2));
                w->show();
            }
            m_engine->addOutput(w);
            m_outputs[i] = w;
        }
        if (w) {
            w->setScreenIndex(i);
            w->setTitle(tr("Display Hawk output - %1").arg(p.screens[i].name));
            ++opened;
        }
    }
    while (!m_outputs.isEmpty() && !m_outputs.last())
        m_outputs.removeLast();

    // NDI screens: published as "<Screen name>" from this machine while outputs are live.
    QVector<QPair<int, QString>> ndi;
    for (int i = 0; m_live && i < p.screens.size(); ++i)
        if (p.screens[i].enabled && p.screens[i].display == QLatin1String("ndi"))
            ndi.append({i, QStringLiteral("Display Hawk - %1").arg(p.screens[i].name)});
    if (ndi != m_ndiOutputs) {
        m_ndiOutputs = ndi;
        m_engine->setNdiOutputs(ndi);
    }
    opened += int(ndi.size());

    // Spout / Syphon screens, shared with other apps on this machine.
    QVector<QPair<int, QString>> share;
    for (int i = 0; m_live && i < p.screens.size(); ++i)
        if (p.screens[i].enabled && p.screens[i].display == QLatin1String("share"))
            share.append({i, QStringLiteral("Display Hawk - %1").arg(p.screens[i].name)});
    if (share != m_shareOutputs) {
        m_shareOutputs = share;
        m_engine->setShareOutputs(share);
    }
    opened += int(share.size());

    if (m_live && opened == 0)
        statusBar()->showMessage(tr("Live, but no screen has an output device assigned. "
                                    "Select a screen and choose a display, 'Window' or 'NDI'."), 6000);
}

void MainWindow::closeOutputs()
{
    for (const QPointer<OutputWindow> &w : std::as_const(m_outputs)) {
        if (!w)
            continue;
        m_engine->removeOutput(w);
        delete w.data();
    }
    m_outputs.clear();
}

void MainWindow::identifyDisplays()
{
    IdentifyOverlay::showAll(m_doc->project());
}

// ---------------------------------------------------------------- files, autosave, startup

QString MainWindow::autosavePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/autosave.dhawk");
}

void MainWindow::autosave()
{
    if (!m_autosaveDirty)
        return;
    m_autosaveDirty = false;
    if (m_doc->isModified())
        m_doc->writeAutosave(autosavePath());
    else
        QFile::remove(autosavePath());
}

void MainWindow::startup(const QStringList &args)
{
    QSettings settings;
    QString fileArg;
    bool live = m_liveOnLaunchAction->isChecked();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QLatin1String("--live"))
            live = true;
        else if (!args[i].startsWith(QLatin1String("--")))
            fileArg = args[i];
    }

    bool loaded = false;
    const QString recovery = autosavePath();
    if (QFile::exists(recovery)) {
        // An autosave only survives an unclean exit.
        const QString when = QFileInfo(recovery).lastModified().toString(QStringLiteral("ddd d MMM, HH:mm:ss"));
        const bool recover = live
            || QMessageBox::question(this, tr("Recover setup"),
                                     tr("Display Hawk did not shut down cleanly.\n\n"
                                        "Recover the unsaved setup from %1?").arg(when),
                                     QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes)
                   == QMessageBox::Yes;
        QString error;
        if (recover && m_doc->load(recovery, &error, true)) {
            loaded = true;
            statusBar()->showMessage(tr("Recovered unsaved setup from %1").arg(when), 8000);
            qInfo("Recovered autosave from %s", qPrintable(when));
        } else if (recover) {
            QMessageBox::warning(this, tr("Recovery failed"), error);
        }
        if (!recover)
            QFile::remove(recovery);
    }

    if (!loaded && !fileArg.isEmpty())
        loaded = openPath(fileArg);
    if (!loaded && m_reopenLastAction->isChecked()) {
        const QString last = settings.value(kSettingLastProject).toString();
        if (!last.isEmpty() && QFile::exists(last))
            loaded = openPath(last);
    }

    if (live)
        QTimer::singleShot(500, this, [this] { setLive(true); });
}

void MainWindow::rememberProject(const QString &path)
{
    QSettings().setValue(kSettingLastProject, QFileInfo(path).absoluteFilePath());
}

void MainWindow::updateTitle()
{
    const QString name = m_doc->filePath().isEmpty() ? tr("Untitled") : QFileInfo(m_doc->filePath()).fileName();
    setWindowTitle(QStringLiteral("%1%2 - Display Hawk").arg(name, m_doc->isModified() ? QStringLiteral("*") : QString()));
}

bool MainWindow::confirmDiscard()
{
    if (!m_doc->isModified())
        return true;
    const auto r = QMessageBox::question(this, tr("Unsaved changes"), tr("Save changes to the current setup?"),
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Save)
        return saveProject();
    return r == QMessageBox::Discard;
}

bool MainWindow::saveTo(const QString &path)
{
    QString error;
    if (!m_doc->save(path, &error)) {
        QMessageBox::warning(this, tr("Save failed"), error);
        return false;
    }
    rememberProject(path);
    QFile::remove(autosavePath());
    statusBar()->showMessage(tr("Saved %1").arg(path), 4000);
    return true;
}

bool MainWindow::openPath(const QString &path)
{
    QString error;
    if (!m_doc->load(path, &error)) {
        QMessageBox::warning(this, tr("Open failed"), tr("%1\n\n%2").arg(path, error));
        return false;
    }
    rememberProject(path);
    return true;
}

void MainWindow::openProject()
{
    if (!confirmDiscard())
        return;
    const QString path = QFileDialog::getOpenFileName(this, tr("Open setup"), QString(),
                                                      tr("Display Hawk setup (*.dhawk);;JSON (*.json)"));
    if (!path.isEmpty())
        openPath(path);
}

bool MainWindow::saveProject()
{
    if (m_doc->filePath().isEmpty())
        return saveProjectAs();
    return saveTo(m_doc->filePath());
}

bool MainWindow::saveProjectAs()
{
    const QString start = m_doc->filePath().isEmpty() ? QStringLiteral("setup.dhawk") : m_doc->filePath();
    const QString path = QFileDialog::getSaveFileName(this, tr("Save setup"), start, tr("Display Hawk setup (*.dhawk)"));
    return !path.isEmpty() && saveTo(path);
}

void MainWindow::showAudioReactive()
{
    if (!m_audioReactiveDialog)
        m_audioReactiveDialog = new AudioReactiveDialog(m_doc, m_audioReactive, this);
    m_audioReactiveDialog->show();
    m_audioReactiveDialog->raise();
}

void MainWindow::showControls()
{
    if (!m_controlDialog)
        m_controlDialog = new ControlDialog(m_doc, m_router, this);
    m_controlDialog->show();
    m_controlDialog->raise();
    m_controlDialog->activateWindow();
}

void MainWindow::exportReport()
{
    const QString start = m_doc->filePath().isEmpty() ? QString() : QFileInfo(m_doc->filePath()).absolutePath();
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Export setup report to"), start);
    if (dir.isEmpty())
        return;
    const QString base = m_doc->filePath().isEmpty() ? QStringLiteral("display-hawk")
                                                     : QFileInfo(m_doc->filePath()).completeBaseName();
    QString error;
    const QStringList files = ReportExporter::exportReport(m_doc->project(), dir, base, &error);
    if (files.isEmpty()) {
        QMessageBox::warning(this, tr("Export failed"), error);
        return;
    }
    QMessageBox box(QMessageBox::Information, tr("Setup report exported"),
                    tr("Wrote %n file(s) to %1:\n\n%2", nullptr, int(files.size()))
                        .arg(QDir::toNativeSeparators(dir),
                             [&files] {
                                 QStringList names;
                                 for (const QString &f : files)
                                     names << QFileInfo(f).fileName();
                                 return names.join(QLatin1Char('\n'));
                             }()),
                    QMessageBox::Ok, this);
    QPushButton *open = box.addButton(tr("Open folder"), QMessageBox::ActionRole);
    box.exec();
    if (box.clickedButton() == open)
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    if (!confirmDiscard()) {
        e->ignore();
        return;
    }
    m_autosaveTimer->stop();
    closeOutputs();
    QFile::remove(autosavePath()); // clean exit: nothing to recover
    e->accept();
}
