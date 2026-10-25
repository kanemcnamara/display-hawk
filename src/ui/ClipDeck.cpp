#include "ui/ClipDeck.h"
#include <QLineEdit>
#include <QTabBar>
#include <QInputDialog>
#include <QDoubleSpinBox>
#include <QComboBox>
#include "control/TempoClock.h"
#include <QTimer>
#include "render/RenderEngine.h"
#include "media/MediaSource.h"

#include "model/Document.h"
#include "media/Ndi.h"
#include "media/TextureShare.h"
#include "render/Patterns.h"

#include <QCameraDevice>
#include <QMediaDevices>

#include <QContextMenuEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

const QString kMediaFilter = QStringLiteral(
    "Media (*.mp4 *.mov *.mkv *.avi *.webm *.wmv *.m4v *.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff);;All files (*)");

QToolButton *toggleButton(const QString &text, const QString &tip, const QString &checkedColor)
{
    auto *b = new QToolButton;
    b->setText(text);
    b->setToolTip(tip);
    b->setCheckable(true);
    b->setFixedSize(24, 22);
    b->setStyleSheet(QStringLiteral("QToolButton:checked { background: %1; color: black; font-weight: bold; }")
                         .arg(checkedColor));
    return b;
}

// One layer's controls, aligned with its row in the grid.
class LayerRow : public QWidget
{
public:
    LayerRow(Document *doc, int layer)
        : m_doc(doc)
        , m_layer(layer)
    {
        setFixedHeight(ClipDeck::kRowH);
        setAutoFillBackground(true);
        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(6, 4, 6, 4);
        v->setSpacing(4);

        m_name = new QPushButton;
        m_name->setFlat(true);
        m_name->setStyleSheet(QStringLiteral("text-align: left; font-weight: bold;"));
        m_opacity = new QSlider(Qt::Horizontal);
        m_opacity->setRange(0, 100);
        m_opacity->setToolTip(tr("Layer opacity"));
        m_bypass = toggleButton(QStringLiteral("B"), tr("Bypass: hide this layer"), QStringLiteral("#e0a020"));
        m_solo = toggleButton(QStringLiteral("S"), tr("Solo: show only soloed layers"), QStringLiteral("#20b0e0"));
        auto *clear = new QToolButton;
        clear->setText(QStringLiteral("X"));
        clear->setToolTip(tr("Stop this layer"));
        clear->setFixedSize(24, 22);
        auto *buttons = new QHBoxLayout;
        buttons->setSpacing(3);
        buttons->addWidget(m_bypass);
        buttons->addWidget(m_solo);
        buttons->addWidget(clear);
        m_side = new QToolButton;
        m_side->setFixedSize(24, 22);
        m_side->setToolTip(tr("Crossfader side: none, A or B (click to change)"));
        buttons->addWidget(m_side);
        buttons->addStretch();
        QObject::connect(m_side, &QToolButton::clicked, this, [this] {
            Layer &l = m_doc->project().layers[m_layer];
            l.crossfade = CrossfadeSide((int(l.crossfade) + 1) % 3);
            m_doc->notifyChanged();
        });
        v->addWidget(m_name);
        v->addWidget(m_opacity);
        v->addLayout(buttons);

        QObject::connect(m_name, &QPushButton::clicked, this, [this] { m_doc->select(Selection::ofLayer(m_layer)); });
        QObject::connect(clear, &QToolButton::clicked, this, [this] { m_doc->clearLayer(m_layer); });
        auto playback = [this] {
            if (!m_refreshing)
                m_doc->setLayerPlayback(m_layer, m_opacity->value() / 100.0, m_bypass->isChecked(),
                                        m_solo->isChecked());
        };
        QObject::connect(m_opacity, &QSlider::valueChanged, this, playback);
        QObject::connect(m_bypass, &QToolButton::toggled, this, playback);
        QObject::connect(m_solo, &QToolButton::toggled, this, playback);
    }

    void refresh()
    {
        const Project &p = m_doc->project();
        if (m_layer >= p.layers.size())
            return;
        const Layer &l = p.layers[m_layer];
        m_refreshing = true;
        m_name->setText(QStringLiteral("%1   %2").arg(l.name, blendModeName(l.blend)));
        m_opacity->setValue(qRound(l.opacity * 100.0));
        m_bypass->setChecked(l.bypass);
        m_solo->setChecked(l.solo);
        static const char *const sides[] = {"\u2013", "A", "B"};
        m_side->setText(QString::fromUtf8(sides[int(l.crossfade)]));
        m_side->setStyleSheet(l.crossfade == CrossfadeSide::None ? QString()
                              : QStringLiteral("QToolButton { background: %1; color: black; font-weight: bold; }")
                                    .arg(l.crossfade == CrossfadeSide::A ? QStringLiteral("#ff8a50") : QStringLiteral("#50b8ff")));
        const LayerGroup *g = l.group.isEmpty() ? nullptr : p.groupById(l.group);
        m_name->setToolTip(g ? tr("Group: %1").arg(g->name) : QString());
        m_refreshing = false;
        const Selection s = m_doc->selection();
        const bool selected = (s.kind == Selection::Layer || s.kind == Selection::Clip) && s.layer == m_layer;
        QPalette pal = palette();
        pal.setColor(QPalette::Window, selected ? QColor(45, 55, 70) : QColor(36, 36, 40));
        setPalette(pal);
    }

private:
    Document *m_doc;
    int m_layer;
    QPushButton *m_name;
    QSlider *m_opacity;
    QToolButton *m_bypass;
    QToolButton *m_solo;
    QToolButton *m_side;
    bool m_refreshing = false;
};

} // namespace

// ---------------------------------------------------------------- ClipDeck

namespace {

// Four lamps, one per beat of the bar; the current beat is lit (the first one brighter).
class BeatLamps : public QWidget
{
public:
    explicit BeatLamps(int *beat, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_beat(beat)
    {
        setFixedSize(4 * 14 + 4, 20);
        setToolTip(QObject::tr("The beat: click to resync (the next beat is the start of a bar)"));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        for (int i = 0; i < 4; ++i) {
            const bool on = *m_beat == i;
            p.setPen(Qt::NoPen);
            p.setBrush(on ? (i == 0 ? QColor(255, 80, 60) : QColor(255, 190, 60)) : QColor(60, 60, 66));
            p.drawEllipse(QRectF(2 + i * 14, 4, 11, 11));
        }
    }

private:
    int *m_beat;
};

} // namespace

ClipDeck::ClipDeck(Document *doc, RenderEngine *engine, TempoClock *tempo, QWidget *parent)
    : QWidget(parent)
    , m_doc(doc)
{
    auto *top = new QHBoxLayout;
    top->setContentsMargins(6, 4, 6, 4);
    auto *addLayer = new QPushButton(tr("+ Layer"));
    auto *addColumn = new QPushButton(tr("+ Column"));
    m_master = new QSlider(Qt::Horizontal);
    m_master->setRange(0, 100);
    m_master->setFixedWidth(220);
    m_master->setToolTip(tr("Master fader: dims everything sent to the outputs"));
    m_blackout = new QPushButton(tr("BLACKOUT"));
    m_blackout->setCheckable(true);
    m_blackout->setShortcut(QKeySequence(tr("Ctrl+B")));
    m_blackout->setToolTip(tr("Send black to every output (Ctrl+B)"));
    m_blackout->setStyleSheet(QStringLiteral("QPushButton:checked { background: #c62828; color: white; font-weight: bold; }"));
    top->addWidget(addLayer);
    top->addWidget(addColumn);
    top->addSpacing(16);

    // Tempo: BPM, tap, resync, beat lamps, where the beat comes from, trigger quantizing.
    m_bpm = new QDoubleSpinBox;
    m_bpm->setRange(20, 300);
    m_bpm->setDecimals(1);
    m_bpm->setSuffix(tr(" BPM"));
    m_bpm->setToolTip(tr("Tempo for beat-synced clips, LFOs and quantized triggers"));
    auto *tap = new QPushButton(tr("TAP"));
    tap->setToolTip(tr("Tap on the beat to set the tempo (OSC /dh/tempo/tap, or map a MIDI control)"));
    tap->setAutoRepeat(false);
    auto *resync = new QPushButton(tr("Sync"));
    resync->setToolTip(tr("Now is the first beat of a bar"));
    m_beatLamps = new BeatLamps(&m_beatInBar);
    m_tempoSource = new QComboBox;
    for (int i = 0; i < 3; ++i)
        m_tempoSource->addItem(tempoSourceName(TempoSource(i)));
    m_tempoSource->setToolTip(tr("Where the tempo comes from"));
    m_quantize = new QComboBox;
    for (int i = 0; i < 3; ++i)
        m_quantize->addItem(triggerQuantizeName(TriggerQuantize(i)));
    m_quantize->setToolTip(tr("Clip and column triggers wait for the next beat or bar"));
    top->addWidget(m_bpm);
    top->addWidget(tap);
    top->addWidget(resync);
    top->addWidget(m_beatLamps);
    top->addWidget(m_tempoSource);
    top->addWidget(m_quantize);
    top->addStretch();
    // Tap fires on press, not release: taps land where the finger does.
    connect(tap, &QPushButton::pressed, tempo, [tempo] { tempo->tap(); });
    connect(resync, &QPushButton::clicked, tempo, [tempo] { tempo->resync(); });
    connect(tempo, &TempoClock::beat, this, [this](int beat) {
        m_beatInBar = beat;
        m_beatLamps->update();
    });
    connect(m_bpm, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (!m_refreshing)
            m_doc->setBpm(v);
    });
    connect(m_tempoSource, &QComboBox::activated, this, [this](int i) {
        m_doc->project().tempoSource = TempoSource(i);
        m_doc->notifyChanged();
    });
    connect(m_quantize, &QComboBox::activated, this, [this](int i) {
        m_doc->project().quantize = TriggerQuantize(i);
        m_doc->notifyChanged();
    });
    m_audioMaster = new QSlider(Qt::Horizontal);
    m_audioMaster->setRange(0, 100);
    m_audioMaster->setFixedWidth(140);
    m_audioMaster->setToolTip(tr("Audio master: level of all clip sound"));
    m_audioMute = new QPushButton(tr("MUTE"));
    m_audioMute->setCheckable(true);
    m_audioMute->setToolTip(tr("Mute all clip sound"));
    m_audioMute->setStyleSheet(QStringLiteral("QPushButton:checked { background: #e0a020; color: black; font-weight: bold; }"));
    top->addWidget(new QLabel(tr("Audio")));
    top->addWidget(m_audioMaster);
    top->addWidget(m_audioMute);
    top->addSpacing(16);
    m_crossfader = new QSlider(Qt::Horizontal);
    m_crossfader->setRange(0, 1000);
    m_crossfader->setFixedWidth(140);
    m_crossfader->setToolTip(tr("A/B crossfader: left plays the A layers, right the B layers, both in the middle "
                                "(OSC /dh/crossfader, or map a MIDI control)"));
    auto *labelA = new QLabel(QStringLiteral("A"));
    auto *labelB = new QLabel(QStringLiteral("B"));
    labelA->setStyleSheet(QStringLiteral("color: #ff8a50; font-weight: bold;"));
    labelB->setStyleSheet(QStringLiteral("color: #50b8ff; font-weight: bold;"));
    top->addWidget(labelA);
    top->addWidget(m_crossfader);
    top->addWidget(labelB);
    top->addSpacing(16);
    connect(m_crossfader, &QSlider::valueChanged, this, [this](int v) {
        if (!m_refreshing)
            m_doc->setCrossfader(v / 1000.0);
    });
    top->addWidget(new QLabel(tr("Master")));
    top->addWidget(m_master);
    top->addWidget(m_blackout);

    // Layer controls and the grid scroll together.
    auto *content = new QWidget;
    auto *h = new QHBoxLayout(content);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);
    auto *left = new QWidget;
    left->setFixedWidth(190);
    m_rowLayout = new QVBoxLayout(left);
    m_rowLayout->setContentsMargins(0, kHeaderH, 0, 0);
    m_rowLayout->setSpacing(0);
    m_rowLayout->addStretch();
    m_grid = new ClipGrid(doc, engine);
    connect(m_grid, &ClipGrid::midiLearnRequested, this, &ClipDeck::midiLearnRequested);
    h->addWidget(left, 0, Qt::AlignTop);
    h->addWidget(m_grid, 0, Qt::AlignTop);
    h->addStretch();

    auto *scroll = new QScrollArea;
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    // Decks: pages of columns. Double-click a tab to rename it; right-click to remove it.
    m_deckTabs = new QTabBar;
    m_deckTabs->setExpanding(false);
    m_deckTabs->setContextMenuPolicy(Qt::CustomContextMenu);
    auto *addDeck = new QToolButton;
    addDeck->setText(tr("+ Deck"));
    addDeck->setToolTip(tr("A new page of 8 empty columns"));
    auto *tabs = new QHBoxLayout;
    tabs->setContentsMargins(6, 0, 6, 0);
    tabs->addWidget(m_deckTabs);
    tabs->addWidget(addDeck);
    tabs->addStretch();
    connect(m_deckTabs, &QTabBar::currentChanged, this, [this](int i) {
        if (m_rebuildingTabs)
            return;
        const QVector<Deck> &decks = m_doc->project().decks;
        if (i >= 0 && i < decks.size())
            m_grid->setColumnRange(decks[i].firstColumn, decks[i].columnCount);
        else
            m_grid->setColumnRange(0, -1);
    });
    connect(addDeck, &QToolButton::clicked, this, [this] {
        Project &p = m_doc->project();
        if (p.decks.isEmpty()) // the columns so far become the first deck
            p.decks.append({tr("Deck 1"), 0, p.columns});
        const int first = p.columns;
        p.decks.append({tr("Deck %1").arg(p.decks.size() + 1), first, 8});
        for (int i = 0; i < 8; ++i)
            m_doc->addColumn();
        m_doc->notifyChanged(true);
        m_deckTabs->setCurrentIndex(int(p.decks.size()) - 1);
    });
    connect(m_deckTabs, &QTabBar::tabBarDoubleClicked, this, [this](int i) {
        QVector<Deck> &decks = m_doc->project().decks;
        if (i < 0 || i >= decks.size())
            return;
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Rename deck"), tr("Name"), QLineEdit::Normal, decks[i].name, &ok);
        if (ok && !name.isEmpty()) {
            decks[i].name = name;
            m_doc->notifyChanged(true);
        }
    });
    connect(m_deckTabs, &QTabBar::customContextMenuRequested, this, [this](const QPoint &pos) {
        const int i = m_deckTabs->tabAt(pos);
        QVector<Deck> &decks = m_doc->project().decks;
        if (i < 0 || i >= decks.size())
            return;
        QMenu menu;
        menu.addAction(tr("Remove deck (its columns stay, under All)"), this, [this, i] {
            m_doc->project().decks.removeAt(i);
            m_doc->notifyChanged(true);
        });
        menu.exec(m_deckTabs->mapToGlobal(pos));
    });
    connect(m_doc, &Document::structureChanged, this, &ClipDeck::rebuildDeckTabs);
    rebuildDeckTabs();

    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addLayout(top);
    v->addLayout(tabs);
    v->addWidget(scroll, 1);

    connect(addLayer, &QPushButton::clicked, m_doc, &Document::addLayer);
    connect(addColumn, &QPushButton::clicked, m_doc, &Document::addColumn);
    connect(m_master, &QSlider::valueChanged, this, [this](int v) {
        if (!m_refreshing)
            m_doc->setMaster(v / 100.0);
    });
    connect(m_blackout, &QPushButton::toggled, this, [this](bool on) {
        if (!m_refreshing)
            m_doc->setBlackout(on);
    });
    connect(m_audioMaster, &QSlider::valueChanged, this, [this](int v) {
        if (!m_refreshing)
            m_doc->setAudioMaster(v / 100.0);
    });
    connect(m_audioMute, &QPushButton::toggled, this, [this](bool on) {
        if (!m_refreshing)
            m_doc->setAudioMuted(on);
    });
    connect(m_doc, &Document::structureChanged, this, &ClipDeck::rebuildRows);
    connect(m_doc, &Document::changed, this, &ClipDeck::refresh);
    connect(m_doc, &Document::selectionChanged, this, &ClipDeck::refresh);
    rebuildRows();
}

void ClipDeck::rebuildRows()
{
    const int count = m_doc->project().layers.size();
    if (m_rows.size() != count) {
        qDeleteAll(m_rows);
        m_rows.clear();
        // Top layer first, like the composition stacks them.
        for (int r = 0; r < count; ++r) {
            auto *row = new LayerRow(m_doc, count - 1 - r);
            m_rowLayout->insertWidget(r, row);
            m_rows.append(row);
        }
    }
    m_grid->updateSize();
    refresh();
}

void ClipDeck::refresh()
{
    const Project &p = m_doc->project();
    m_refreshing = true;
    m_master->setValue(qRound(p.master * 100.0));
    m_blackout->setChecked(p.blackout);
    m_audioMaster->setValue(qRound(p.audioMaster * 100.0));
    m_audioMute->setChecked(p.audioMuted);
    m_crossfader->setValue(qRound(p.crossfader * 1000.0));
    if (!m_bpm->hasFocus() && qAbs(m_bpm->value() - p.bpm) > 0.049)
        m_bpm->setValue(p.bpm);
    m_bpm->setEnabled(p.tempoSource == TempoSource::Internal);
    m_tempoSource->setCurrentIndex(int(p.tempoSource));
    m_quantize->setCurrentIndex(int(p.quantize));
    m_refreshing = false;
    for (QWidget *w : std::as_const(m_rows))
        static_cast<LayerRow *>(w)->refresh();
    m_grid->update();
}

void ClipDeck::rebuildDeckTabs()
{
    const QVector<Deck> &decks = m_doc->project().decks;
    m_rebuildingTabs = true;
    const int current = m_deckTabs->currentIndex();
    while (m_deckTabs->count() > 0)
        m_deckTabs->removeTab(0);
    for (const Deck &d : decks)
        m_deckTabs->addTab(d.name);
    m_deckTabs->addTab(tr("All"));
    m_rebuildingTabs = false;
    m_deckTabs->setCurrentIndex(qBound(0, current, m_deckTabs->count() - 1));
    const int i = m_deckTabs->currentIndex();
    if (i >= 0 && i < decks.size())
        m_grid->setColumnRange(decks[i].firstColumn, decks[i].columnCount);
    else
        m_grid->setColumnRange(0, -1);
}

// ---------------------------------------------------------------- ClipGrid

ClipGrid::ClipGrid(Document *doc, RenderEngine *engine, QWidget *parent)
    : QWidget(parent)
    , m_doc(doc)
    , m_engine(engine)
{
    setAcceptDrops(true);
    setMouseTracking(true);
    updateSize();
    // Playing movies show their position under the thumbnail.
    auto *timer = new QTimer(this);
    timer->setInterval(100);
    connect(timer, &QTimer::timeout, this, [this] {
        for (const Layer &l : m_doc->project().layers)
            if (const Clip *c = l.activeClip(); c && m_engine->mediaFor(c->id)) {
                update();
                return;
            }
    });
    timer->start();
}

void ClipGrid::setColumnRange(int first, int count)
{
    m_first = qMax(0, first);
    m_count = count;
    updateSize();
}

int ClipGrid::lastColumn() const
{
    const int columns = m_doc->project().columns;
    return m_count < 0 ? columns : qMin(columns, m_first + m_count);
}

void ClipGrid::updateSize()
{
    const Project &p = m_doc->project();
    setFixedSize(qMax(0, lastColumn() - m_first) * ClipDeck::kCellW + 1, ClipDeck::kHeaderH + int(p.layers.size()) * ClipDeck::kRowH + 1);
    update();
}

QRect ClipGrid::cellRect(int layer, int column) const
{
    const int row = int(m_doc->project().layers.size()) - 1 - layer;
    return QRect((column - m_first) * ClipDeck::kCellW, ClipDeck::kHeaderH + row * ClipDeck::kRowH, ClipDeck::kCellW,
                 ClipDeck::kRowH);
}

QPair<int, int> ClipGrid::cellAt(const QPoint &pos) const
{
    const Project &p = m_doc->project();
    const int column = m_first + pos.x() / ClipDeck::kCellW;
    if (pos.x() < 0 || column >= lastColumn())
        return {-1, -1};
    if (pos.y() < ClipDeck::kHeaderH)
        return {-2, column};
    const int row = (pos.y() - ClipDeck::kHeaderH) / ClipDeck::kRowH;
    if (row < 0 || row >= p.layers.size())
        return {-1, -1};
    return {int(p.layers.size()) - 1 - row, column};
}

QPixmap ClipGrid::thumbnail(const Clip &clip, const Project &project)
{
    const QSize size(ClipDeck::kCellW - 8, ClipDeck::kRowH - 22);
    QJsonObject keyObj{{"src", int(clip.source)},
                       {"pattern", int(clip.pattern)},
                       {"path", clip.mediaPath},
                       {"input", clip.inputId},
                       {"color", clip.solidColor.name()},
                       {"spacing", clip.gridSpacing}};
    if (clip.source == ContentSource::TestPattern && clip.pattern == TestPattern::PanelMap) {
        keyObj["walls"] = project.toJson()["walls"];
        keyObj["comp"] = QStringLiteral("%1x%2").arg(project.compWidth).arg(project.compHeight);
    }
    const QByteArray key = QJsonDocument(keyObj).toJson(QJsonDocument::Compact);
    if (auto it = m_thumbs.constFind(key); it != m_thumbs.cend())
        return *it;

    QImage img;
    if (clip.isLive()) {
        // Live inputs: a badge rather than a frame (the picture changes all the time).
        QPixmap pm(size);
        pm.fill(QColor(30, 22, 22));
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        QFont f = p.font();
        f.setBold(true);
        f.setPixelSize(15);
        p.setFont(f);
        p.setBrush(QColor(220, 40, 40));
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRect(size.width() / 2 - 26, size.height() / 2 - 22, 52, 22), 4, 4);
        p.setPen(Qt::white);
        p.drawText(QRect(size.width() / 2 - 26, size.height() / 2 - 22, 52, 22), Qt::AlignCenter, QStringLiteral("LIVE"));
        f.setPixelSize(10);
        f.setBold(false);
        p.setFont(f);
        p.setPen(QColor(220, 200, 200));
        p.drawText(pm.rect().adjusted(4, 0, -4, -4), Qt::AlignHCenter | Qt::AlignBottom,
                   clip.source == ContentSource::Ndi     ? QStringLiteral("NDI")
                   : clip.source == ContentSource::Share ? TextureShare::technology()
                                                         : QStringLiteral("Capture"));
        p.end();
        m_thumbs.insert(key, pm);
        return pm;
    }
    if (clip.source == ContentSource::TestPattern) {
        // Generated at the real size (capped) so labels like the grid's resolution are truthful.
        const QSize genSize = project.compSize().boundedTo(QSize(3840, 2160));
        img = Patterns::generate(project, clip, genSize);
    } else {
        QImageReader reader(clip.mediaPath);
        reader.setAutoTransform(true);
        if (reader.canRead()) {
            const QSize full = reader.size();
            if (full.isValid())
                reader.setScaledSize(full.scaled(size, Qt::KeepAspectRatio));
            img = reader.read();
        }
    }

    QPixmap pm(size);
    pm.fill(QColor(20, 20, 22));
    QPainter p(&pm);
    if (!img.isNull()) {
        const QImage scaled = img.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        p.drawImage((size.width() - scaled.width()) / 2, (size.height() - scaled.height()) / 2, scaled);
    } else {
        // Video (or unreadable file): a film-strip placeholder with the file type.
        p.setPen(QColor(120, 120, 130));
        QFont f = p.font();
        f.setPixelSize(22);
        p.setFont(f);
        p.drawText(pm.rect().adjusted(0, -8, 0, -8), Qt::AlignCenter, QStringLiteral("â–¶"));
        f.setPixelSize(10);
        p.setFont(f);
        const QString ext = QFileInfo(clip.mediaPath).suffix().toUpper();
        p.drawText(pm.rect().adjusted(0, 0, 0, -4), Qt::AlignHCenter | Qt::AlignBottom,
                   QFileInfo::exists(clip.mediaPath) ? ext : tr("missing file"));
    }
    p.end();
    if (m_thumbs.size() > 300)
        m_thumbs.clear();
    m_thumbs.insert(key, pm);
    return pm;
}

void ClipGrid::paintEvent(QPaintEvent *)
{
    const Project &p = m_doc->project();
    const Selection sel = m_doc->selection();
    QPainter painter(this);
    painter.fillRect(rect(), QColor(28, 28, 31));

    // Column headers trigger whole columns.
    QFont small = font();
    small.setPixelSize(11);
    painter.setFont(small);
    for (int c = m_first; c < lastColumn(); ++c) {
        bool all = !p.layers.isEmpty();
        for (const Layer &l : p.layers)
            all = all && (l.activeColumn == c || (l.clips.value(c).isEmpty() && l.activeColumn == -1));
        const QRect hr((c - m_first) * ClipDeck::kCellW, 0, ClipDeck::kCellW - 1, ClipDeck::kHeaderH - 2);
        painter.fillRect(hr, all ? QColor(40, 90, 50) : QColor(44, 44, 50));
        painter.setPen(QColor(200, 200, 200));
        painter.drawText(hr, Qt::AlignCenter, QString::number(c + 1));
    }

    for (int li = 0; li < p.layers.size(); ++li) {
        const Layer &layer = p.layers[li];
        const bool dimmed = layer.bypass;
        for (int c = m_first; c < lastColumn(); ++c) {
            const QRect cell = cellRect(li, c).adjusted(2, 2, -2, -2);
            const Clip clip = layer.clips.value(c);
            const bool active = layer.activeColumn == c && !clip.isEmpty();
            const bool selected = sel.kind == Selection::Clip && sel.layer == li && sel.column == c;
            painter.fillRect(cell, QColor(34, 34, 38));
            if (!clip.isEmpty()) {
                const QPixmap thumb = thumbnail(clip, p);
                painter.setOpacity(dimmed ? 0.35 : 1.0);
                painter.drawPixmap(cell.x() + 2, cell.y() + 2, thumb);
                painter.setOpacity(1.0);
                const QRect label(cell.x() + 3, cell.bottom() - 16, cell.width() - 6, 16);
                painter.setPen(active ? QColor(140, 255, 160) : QColor(210, 210, 210));
                painter.drawText(label, Qt::AlignLeft | Qt::AlignVCenter,
                                 painter.fontMetrics().elidedText(clip.displayName(), Qt::ElideRight, label.width()));
            }
            if (active) {
                painter.setPen(QPen(QColor(60, 220, 90), 3));
                painter.drawRect(cell.adjusted(1, 1, -2, -2));
                const MediaSource *m = m_engine->mediaFor(clip.id);
                const double pos = m ? m->position() : -1.0;
                if (pos >= 0.0) { // progress bar, with the in/out range shaded
                    const QRect bar(cell.x() + 4, cell.bottom() - 21, cell.width() - 8, 4);
                    painter.fillRect(bar, QColor(0, 0, 0, 160));
                    painter.fillRect(QRect(bar.x() + int(bar.width() * clip.inPoint), bar.y(),
                                           int(bar.width() * (clip.outPoint - clip.inPoint)), bar.height()),
                                     QColor(70, 70, 80));
                    painter.fillRect(QRect(bar.x(), bar.y(), int(bar.width() * qBound(0.0, pos, 1.0)), bar.height()),
                                     QColor(60, 220, 90));
                }
            }
            if (selected) {
                painter.setPen(QPen(QColor(0, 170, 255), 1));
                painter.drawRect(cell.adjusted(0, 0, -1, -1));
            }
            if (m_dropCell == qMakePair(li, c)) {
                painter.fillRect(cell, QColor(0, 170, 255, 60));
                painter.setPen(QPen(QColor(0, 170, 255), 2, Qt::DashLine));
                painter.drawRect(cell.adjusted(1, 1, -2, -2));
            }
        }
    }
}

void ClipGrid::mousePressEvent(QMouseEvent *e)
{
    const auto cell = cellAt(e->pos());
    m_pressPos = e->pos();
    m_pressCell = cell;
    m_moving = false;
    if (e->button() != Qt::LeftButton)
        return;
    if (cell.first == -2) {
        m_doc->triggerColumn(cell.second);
        return;
    }
    if (cell.first < 0)
        return;
    m_doc->select(Selection::ofClip(cell.first, cell.second));
    // Trigger on press, like a hardware launcher.
    m_doc->triggerClip(cell.first, cell.second);
}

void ClipGrid::mouseMoveEvent(QMouseEvent *e)
{
    if (!(e->buttons() & Qt::LeftButton) || m_pressCell.first < 0)
        return;
    const Clip clip = m_doc->project().layers.value(m_pressCell.first).clips.value(m_pressCell.second);
    if (clip.isEmpty())
        return;
    if (!m_moving && (e->pos() - m_pressPos).manhattanLength() > 8)
        m_moving = true;
    if (m_moving) {
        const auto cell = cellAt(e->pos());
        m_dropCell = cell.first >= 0 ? cell : qMakePair(-1, -1);
        setCursor(Qt::ClosedHandCursor);
        update();
    }
}

void ClipGrid::mouseReleaseEvent(QMouseEvent *)
{
    if (m_moving && m_dropCell.first >= 0 && m_dropCell != m_pressCell)
        m_doc->moveClip(m_pressCell.first, m_pressCell.second, m_dropCell.first, m_dropCell.second);
    else if (!m_moving && m_pressCell.first >= 0)
        m_doc->releaseClip(m_pressCell.first, m_pressCell.second); // piano clips play while held
    m_moving = false;
    m_dropCell = {-1, -1};
    m_pressCell = {-1, -1};
    unsetCursor();
    update();
}

void ClipGrid::mouseDoubleClickEvent(QMouseEvent *e)
{
    const auto cell = cellAt(e->pos());
    if (cell.first < 0)
        return;
    if (m_doc->project().layers[cell.first].clips.value(cell.second).isEmpty())
        browseInto(cell.first, cell.second);
}

void ClipGrid::browseInto(int layer, int column)
{
    const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Load media"), QString(), kMediaFilter);
    for (int i = 0; i < paths.size(); ++i)
        m_doc->setClip(layer, column + i, Clip::fromMedia(paths[i]));
    if (!paths.isEmpty())
        m_doc->select(Selection::ofClip(layer, column));
}

void ClipGrid::contextMenuEvent(QContextMenuEvent *e)
{
    const auto cell = cellAt(e->pos());
    if (cell.first == -2) { // column header
        const int column = cell.second;
        QMenu menu(this);
        menu.addAction(tr("Trigger column %1").arg(column + 1), this, [this, column] { m_doc->triggerColumn(column); });
        menu.addAction(tr("MIDI learn: trigger column %1").arg(column + 1), this, [this, column] {
            emit midiLearnRequested(int(ControlAction::TriggerColumn), 0, column);
        });
        menu.exec(e->globalPos());
        return;
    }
    if (cell.first < 0)
        return;
    const int layer = cell.first, column = cell.second;
    m_doc->select(Selection::ofClip(layer, column));
    const Clip clip = m_doc->project().layers[layer].clips.value(column);

    QMenu menu(this);
    if (!clip.isEmpty())
        menu.addAction(tr("Trigger"), this, [this, layer, column] { m_doc->triggerClip(layer, column); });
    menu.addAction(tr("Load media..."), this, [this, layer, column] { browseInto(layer, column); });
    QMenu *capture = menu.addMenu(tr("Capture device"));
    const QList<QCameraDevice> cameras = QMediaDevices::videoInputs();
    for (const QCameraDevice &cam : cameras) {
        const QString id = QString::fromUtf8(cam.id()), name = cam.description();
        capture->addAction(name, this, [this, layer, column, id, name] { m_doc->setClip(layer, column, Clip::fromCapture(id, name)); });
    }
    if (cameras.isEmpty())
        capture->addAction(tr("No capture devices found"))->setEnabled(false);
    QMenu *ndi = menu.addMenu(tr("NDI source"));
    const QStringList ndiSources = Ndi::sources();
    for (const QString &name : ndiSources)
        ndi->addAction(name, this, [this, layer, column, name] { m_doc->setClip(layer, column, Clip::fromNdi(name)); });
    if (ndiSources.isEmpty())
        ndi->addAction(Ndi::available() ? tr("No NDI sources found yet") : Ndi::runtimeStatus())->setEnabled(false);
    if (!TextureShare::technology().isEmpty()) {
        QMenu *share = menu.addMenu(tr("%1 source").arg(TextureShare::technology()));
        const QStringList shared = TextureShare::sources();
        for (const QString &name : shared)
            share->addAction(name, this, [this, layer, column, name] { m_doc->setClip(layer, column, Clip::fromShare(name)); });
        if (shared.isEmpty())
            share->addAction(tr("No %1 sources found").arg(TextureShare::technology()))->setEnabled(false);
    }
    QMenu *patterns = menu.addMenu(tr("Test pattern"));
    for (int i = 0; i < kTestPatternCount; ++i) {
        patterns->addAction(testPatternName(TestPattern(i)), this, [this, layer, column, i] {
            m_doc->setClip(layer, column, Clip::fromPattern(TestPattern(i)));
        });
    }
    if (!clip.isEmpty()) {
        menu.addAction(tr("MIDI learn: trigger this clip"), this, [this, layer, column] {
            emit midiLearnRequested(int(ControlAction::TriggerClip), layer, column);
        });
        menu.addSeparator();
        menu.addAction(tr("Duplicate"), m_doc, &Document::duplicateSelection);
        menu.addAction(tr("Clear slot"), this, [this, layer, column] { m_doc->setClip(layer, column, Clip()); });
    }
    menu.exec(e->globalPos());
}

void ClipGrid::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void ClipGrid::dragMoveEvent(QDragMoveEvent *e)
{
    const auto cell = cellAt(e->position().toPoint());
    m_dropCell = cell.first >= 0 ? cell : qMakePair(-1, -1);
    update();
    if (m_dropCell.first >= 0)
        e->acceptProposedAction();
    else
        e->ignore();
}

void ClipGrid::dragLeaveEvent(QDragLeaveEvent *)
{
    m_dropCell = {-1, -1};
    update();
}

void ClipGrid::dropEvent(QDropEvent *e)
{
    const auto cell = cellAt(e->position().toPoint());
    m_dropCell = {-1, -1};
    update();
    if (cell.first < 0)
        return;
    // Several files fill consecutive slots, adding columns as needed.
    int column = cell.second;
    for (const QUrl &url : e->mimeData()->urls()) {
        const QString path = url.toLocalFile();
        if (path.isEmpty() || QFileInfo(path).isDir())
            continue;
        m_doc->setClip(cell.first, column++, Clip::fromMedia(path));
    }
    m_doc->select(Selection::ofClip(cell.first, cell.second));
    e->acceptProposedAction();
}
