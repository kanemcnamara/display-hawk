#include "ui/PropertyPanel.h"

#include "media/MediaSource.h"
#include "model/Document.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QtMath>

namespace {

enum Page { CompositionPage, WallPage, ScreenPage, SlicePage };

QSpinBox *makeSpin(int min, int max, const QString &suffix = QStringLiteral(" px"))
{
    auto *s = new QSpinBox;
    s->setRange(min, max);
    s->setSuffix(suffix);
    s->setAccelerated(true);
    return s;
}

QDoubleSpinBox *makeDouble(double min, double max, int decimals, const QString &suffix)
{
    auto *s = new QDoubleSpinBox;
    s->setRange(min, max);
    s->setDecimals(decimals);
    s->setSuffix(suffix);
    return s;
}

void setButtonColor(QPushButton *b, const QColor &c)
{
    b->setText(c.name().toUpper());
    b->setStyleSheet(QStringLiteral("QPushButton { background: %1; color: %2; }")
                         .arg(c.name(), c.lightness() > 128 ? QStringLiteral("black") : QStringLiteral("white")));
}

QWidget *row(std::initializer_list<QWidget *> widgets)
{
    auto *w = new QWidget;
    auto *l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    for (QWidget *c : widgets)
        l->addWidget(c);
    return w;
}

template <typename T>
void setValue(T *w, decltype(w->value()) v)
{
    if (w->value() != v)
        w->setValue(v);
}

void setText(QLineEdit *e, const QString &t)
{
    if (e->text() != t)
        e->setText(t);
}

struct SizePreset {
    const char *label;
    int w, h;
};

const SizePreset kSizePresets[] = {
    {"1920 x 1080 (HD)", 1920, 1080}, {"3840 x 2160 (UHD)", 3840, 2160}, {"2560 x 1440", 2560, 1440},
    {"1280 x 720", 1280, 720},        {"4096 x 2160 (DCI 4K)", 4096, 2160}, {"1920 x 1200", 1920, 1200},
    {"7680 x 1080 (4x HD wide)", 7680, 1080},
};

struct PanelPreset {
    const char *label;
    int pxW, pxH;
    double mmW, mmH;
};

const PanelPreset kPanelPresets[] = {
    {"P1.9  500 x 500 mm (256 x 256)", 256, 256, 500, 500},
    {"P2.6  500 x 500 mm (192 x 192)", 192, 192, 500, 500},
    {"P2.6  500 x 1000 mm (192 x 384)", 192, 384, 500, 1000},
    {"P2.9  500 x 500 mm (168 x 168)", 168, 168, 500, 500},
    {"P2.9  500 x 1000 mm (168 x 336)", 168, 336, 500, 1000},
    {"P3.9  500 x 500 mm (128 x 128)", 128, 128, 500, 500},
    {"P3.9  500 x 1000 mm (128 x 256)", 128, 256, 500, 1000},
    {"P4.8  500 x 500 mm (104 x 104)", 104, 104, 500, 500},
    {"P5.9  500 x 500 mm (84 x 84)", 84, 84, 500, 500},
    {"P1.5  600 x 337.5 mm (384 x 216)", 384, 216, 600, 337.5},
};

void fillSizePresets(QComboBox *c)
{
    c->addItem(QObject::tr("Custom"));
    for (const SizePreset &p : kSizePresets)
        c->addItem(QString::fromLatin1(p.label), QSize(p.w, p.h));
}

void syncSizePreset(QComboBox *c, int w, int h)
{
    const int i = c->findData(QSize(w, h));
    c->setCurrentIndex(i < 0 ? 0 : i);
}

// A 1G port on common LED processors carries roughly 650k pixels at 60 Hz, 8-bit.
constexpr double kPixelsPerGigPort = 655360.0;

} // namespace

PropertyPanel::PropertyPanel(Document *doc, MediaSource *media, QWidget *parent)
    : QWidget(parent)
    , m_doc(doc)
    , m_media(media)
{
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_stack = new QStackedWidget;
    m_stack->addWidget(buildCompositionPage());
    m_stack->addWidget(buildWallPage());
    m_stack->addWidget(buildScreenPage());
    m_stack->addWidget(buildSlicePage());
    scroll->setWidget(m_stack);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);

    connect(m_doc, &Document::changed, this, &PropertyPanel::refresh);
    connect(m_doc, &Document::selectionChanged, this, &PropertyPanel::refresh);
    connect(m_media, &MediaSource::stateChanged, this, &PropertyPanel::refresh);
    connect(qApp, &QGuiApplication::screenAdded, this, &PropertyPanel::refreshDisplays);
    connect(qApp, &QGuiApplication::screenRemoved, this, &PropertyPanel::refreshDisplays);
    refreshDisplays();
    refresh();
}

// ---------------------------------------------------------------- Composition

QWidget *PropertyPanel::buildCompositionPage()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);

    auto *comp = new QGroupBox(tr("Composition"));
    auto *cf = new QFormLayout(comp);
    m_compPreset = new QComboBox;
    fillSizePresets(m_compPreset);
    m_compW = makeSpin(1, 16384);
    m_compH = makeSpin(1, 16384);
    m_compBg = new QPushButton;
    auto *fitWalls = new QPushButton(tr("Fit composition to LED walls"));
    fitWalls->setToolTip(tr("Resize the composition to the bounding box of all walls and move them to 0,0"));
    cf->addRow(tr("Preset"), m_compPreset);
    cf->addRow(tr("Width"), m_compW);
    cf->addRow(tr("Height"), m_compH);
    cf->addRow(tr("Background"), m_compBg);
    cf->addRow(fitWalls);
    v->addWidget(comp);

    auto *content = new QGroupBox(tr("Content"));
    auto *ctf = new QFormLayout(content);
    m_source = new QComboBox;
    m_source->addItems({tr("Test pattern"), tr("Media file")});
    m_pattern = new QComboBox;
    for (int i = 0; i <= int(TestPattern::Solid); ++i)
        m_pattern->addItem(testPatternName(TestPattern(i)));
    m_gridSpacing = makeSpin(4, 2048);
    m_solidColor = new QPushButton;
    m_mediaPath = new QLineEdit;
    m_mediaPath->setReadOnly(true);
    auto *browse = new QPushButton(tr("Browse..."));
    auto *play = new QPushButton(tr("Play"));
    auto *pause = new QPushButton(tr("Pause"));
    auto *restart = new QPushButton(tr("Restart"));
    m_mediaStatus = new QLabel;
    m_mediaStatus->setWordWrap(true);
    m_loop = new QCheckBox(tr("Loop"));
    m_scale = new QComboBox;
    for (int i = 0; i <= int(ScaleMode::Native); ++i)
        m_scale->addItem(scaleModeName(ScaleMode(i)));
    m_target = new QComboBox;
    m_opacity = makeDouble(0, 100, 0, QStringLiteral(" %"));
    ctf->addRow(tr("Source"), m_source);
    ctf->addRow(tr("Pattern"), m_pattern);
    ctf->addRow(tr("Grid size"), m_gridSpacing);
    ctf->addRow(tr("Solid colour"), m_solidColor);
    ctf->addRow(tr("Media"), row({m_mediaPath, browse}));
    ctf->addRow(QString(), row({play, pause, restart}));
    ctf->addRow(QString(), m_loop);
    ctf->addRow(QString(), m_mediaStatus);
    ctf->addRow(tr("Scale"), m_scale);
    ctf->addRow(tr("Fit into"), m_target);
    ctf->addRow(tr("Opacity"), m_opacity);
    v->addWidget(content);

    auto *overlay = new QGroupBox(tr("LED panel overlay"));
    auto *of = new QFormLayout(overlay);
    m_overlay = new QCheckBox(tr("Show panel outlines and numbers on top of content"));
    m_overlayOpacity = makeDouble(0, 100, 0, QStringLiteral(" %"));
    of->addRow(m_overlay);
    of->addRow(tr("Opacity"), m_overlayOpacity);
    v->addWidget(overlay);
    v->addStretch();

    Project &p = m_doc->project();
    connect(m_compPreset, &QComboBox::activated, this, [this, &p](int i) {
        const QSize s = m_compPreset->itemData(i).toSize();
        if (s.isEmpty())
            return;
        p.compWidth = s.width();
        p.compHeight = s.height();
        m_doc->notifyChanged();
    });
    connect(m_compW, &QSpinBox::valueChanged, this, [this, &p](int v) {
        if (m_refreshing) return;
        p.compWidth = v;
        m_doc->notifyChanged();
    });
    connect(m_compH, &QSpinBox::valueChanged, this, [this, &p](int v) {
        if (m_refreshing) return;
        p.compHeight = v;
        m_doc->notifyChanged();
    });
    connect(m_compBg, &QPushButton::clicked, this, [this, &p] {
        const QColor c = QColorDialog::getColor(p.background, this, tr("Background colour"));
        if (c.isValid()) {
            p.background = c;
            m_doc->notifyChanged();
        }
    });
    connect(fitWalls, &QPushButton::clicked, m_doc, &Document::fitCompositionToWalls);

    connect(m_source, &QComboBox::activated, this, [this, &p](int i) {
        p.content.source = ContentSource(i);
        m_doc->notifyMediaChanged();
    });
    connect(m_pattern, &QComboBox::activated, this, [this, &p](int i) {
        p.content.pattern = TestPattern(i);
        p.content.source = ContentSource::TestPattern;
        m_doc->notifyMediaChanged();
    });
    connect(m_gridSpacing, &QSpinBox::valueChanged, this, [this, &p](int v) {
        if (m_refreshing) return;
        p.content.gridSpacing = v;
        m_doc->notifyChanged();
    });
    connect(m_solidColor, &QPushButton::clicked, this, [this, &p] {
        const QColor c = QColorDialog::getColor(p.content.solidColor, this, tr("Solid colour"));
        if (c.isValid()) {
            p.content.solidColor = c;
            m_doc->notifyChanged();
        }
    });
    connect(browse, &QPushButton::clicked, this, &PropertyPanel::browseMedia);
    connect(play, &QPushButton::clicked, m_media, &MediaSource::play);
    connect(pause, &QPushButton::clicked, m_media, &MediaSource::pause);
    connect(restart, &QPushButton::clicked, m_media, &MediaSource::restart);
    connect(m_loop, &QCheckBox::toggled, this, [this, &p](bool on) {
        if (m_refreshing) return;
        p.content.loop = on;
        m_media->setLoop(on);
        m_doc->notifyChanged();
    });
    connect(m_scale, &QComboBox::activated, this, [this, &p](int i) {
        p.content.scale = ScaleMode(i);
        m_doc->notifyChanged();
    });
    connect(m_target, &QComboBox::activated, this, [this, &p](int i) {
        p.content.targetWall = m_target->itemData(i).toInt();
        m_doc->notifyChanged();
    });
    connect(m_opacity, &QDoubleSpinBox::valueChanged, this, [this, &p](double v) {
        if (m_refreshing) return;
        p.content.opacity = v / 100.0;
        m_doc->notifyChanged();
    });
    connect(m_overlay, &QCheckBox::toggled, this, [this, &p](bool on) {
        if (m_refreshing) return;
        p.panelOverlay = on;
        m_doc->notifyChanged();
    });
    connect(m_overlayOpacity, &QDoubleSpinBox::valueChanged, this, [this, &p](double v) {
        if (m_refreshing) return;
        p.overlayOpacity = v / 100.0;
        m_doc->notifyChanged();
    });
    return page;
}

void PropertyPanel::browseMedia()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open media"), QFileInfo(m_doc->project().content.mediaPath).absolutePath(),
        tr("Media (*.mp4 *.mov *.mkv *.avi *.webm *.wmv *.m4v *.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff);;All files (*)"));
    if (path.isEmpty())
        return;
    Content &c = m_doc->project().content;
    c.mediaPath = path;
    c.source = ContentSource::Media;
    m_doc->notifyMediaChanged();
}

void PropertyPanel::refreshComposition()
{
    const Project &p = m_doc->project();
    const Content &c = p.content;
    syncSizePreset(m_compPreset, p.compWidth, p.compHeight);
    setValue(m_compW, p.compWidth);
    setValue(m_compH, p.compHeight);
    setButtonColor(m_compBg, p.background);
    m_source->setCurrentIndex(int(c.source));
    m_pattern->setCurrentIndex(int(c.pattern));
    setValue(m_gridSpacing, c.gridSpacing);
    setButtonColor(m_solidColor, c.solidColor);
    setText(m_mediaPath, QFileInfo(c.mediaPath).fileName());
    m_mediaPath->setToolTip(c.mediaPath);
    m_loop->setChecked(c.loop);
    m_scale->setCurrentIndex(int(c.scale));
    setValue(m_opacity, qRound(c.opacity * 100.0));
    m_overlay->setChecked(p.panelOverlay);
    setValue(m_overlayOpacity, qRound(p.overlayOpacity * 100.0));

    m_target->clear();
    m_target->addItem(tr("Whole composition"), -1);
    for (int i = 0; i < p.walls.size(); ++i)
        m_target->addItem(p.walls[i].name, i);
    m_target->setCurrentIndex(qMax(0, m_target->findData(c.targetWall)));

    const bool media = c.source == ContentSource::Media;
    m_pattern->setEnabled(!media);
    m_gridSpacing->setEnabled(!media && (c.pattern == TestPattern::Grid || c.pattern == TestPattern::Checker));
    m_solidColor->setEnabled(!media && c.pattern == TestPattern::Solid);
    m_scale->setEnabled(media);
    QString status;
    if (media) {
        if (!m_media->errorString().isEmpty())
            status = tr("Error: %1").arg(m_media->errorString());
        else if (m_media->isVideo())
            status = m_media->isPlaying() ? tr("Video playing") : tr("Video paused");
        else if (m_media->isOpen())
            status = tr("Still image");
        else
            status = tr("No media loaded");
    }
    m_mediaStatus->setText(status);
}

// ---------------------------------------------------------------- LED wall

QWidget *PropertyPanel::buildWallPage()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);

    auto *box = new QGroupBox(tr("LED wall"));
    auto *f = new QFormLayout(box);
    m_wallName = new QLineEdit;
    m_wallPreset = new QComboBox;
    m_wallPreset->addItem(tr("Custom panel"));
    for (const PanelPreset &pp : kPanelPresets)
        m_wallPreset->addItem(QString::fromLatin1(pp.label));
    m_panelPxW = makeSpin(1, 4096);
    m_panelPxH = makeSpin(1, 4096);
    m_panelMmW = makeDouble(1, 10000, 1, QStringLiteral(" mm"));
    m_panelMmH = makeDouble(1, 10000, 1, QStringLiteral(" mm"));
    m_cols = makeSpin(1, 500, QString());
    m_rows = makeSpin(1, 500, QString());
    m_wallX = makeSpin(-100000, 100000);
    m_wallY = makeSpin(-100000, 100000);
    m_wallColor = new QPushButton;
    f->addRow(tr("Name"), m_wallName);
    f->addRow(tr("Panel type"), m_wallPreset);
    f->addRow(tr("Panel pixels"), row({m_panelPxW, new QLabel(QStringLiteral("x")), m_panelPxH}));
    f->addRow(tr("Panel size"), row({m_panelMmW, new QLabel(QStringLiteral("x")), m_panelMmH}));
    f->addRow(tr("Panels wide"), m_cols);
    f->addRow(tr("Panels high"), m_rows);
    f->addRow(tr("Position X"), m_wallX);
    f->addRow(tr("Position Y"), m_wallY);
    f->addRow(tr("Colour"), m_wallColor);
    v->addWidget(box);

    auto *infoBox = new QGroupBox(tr("Wall specs"));
    auto *il = new QVBoxLayout(infoBox);
    m_wallInfo = new QLabel;
    m_wallInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    il->addWidget(m_wallInfo);
    v->addWidget(infoBox);

    auto *mapBox = new QGroupBox(tr("Map"));
    auto *mf = new QFormLayout(mapBox);
    m_wallScreen = new QComboBox;
    auto *createSlice = new QPushButton(tr("Create 1:1 slice on screen"));
    auto *fillContent = new QPushButton(tr("Fit content into this wall"));
    mf->addRow(tr("Screen"), m_wallScreen);
    mf->addRow(createSlice);
    mf->addRow(fillContent);
    v->addWidget(mapBox);
    v->addStretch();

    connect(m_wallName, &QLineEdit::textEdited, this, [this](const QString &t) {
        editWall([&](LedWall &w) { w.name = t; });
    });
    connect(m_wallPreset, &QComboBox::activated, this, [this](int i) {
        if (i <= 0)
            return;
        const PanelPreset &pp = kPanelPresets[i - 1];
        editWall([&](LedWall &w) {
            w.panelPxW = pp.pxW;
            w.panelPxH = pp.pxH;
            w.panelMmW = pp.mmW;
            w.panelMmH = pp.mmH;
        });
    });
    auto intField = [this](QSpinBox *s, int LedWall::*field) {
        connect(s, &QSpinBox::valueChanged, this, [this, field](int v) {
            if (!m_refreshing)
                editWall([&](LedWall &w) { w.*field = v; });
        });
    };
    intField(m_panelPxW, &LedWall::panelPxW);
    intField(m_panelPxH, &LedWall::panelPxH);
    intField(m_cols, &LedWall::cols);
    intField(m_rows, &LedWall::rows);
    intField(m_wallX, &LedWall::x);
    intField(m_wallY, &LedWall::y);
    auto mmField = [this](QDoubleSpinBox *s, double LedWall::*field) {
        connect(s, &QDoubleSpinBox::valueChanged, this, [this, field](double v) {
            if (!m_refreshing)
                editWall([&](LedWall &w) { w.*field = v; });
        });
    };
    mmField(m_panelMmW, &LedWall::panelMmW);
    mmField(m_panelMmH, &LedWall::panelMmH);
    connect(m_wallColor, &QPushButton::clicked, this, [this] {
        const LedWall *w = m_doc->selectedWall();
        if (!w)
            return;
        const QColor c = QColorDialog::getColor(w->color, this, tr("Wall colour"));
        if (c.isValid())
            editWall([&](LedWall &wall) { wall.color = c; });
    });
    connect(createSlice, &QPushButton::clicked, this, [this] {
        const Selection s = m_doc->selection();
        if (s.kind == Selection::Wall)
            m_doc->createSliceForWall(s.wall, m_wallScreen->currentData().toInt());
    });
    connect(fillContent, &QPushButton::clicked, this, [this] {
        const Selection s = m_doc->selection();
        if (s.kind != Selection::Wall)
            return;
        m_doc->project().content.targetWall = s.wall;
        m_doc->notifyChanged();
    });
    return page;
}

void PropertyPanel::editWall(const std::function<void(LedWall &)> &fn)
{
    const Selection s = m_doc->selection();
    if (s.kind != Selection::Wall)
        return;
    LedWall w = m_doc->project().walls[s.wall];
    const QString oldName = w.name;
    fn(w);
    m_doc->applyWall(s.wall, w);
}

void PropertyPanel::refreshWall()
{
    const LedWall *w = m_doc->selectedWall();
    if (!w)
        return;
    setText(m_wallName, w->name);
    setValue(m_panelPxW, w->panelPxW);
    setValue(m_panelPxH, w->panelPxH);
    setValue(m_panelMmW, w->panelMmW);
    setValue(m_panelMmH, w->panelMmH);
    setValue(m_cols, w->cols);
    setValue(m_rows, w->rows);
    setValue(m_wallX, w->x);
    setValue(m_wallY, w->y);
    setButtonColor(m_wallColor, w->color);

    int preset = 0;
    for (int i = 0; i < int(std::size(kPanelPresets)); ++i) {
        const PanelPreset &pp = kPanelPresets[i];
        if (pp.pxW == w->panelPxW && pp.pxH == w->panelPxH && qFuzzyCompare(pp.mmW, w->panelMmW)
            && qFuzzyCompare(pp.mmH, w->panelMmH))
            preset = i + 1;
    }
    m_wallPreset->setCurrentIndex(preset);

    const double aspect = double(w->pixelWidth()) / w->pixelHeight();
    const double pitchV = w->panelPxH > 0 ? w->panelMmH / w->panelPxH : 0.0;
    const int ports = qCeil(w->totalPixels() / kPixelsPerGigPort);
    QString pitch = QStringLiteral("%1 mm").arg(w->pitchMm(), 0, 'f', 2);
    if (qAbs(pitchV - w->pitchMm()) > 0.01)
        pitch += QStringLiteral(" x %1 mm").arg(pitchV, 0, 'f', 2);
    const QString diag = QString::number(qSqrt(w->widthM() * w->widthM() + w->heightM() * w->heightM()), 'f', 2);
    m_wallInfo->setText(
        tr("<table cellspacing=3>"
           "<tr><td>Resolution</td><td><b>%1 x %2 px</b></td></tr>"
           "<tr><td>Pixel pitch</td><td>%3</td></tr>"
           "<tr><td>Physical size</td><td>%4 x %5 m (%6 m diagonal)</td></tr>"
           "<tr><td>Area</td><td>%7 m²</td></tr>"
           "<tr><td>Aspect ratio</td><td>%8 : 1</td></tr>"
           "<tr><td>Panels</td><td>%9</td></tr>"
           "<tr><td>Total pixels</td><td>%10</td></tr>"
           "<tr><td>Composition area</td><td>%11, %12 → %13, %14</td></tr>"
           "<tr><td>Est. 1G ports</td><td>%15 (≈650k px/port @ 60 Hz)</td></tr>"
           "</table>")
            .arg(w->pixelWidth())
            .arg(w->pixelHeight())
            .arg(pitch)
            .arg(w->widthM(), 0, 'f', 2)
            .arg(w->heightM(), 0, 'f', 2)
            .arg(diag)
            .arg(w->widthM() * w->heightM(), 0, 'f', 2)
            .arg(aspect, 0, 'f', 3)
            .arg(w->cols * w->rows)
            .arg(QLocale().toString(w->totalPixels()))
            .arg(w->x)
            .arg(w->y)
            .arg(w->x + w->pixelWidth())
            .arg(w->y + w->pixelHeight())
            .arg(ports));

    const Project &p = m_doc->project();
    const int prev = m_wallScreen->currentData().toInt();
    m_wallScreen->clear();
    for (int i = 0; i < p.screens.size(); ++i)
        m_wallScreen->addItem(p.screens[i].name, i);
    if (p.screens.isEmpty())
        m_wallScreen->addItem(tr("(new screen)"), 0);
    m_wallScreen->setCurrentIndex(qMax(0, m_wallScreen->findData(prev)));
}

// ---------------------------------------------------------------- Screen

QWidget *PropertyPanel::buildScreenPage()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    auto *box = new QGroupBox(tr("Screen (video output)"));
    auto *f = new QFormLayout(box);
    m_screenName = new QLineEdit;
    m_screenEnabled = new QCheckBox(tr("Enabled"));
    m_screenPreset = new QComboBox;
    fillSizePresets(m_screenPreset);
    m_screenW = makeSpin(1, 16384);
    m_screenH = makeSpin(1, 16384);
    m_display = new QComboBox;
    auto *refreshBtn = new QPushButton(tr("Refresh"));
    auto *matchBtn = new QPushButton(tr("Match display resolution"));
    auto *identifyBtn = new QPushButton(tr("Identify displays"));
    identifyBtn->setToolTip(tr("Show each physical display's number and name on it (Ctrl+I)"));
    connect(identifyBtn, &QPushButton::clicked, this, &PropertyPanel::identifyRequested);
    auto *addSlice = new QPushButton(tr("Add slice"));
    m_screenInfo = new QLabel;
    m_screenInfo->setWordWrap(true);
    f->addRow(tr("Name"), m_screenName);
    f->addRow(QString(), m_screenEnabled);
    f->addRow(tr("Output device"), row({m_display, refreshBtn}));
    f->addRow(QString(), row({matchBtn, identifyBtn}));
    f->addRow(tr("Preset"), m_screenPreset);
    f->addRow(tr("Width"), m_screenW);
    f->addRow(tr("Height"), m_screenH);
    f->addRow(addSlice);
    f->addRow(m_screenInfo);
    v->addWidget(box);
    v->addStretch();

    connect(m_screenName, &QLineEdit::textEdited, this, [this](const QString &t) {
        editScreen([&](Screen &s) { s.name = t; }, true);
    });
    connect(m_screenEnabled, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_refreshing)
            editScreen([&](Screen &s) { s.enabled = on; });
    });
    connect(m_screenPreset, &QComboBox::activated, this, [this](int i) {
        const QSize sz = m_screenPreset->itemData(i).toSize();
        if (!sz.isEmpty())
            editScreen([&](Screen &s) {
                s.width = sz.width();
                s.height = sz.height();
            });
    });
    connect(m_screenW, &QSpinBox::valueChanged, this, [this](int v) {
        if (!m_refreshing)
            editScreen([&](Screen &s) { s.width = v; });
    });
    connect(m_screenH, &QSpinBox::valueChanged, this, [this](int v) {
        if (!m_refreshing)
            editScreen([&](Screen &s) { s.height = v; });
    });
    connect(m_display, &QComboBox::activated, this, [this](int i) {
        const QString name = m_display->itemData(i).toString();
        editScreen([&](Screen &s) { s.display = name; });
    });
    connect(refreshBtn, &QPushButton::clicked, this, &PropertyPanel::refreshDisplays);
    connect(matchBtn, &QPushButton::clicked, this, [this] {
        const Screen *s = m_doc->selectedScreen();
        if (!s)
            return;
        for (QScreen *qs : QGuiApplication::screens()) {
            if (qs->name() != s->display)
                continue;
            const QSize px = qs->geometry().size() * qs->devicePixelRatio();
            editScreen([&](Screen &sc) {
                sc.width = px.width();
                sc.height = px.height();
            });
        }
    });
    connect(addSlice, &QPushButton::clicked, this, [this] {
        const Selection s = m_doc->selection();
        if (s.screen >= 0)
            m_doc->addSlice(s.screen);
    });
    return page;
}

void PropertyPanel::refreshDisplays()
{
    const QString current = m_display->currentData().toString();
    QSignalBlocker block(m_display);
    m_display->clear();
    m_display->addItem(tr("None (preview only)"), QString());
    m_display->addItem(tr("Window (for testing)"), QStringLiteral("window"));
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (int i = 0; i < screens.size(); ++i) {
        QScreen *qs = screens[i];
        const QSize px = qs->geometry().size() * qs->devicePixelRatio();
        const QString label = tr("Display %1: %2  %3 x %4 @ %5 Hz%6")
                                  .arg(i + 1)
                                  .arg(qs->model().isEmpty() ? qs->name() : qs->model())
                                  .arg(px.width())
                                  .arg(px.height())
                                  .arg(qRound(qs->refreshRate()))
                                  .arg(qs == QGuiApplication::primaryScreen() ? tr(" (primary)") : QString());
        m_display->addItem(label, qs->name());
    }
    m_display->setCurrentIndex(qMax(0, m_display->findData(current)));
    refresh();
}

void PropertyPanel::editScreen(const std::function<void(Screen &)> &fn, bool structural)
{
    Screen *s = m_doc->selectedScreen();
    if (!s)
        return;
    fn(*s);
    m_doc->notifyChanged(structural);
}

void PropertyPanel::refreshScreen()
{
    const Screen *s = m_doc->selectedScreen();
    if (!s)
        return;
    setText(m_screenName, s->name);
    m_screenEnabled->setChecked(s->enabled);
    syncSizePreset(m_screenPreset, s->width, s->height);
    setValue(m_screenW, s->width);
    setValue(m_screenH, s->height);
    int di = m_display->findData(s->display);
    if (di < 0 && !s->display.isEmpty()) {
        m_display->addItem(tr("%1 (not connected)").arg(s->display), s->display);
        di = m_display->count() - 1;
    }
    m_display->setCurrentIndex(qMax(0, di));

    QString info = tr("%n slice(s).", nullptr, int(s->slices.size()));
    for (QScreen *qs : QGuiApplication::screens()) {
        if (qs->name() != s->display)
            continue;
        const QSize px = qs->geometry().size() * qs->devicePixelRatio();
        if (px != QSize(s->width, s->height))
            info += tr(" <b>Warning:</b> the display runs at %1 x %2, so the output will be rescaled and "
                       "will not be pixel-exact.")
                        .arg(px.width())
                        .arg(px.height());
    }
    m_screenInfo->setText(info);
}

// ---------------------------------------------------------------- Slice

QWidget *PropertyPanel::buildSlicePage()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);

    auto *box = new QGroupBox(tr("Slice"));
    auto *f = new QFormLayout(box);
    m_sliceName = new QLineEdit;
    m_sliceEnabled = new QCheckBox(tr("Enabled"));
    f->addRow(tr("Name"), m_sliceName);
    f->addRow(QString(), m_sliceEnabled);
    v->addWidget(box);

    auto *in = new QGroupBox(tr("Input (from composition)"));
    auto *inf = new QFormLayout(in);
    m_inX = makeSpin(-100000, 100000);
    m_inY = makeSpin(-100000, 100000);
    m_inW = makeSpin(1, 100000);
    m_inH = makeSpin(1, 100000);
    m_sliceWall = new QComboBox;
    auto *fromWall = new QPushButton(tr("Use wall"));
    auto *fullComp = new QPushButton(tr("Whole composition"));
    inf->addRow(tr("X / Y"), row({m_inX, m_inY}));
    inf->addRow(tr("W / H"), row({m_inW, m_inH}));
    inf->addRow(tr("From wall"), row({m_sliceWall, fromWall}));
    inf->addRow(fullComp);
    v->addWidget(in);

    auto *out = new QGroupBox(tr("Output (on screen)"));
    auto *outf = new QFormLayout(out);
    m_outX = makeSpin(-100000, 100000);
    m_outY = makeSpin(-100000, 100000);
    m_outW = makeSpin(1, 100000);
    m_outH = makeSpin(1, 100000);
    m_rotation = new QComboBox;
    m_rotation->addItems({QStringLiteral("0°"), QStringLiteral("90° CW"), QStringLiteral("180°"), QStringLiteral("270° CW")});
    m_flipH = new QCheckBox(tr("Flip horizontal"));
    m_flipV = new QCheckBox(tr("Flip vertical"));
    m_brightness = makeDouble(0, 200, 0, QStringLiteral(" %"));
    auto *oneToOne = new QPushButton(tr("Pixel-exact (1:1 with input)"));
    auto *fitScreen = new QPushButton(tr("Fit to screen"));
    auto *fillScreen = new QPushButton(tr("Stretch to screen"));
    outf->addRow(tr("X / Y"), row({m_outX, m_outY}));
    outf->addRow(tr("W / H"), row({m_outW, m_outH}));
    outf->addRow(tr("Rotation"), m_rotation);
    outf->addRow(QString(), row({m_flipH, m_flipV}));
    outf->addRow(tr("Brightness"), m_brightness);
    outf->addRow(oneToOne);
    outf->addRow(row({fitScreen, fillScreen}));
    v->addWidget(out);
    v->addStretch();

    connect(m_sliceName, &QLineEdit::textEdited, this, [this](const QString &t) {
        Slice *sl = m_doc->selectedSlice();
        if (!sl)
            return;
        sl->name = t;
        m_doc->notifyChanged(true);
    });
    connect(m_sliceEnabled, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_refreshing)
            editSlice([&](Slice &s) { s.enabled = on; });
    });

    auto rectField = [this](QSpinBox *s, QRect Slice::*rect, int which) {
        connect(s, &QSpinBox::valueChanged, this, [this, rect, which](int v) {
            if (m_refreshing)
                return;
            editSlice([&](Slice &sl) {
                QRect &r = sl.*rect;
                switch (which) {
                case 0: r.moveLeft(v); break;
                case 1: r.moveTop(v); break;
                case 2: r.setWidth(v); break;
                case 3: r.setHeight(v); break;
                }
            });
        });
    };
    rectField(m_inX, &Slice::input, 0);
    rectField(m_inY, &Slice::input, 1);
    rectField(m_inW, &Slice::input, 2);
    rectField(m_inH, &Slice::input, 3);
    rectField(m_outX, &Slice::output, 0);
    rectField(m_outY, &Slice::output, 1);
    rectField(m_outW, &Slice::output, 2);
    rectField(m_outH, &Slice::output, 3);

    connect(m_rotation, &QComboBox::activated, this, [this](int i) {
        editSlice([&](Slice &s) {
            // Keep the output footprint matching the rotated content.
            if ((s.rotation ^ i) & 1)
                s.output.setSize(s.output.size().transposed());
            s.rotation = i;
        });
    });
    connect(m_flipH, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_refreshing)
            editSlice([&](Slice &s) { s.flipH = on; });
    });
    connect(m_flipV, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_refreshing)
            editSlice([&](Slice &s) { s.flipV = on; });
    });
    connect(m_brightness, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (!m_refreshing)
            editSlice([&](Slice &s) { s.brightness = v / 100.0; });
    });
    connect(fromWall, &QPushButton::clicked, this, [this] {
        const int w = m_sliceWall->currentData().toInt();
        const Project &p = m_doc->project();
        if (w < 0 || w >= p.walls.size())
            return;
        const QRect r = p.walls[w].rect();
        editSlice([&](Slice &s) { s.input = r; });
    });
    connect(fullComp, &QPushButton::clicked, this, [this] {
        const QSize c = m_doc->project().compSize();
        editSlice([&](Slice &s) { s.input = QRect(QPoint(), c); });
    });
    connect(oneToOne, &QPushButton::clicked, this, [this] {
        editSlice([&](Slice &s) {
            s.output.setSize(s.rotation & 1 ? s.input.size().transposed() : s.input.size());
        });
    });
    auto toScreen = [this](bool keepAspect) {
        const Screen *sc = m_doc->selectedScreen();
        if (!sc)
            return;
        const QSize screen(sc->width, sc->height);
        editSlice([&](Slice &s) {
            const QSize content = s.rotation & 1 ? s.input.size().transposed() : s.input.size();
            const QSize sz = keepAspect ? content.scaled(screen, Qt::KeepAspectRatio) : screen;
            s.output = QRect((screen.width() - sz.width()) / 2, (screen.height() - sz.height()) / 2, sz.width(),
                             sz.height());
        });
    };
    connect(fitScreen, &QPushButton::clicked, this, [toScreen] { toScreen(true); });
    connect(fillScreen, &QPushButton::clicked, this, [toScreen] { toScreen(false); });
    return page;
}

void PropertyPanel::editSlice(const std::function<void(Slice &)> &fn)
{
    Slice *s = m_doc->selectedSlice();
    if (!s)
        return;
    fn(*s);
    m_doc->notifyChanged();
}

void PropertyPanel::refreshSlice()
{
    const Slice *s = m_doc->selectedSlice();
    if (!s)
        return;
    setText(m_sliceName, s->name);
    m_sliceEnabled->setChecked(s->enabled);
    setValue(m_inX, s->input.x());
    setValue(m_inY, s->input.y());
    setValue(m_inW, s->input.width());
    setValue(m_inH, s->input.height());
    setValue(m_outX, s->output.x());
    setValue(m_outY, s->output.y());
    setValue(m_outW, s->output.width());
    setValue(m_outH, s->output.height());
    m_rotation->setCurrentIndex(s->rotation & 3);
    m_flipH->setChecked(s->flipH);
    m_flipV->setChecked(s->flipV);
    setValue(m_brightness, qRound(s->brightness * 100.0));

    const Project &p = m_doc->project();
    const int prev = m_sliceWall->currentData().toInt();
    m_sliceWall->clear();
    for (int i = 0; i < p.walls.size(); ++i)
        m_sliceWall->addItem(p.walls[i].name, i);
    int match = -1;
    for (int i = 0; i < p.walls.size(); ++i)
        if (p.walls[i].rect() == s->input)
            match = i;
    m_sliceWall->setCurrentIndex(qMax(0, m_sliceWall->findData(match >= 0 ? match : prev)));
}

// ----------------------------------------------------------------

void PropertyPanel::refresh()
{
    m_refreshing = true;
    const Selection s = m_doc->selection();
    switch (s.kind) {
    case Selection::Wall:
        m_stack->setCurrentIndex(WallPage);
        refreshWall();
        break;
    case Selection::Screen:
        m_stack->setCurrentIndex(ScreenPage);
        refreshScreen();
        break;
    case Selection::Slice:
        m_stack->setCurrentIndex(SlicePage);
        refreshSlice();
        break;
    default:
        m_stack->setCurrentIndex(CompositionPage);
        refreshComposition();
        break;
    }
    m_refreshing = false;
}
