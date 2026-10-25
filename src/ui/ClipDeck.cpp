#include "ui/ClipDeck.h"

#include "model/Document.h"
#include "render/Patterns.h"

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
        buttons->addStretch();
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
    bool m_refreshing = false;
};

} // namespace

// ---------------------------------------------------------------- ClipDeck

ClipDeck::ClipDeck(Document *doc, QWidget *parent)
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
    top->addStretch();
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
    m_grid = new ClipGrid(doc);
    h->addWidget(left, 0, Qt::AlignTop);
    h->addWidget(m_grid, 0, Qt::AlignTop);
    h->addStretch();

    auto *scroll = new QScrollArea;
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addLayout(top);
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
    m_refreshing = false;
    for (QWidget *w : std::as_const(m_rows))
        static_cast<LayerRow *>(w)->refresh();
    m_grid->update();
}

// ---------------------------------------------------------------- ClipGrid

ClipGrid::ClipGrid(Document *doc, QWidget *parent)
    : QWidget(parent)
    , m_doc(doc)
{
    setAcceptDrops(true);
    setMouseTracking(true);
    updateSize();
}

void ClipGrid::updateSize()
{
    const Project &p = m_doc->project();
    setFixedSize(p.columns * ClipDeck::kCellW + 1, ClipDeck::kHeaderH + int(p.layers.size()) * ClipDeck::kRowH + 1);
    update();
}

QRect ClipGrid::cellRect(int layer, int column) const
{
    const int row = int(m_doc->project().layers.size()) - 1 - layer;
    return QRect(column * ClipDeck::kCellW, ClipDeck::kHeaderH + row * ClipDeck::kRowH, ClipDeck::kCellW,
                 ClipDeck::kRowH);
}

QPair<int, int> ClipGrid::cellAt(const QPoint &pos) const
{
    const Project &p = m_doc->project();
    const int column = pos.x() / ClipDeck::kCellW;
    if (pos.x() < 0 || column >= p.columns)
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
    for (int c = 0; c < p.columns; ++c) {
        bool all = !p.layers.isEmpty();
        for (const Layer &l : p.layers)
            all = all && (l.activeColumn == c || (l.clips.value(c).isEmpty() && l.activeColumn == -1));
        const QRect hr(c * ClipDeck::kCellW, 0, ClipDeck::kCellW - 1, ClipDeck::kHeaderH - 2);
        painter.fillRect(hr, all ? QColor(40, 90, 50) : QColor(44, 44, 50));
        painter.setPen(QColor(200, 200, 200));
        painter.drawText(hr, Qt::AlignCenter, QString::number(c + 1));
    }

    for (int li = 0; li < p.layers.size(); ++li) {
        const Layer &layer = p.layers[li];
        const bool dimmed = layer.bypass;
        for (int c = 0; c < p.columns; ++c) {
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
    if (cell.first < 0)
        return;
    const int layer = cell.first, column = cell.second;
    m_doc->select(Selection::ofClip(layer, column));
    const Clip clip = m_doc->project().layers[layer].clips.value(column);

    QMenu menu(this);
    if (!clip.isEmpty())
        menu.addAction(tr("Trigger"), this, [this, layer, column] { m_doc->triggerClip(layer, column); });
    menu.addAction(tr("Load media..."), this, [this, layer, column] { browseInto(layer, column); });
    QMenu *patterns = menu.addMenu(tr("Test pattern"));
    for (int i = 0; i < kTestPatternCount; ++i) {
        patterns->addAction(testPatternName(TestPattern(i)), this, [this, layer, column, i] {
            m_doc->setClip(layer, column, Clip::fromPattern(TestPattern(i)));
        });
    }
    if (!clip.isEmpty()) {
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
