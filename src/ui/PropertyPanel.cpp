#include "ui/PropertyPanel.h"

#include "media/MediaSource.h"
#include "media/Ndi.h"
#include "media/TextureShare.h"
#include "render/RenderEngine.h"
#include "model/CabinetMapping.h"
#include "ui/CabinetLayoutEditor.h"
#include "ui/EffectChainEditor.h"
#include "ui/MaskEditor.h"
#include "ui/PanelGridEditor.h"
#include "model/Document.h"

#include <QCameraDevice>
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
#include <QMediaDevices>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtMath>

namespace {

enum Page { CompositionPage, WallPage, ScreenPage, SlicePage, LayerPage, ClipPage };

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

PropertyPanel::PropertyPanel(Document *doc, RenderEngine *engine, QWidget *parent)
    : QWidget(parent)
    , m_doc(doc)
    , m_engine(engine)
{
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_stack = new QStackedWidget;
    m_stack->addWidget(buildCompositionPage());
    m_stack->addWidget(buildWallPage());
    m_stack->addWidget(buildScreenPage());
    m_stack->addWidget(buildSlicePage());
    m_stack->addWidget(buildLayerPage());
    m_stack->addWidget(buildClipPage());
    scroll->setWidget(m_stack);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);

    connect(m_doc, &Document::changed, this, &PropertyPanel::refresh);
    connect(m_doc, &Document::selectionChanged, this, &PropertyPanel::refresh);
    connect(m_engine, &RenderEngine::mediaStateChanged, this, &PropertyPanel::refresh);
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

    auto *overlay = new QGroupBox(tr("LED panel overlay"));
    auto *of = new QFormLayout(overlay);
    m_overlay = new QCheckBox(tr("Show panel outlines and numbers on top of content"));
    m_overlayOpacity = makeDouble(0, 100, 0, QStringLiteral(" %"));
    of->addRow(m_overlay);
    of->addRow(tr("Opacity"), m_overlayOpacity);
    m_maskToPanels = new QCheckBox(tr("Black out areas without LED panels"));
    m_maskToPanels->setToolTip(tr("Show only what the physical walls can display: gaps and cabinets marked as not fitted go black"));
    of->addRow(m_maskToPanels);
    v->addWidget(overlay);

    auto *fx = new QGroupBox(tr("Composition effects"));
    auto *fxl = new QVBoxLayout(fx);
    m_compFx = new EffectChainEditor(m_doc);
    m_compFx->setToolTip(tr("Applied to everything, before the panel mask and the master fader"));
    connect(m_compFx, &EffectChainEditor::midiLearnRequested, this, &PropertyPanel::midiLearnRequested);
    fxl->addWidget(m_compFx);
    v->addWidget(fx);

    auto *hint = new QLabel(tr("Content is played from the clip deck: select a layer or clip slot to edit it."));
    hint->setWordWrap(true);
    hint->setEnabled(false);
    v->addWidget(hint);
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
    connect(m_overlay, &QCheckBox::toggled, this, [this, &p](bool on) {
        if (m_refreshing) return;
        p.panelOverlay = on;
        m_doc->notifyChanged();
    });
    connect(m_maskToPanels, &QCheckBox::toggled, this, [this, &p](bool on) {
        if (m_refreshing) return;
        p.maskToPanels = on;
        m_doc->notifyChanged();
    });
    connect(m_overlayOpacity, &QDoubleSpinBox::valueChanged, this, [this, &p](double v) {
        if (m_refreshing) return;
        p.overlayOpacity = v / 100.0;
        m_doc->notifyChanged();
    });
    return page;
}

void PropertyPanel::refreshComposition()
{
    const Project &p = m_doc->project();
    syncSizePreset(m_compPreset, p.compWidth, p.compHeight);
    setValue(m_compW, p.compWidth);
    setValue(m_compH, p.compHeight);
    setButtonColor(m_compBg, p.background);
    m_overlay->setChecked(p.panelOverlay);
    setValue(m_overlayOpacity, qRound(p.overlayOpacity * 100.0));
    m_maskToPanels->setChecked(p.maskToPanels);
    m_compFx->setLayer(-1);
}

// ---------------------------------------------------------------- Layer

QWidget *PropertyPanel::buildLayerPage()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    auto *box = new QGroupBox(tr("Layer"));
    auto *f = new QFormLayout(box);
    m_layerName = new QLineEdit;
    m_layerOpacity = makeDouble(0, 100, 0, QStringLiteral(" %"));
    m_layerVolume = makeDouble(0, 100, 0, QStringLiteral(" %"));
    m_layerVolume->setToolTip(tr("Audio level of this layer's clips"));
    m_layerBlend = new QComboBox;
    for (int i = 0; i <= int(BlendMode::Darken); ++i)
        m_layerBlend->addItem(blendModeName(BlendMode(i)));
    m_layerTarget = new QComboBox;
    m_layerTarget->setToolTip(tr("Clips on this layer are fitted into this area"));
    m_layerTransition = makeSpin(0, 60000, QStringLiteral(" ms"));
    m_layerTransition->setSingleStep(100);
    m_layerTransition->setSpecialValueText(tr("Cut"));
    m_layerBypass = new QCheckBox(tr("Bypass"));
    m_layerSolo = new QCheckBox(tr("Solo"));
    f->addRow(tr("Name"), m_layerName);
    f->addRow(tr("Opacity"), m_layerOpacity);
    f->addRow(tr("Volume"), m_layerVolume);
    f->addRow(tr("Blend"), m_layerBlend);
    f->addRow(tr("Fit clips into"), m_layerTarget);
    f->addRow(tr("Transition"), m_layerTransition);
    m_transition = new QComboBox;
    for (int i = 0; i < kTransitionStyleCount; ++i)
        m_transition->addItem(transitionStyleName(TransitionStyle(i)));
    m_transition->setToolTip(tr("How the next clip replaces the playing one, over the transition time"));
    f->addRow(tr("Style"), m_transition);
    m_autopilot = new QComboBox;
    for (int i = 0; i < 3; ++i)
        m_autopilot->addItem(autopilotName(::Autopilot(i)));
    m_autopilot->setToolTip(tr("Move on when a clip finishes: movies at their end (or each loop), "
                               "anything else after the time set here"));
    m_autopilotSeconds = makeDouble(0.5, 3600, 1, QStringLiteral(" s"));
    f->addRow(tr("Autopilot"), row({m_autopilot, m_autopilotSeconds}));
    f->addRow(QString(), row({m_layerBypass, m_layerSolo}));
    v->addWidget(box);

    auto *fx = new QGroupBox(tr("Effects"));
    auto *fxl = new QVBoxLayout(fx);
    m_layerFx = new EffectChainEditor(m_doc);
    connect(m_layerFx, &EffectChainEditor::midiLearnRequested, this, &PropertyPanel::midiLearnRequested);
    fxl->addWidget(m_layerFx);
    v->addWidget(fx);
    v->addStretch();

    connect(m_layerName, &QLineEdit::textEdited, this, [this](const QString &t) {
        editLayer([&](Layer &l) { l.name = t; }, true);
    });
    connect(m_layerBlend, &QComboBox::activated, this, [this](int i) {
        editLayer([&](Layer &l) { l.blend = BlendMode(i); });
    });
    connect(m_layerTarget, &QComboBox::activated, this, [this](int i) {
        const int wall = m_layerTarget->itemData(i).toInt();
        editLayer([&](Layer &l) { l.targetWall = wall; });
    });
    connect(m_transition, &QComboBox::activated, this, [this](int i) {
        editLayer([&](Layer &l) { l.transition = TransitionStyle(i); });
    });
    connect(m_autopilot, &QComboBox::activated, this, [this](int i) {
        editLayer([&](Layer &l) { l.autopilot = ::Autopilot(i); });
    });
    connect(m_autopilotSeconds, &QDoubleSpinBox::valueChanged, this, [this](double s) {
        if (!m_refreshing)
            editLayer([&](Layer &l) { l.autopilotSeconds = s; });
    });
    connect(m_layerTransition, &QSpinBox::valueChanged, this, [this](int ms) {
        if (!m_refreshing)
            editLayer([&](Layer &l) { l.transitionMs = ms; });
    });
    // Opacity, bypass and solo are performance controls: not undo steps.
    auto playback = [this] {
        if (m_refreshing)
            return;
        const Selection s = m_doc->selection();
        m_doc->setLayerPlayback(s.layer, m_layerOpacity->value() / 100.0, m_layerBypass->isChecked(),
                                m_layerSolo->isChecked());
    };
    connect(m_layerOpacity, &QDoubleSpinBox::valueChanged, this, playback);
    connect(m_layerVolume, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (!m_refreshing)
            m_doc->setLayerVolume(m_doc->selection().layer, v / 100.0);
    });
    connect(m_layerBypass, &QCheckBox::toggled, this, playback);
    connect(m_layerSolo, &QCheckBox::toggled, this, playback);
    return page;
}

void PropertyPanel::editLayer(const std::function<void(Layer &)> &fn, bool structural)
{
    Layer *l = m_doc->selectedLayer();
    if (!l)
        return;
    fn(*l);
    m_doc->notifyChanged(structural);
}

void PropertyPanel::refreshLayer()
{
    const Layer *l = m_doc->selectedLayer();
    if (!l)
        return;
    setText(m_layerName, l->name);
    setValue(m_layerOpacity, qRound(l->opacity * 100.0));
    setValue(m_layerVolume, qRound(l->volume * 100.0));
    m_layerBlend->setCurrentIndex(int(l->blend));
    setValue(m_layerTransition, l->transitionMs);
    m_transition->setCurrentIndex(int(l->transition));
    m_autopilot->setCurrentIndex(int(l->autopilot));
    setValue(m_autopilotSeconds, l->autopilotSeconds);
    m_autopilotSeconds->setEnabled(l->autopilot != ::Autopilot::Off);
    m_layerBypass->setChecked(l->bypass);
    m_layerSolo->setChecked(l->solo);
    const Project &p = m_doc->project();
    m_layerTarget->clear();
    m_layerTarget->addItem(tr("Whole composition"), -1);
    for (int i = 0; i < p.walls.size(); ++i)
        m_layerTarget->addItem(p.walls[i].name, i);
    m_layerTarget->setCurrentIndex(qMax(0, m_layerTarget->findData(l->targetWall)));
    m_layerFx->setLayer(m_doc->selection().layer);
}

// ---------------------------------------------------------------- Clip

QWidget *PropertyPanel::buildClipPage()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    m_clipStack = new QStackedWidget;
    v->addWidget(m_clipStack);
    v->addStretch();

    // Empty slot: offer to fill it.
    auto *empty = new QGroupBox(tr("Empty clip slot"));
    auto *ef = new QFormLayout(empty);
    auto *newPattern = new QComboBox;
    for (int i = 0; i < kTestPatternCount; ++i)
        newPattern->addItem(testPatternName(TestPattern(i)));
    auto *addPattern = new QPushButton(tr("Add"));
    auto *addMedia = new QPushButton(tr("Load media file..."));
    ef->addRow(tr("Test pattern"), row({newPattern, addPattern}));
    ef->addRow(addMedia);
    ef->addRow(new QLabel(tr("Tip: drag video or image files onto the clip deck.")));
    m_clipStack->addWidget(empty);

    auto *box = new QGroupBox(tr("Clip"));
    auto *f = new QFormLayout(box);
    m_clipName = new QLineEdit;
    m_source = new QComboBox;
    m_source->addItems({tr("Test pattern"), tr("Media file"), tr("Capture device (live)"), tr("NDI source (live)"),
                        tr("%1 (live)").arg(TextureShare::technology().isEmpty() ? tr("Spout / Syphon") : TextureShare::technology())});
    m_input = new QComboBox;
    m_input->setToolTip(tr("Camera or capture card, or NDI source on the network"));
    auto *refreshInputs = new QPushButton(tr("Refresh"));
    m_pattern = new QComboBox;
    for (int i = 0; i < kTestPatternCount; ++i)
        m_pattern->addItem(testPatternName(TestPattern(i)));
    m_gridSpacing = makeSpin(4, 2048);
    m_solidColor = new QPushButton;
    m_mediaPath = new QLineEdit;
    m_mediaPath->setReadOnly(true);
    auto *browse = new QPushButton(tr("Browse..."));
    m_clipVolume = makeDouble(0, 200, 0, QStringLiteral(" %"));
    m_clipVolume->setToolTip(tr("This clip's audio level (above 100% boosts quiet files)"));
    m_scale = new QComboBox;
    for (int i = 0; i <= int(ScaleMode::Native); ++i)
        m_scale->addItem(scaleModeName(ScaleMode(i)));
    auto *trigger = new QPushButton(tr("Trigger"));
    auto *play = new QPushButton(tr("Play"));
    auto *pause = new QPushButton(tr("Pause"));
    auto *restart = new QPushButton(tr("Restart"));
    m_mediaStatus = new QLabel;
    m_mediaStatus->setWordWrap(true);
    f->addRow(tr("Name"), m_clipName);
    f->addRow(tr("Source"), m_source);
    f->addRow(tr("Pattern"), m_pattern);
    f->addRow(tr("Grid size"), m_gridSpacing);
    f->addRow(tr("Solid colour"), m_solidColor);
    f->addRow(tr("Media"), row({m_mediaPath, browse}));
    f->addRow(tr("Input"), row({m_input, refreshInputs}));
    f->addRow(tr("Volume"), m_clipVolume);
    f->addRow(tr("Scale"), m_scale);
    f->addRow(trigger);
    f->addRow(QString(), row({play, pause, restart}));
    f->addRow(QString(), m_mediaStatus);
    m_trigger = new QComboBox;
    for (int i = 0; i < kTriggerModeCount; ++i)
        m_trigger->addItem(triggerModeName(TriggerMode(i)));
    m_trigger->setToolTip(tr("What triggering this clip does (mouse, MIDI note, OSC)"));
    f->addRow(tr("Trigger"), m_trigger);

    // Playback of movies.
    auto *pb = new QGroupBox(tr("Playback"));
    auto *pf = new QFormLayout(pb);
    m_playMode = new QComboBox;
    for (int i = 0; i < kPlayModeCount; ++i)
        m_playMode->addItem(playModeName(PlayMode(i)));
    m_speed = makeDouble(0.05, 8, 2, QStringLiteral(" x"));
    m_speed->setSingleStep(0.05);
    m_reverse = new QCheckBox(tr("Reverse"));
    m_reverse->setToolTip(tr("Play backwards (HAP movies; other formats only play forwards)"));
    m_inPoint = makeDouble(0, 100, 1, QStringLiteral(" %"));
    m_outPoint = makeDouble(0, 100, 1, QStringLiteral(" %"));
    m_inPoint->setToolTip(tr("Where the clip starts (and loops back to)"));
    m_outPoint->setToolTip(tr("Where the clip ends (and loops from)"));
    m_position = new QSlider(Qt::Horizontal);
    m_position->setRange(0, 1000);
    m_position->setToolTip(tr("Drag to scrub the playing clip"));
    m_positionText = new QLabel;
    m_positionText->setMinimumWidth(110);
    pf->addRow(tr("At the end"), m_playMode);
    pf->addRow(tr("Speed"), row({m_speed, m_reverse}));
    m_beatSync = makeSpin(0, 1024, tr(" beats"));
    m_beatSync->setSpecialValueText(tr("Off"));
    m_beatSync->setToolTip(tr("Stretch the movie (in to out) to last this many beats at the current tempo"));
    pf->addRow(tr("Beat sync"), m_beatSync);
    pf->addRow(tr("In / out"), row({m_inPoint, m_outPoint}));
    pf->addRow(tr("Position"), row({m_position, m_positionText}));
    m_clipStack->addWidget(box);

    // Transform within the layer's area.
    auto *tf = new QGroupBox(tr("Transform"));
    auto *tl = new QFormLayout(tf);
    m_clipX = makeDouble(-100000, 100000, 0, QStringLiteral(" px"));
    m_clipY = makeDouble(-100000, 100000, 0, QStringLiteral(" px"));
    m_clipScale = makeDouble(1, 2000, 0, QStringLiteral(" %"));
    m_clipRotation = makeDouble(-360, 360, 1, QStringLiteral("\u00B0"));
    m_clipOpacity = makeDouble(0, 100, 0, QStringLiteral(" %"));
    auto *resetTransform = new QPushButton(tr("Reset"));
    tl->addRow(tr("Position"), row({m_clipX, m_clipY}));
    tl->addRow(tr("Scale / rotate"), row({m_clipScale, m_clipRotation}));
    tl->addRow(tr("Opacity"), row({m_clipOpacity, resetTransform}));

    auto *cfx = new QGroupBox(tr("Clip effects"));
    auto *cfl = new QVBoxLayout(cfx);
    m_clipFx = new EffectChainEditor(m_doc);
    cfl->addWidget(m_clipFx);

    // The clip page scrolls as one: settings, playback, transform, effects.
    auto *clipPage = new QWidget;
    auto *cv = new QVBoxLayout(clipPage);
    cv->setContentsMargins(0, 0, 0, 0);
    m_clipStack->removeWidget(box);
    cv->addWidget(box);
    cv->addWidget(pb);
    cv->addWidget(tf);
    cv->addWidget(cfx);
    m_clipStack->addWidget(clipPage);

    connect(m_trigger, &QComboBox::activated, this, [this](int i) { editClip([&](Clip &c) { c.trigger = TriggerMode(i); }); });
    connect(m_playMode, &QComboBox::activated, this, [this](int i) { editClip([&](Clip &c) { c.playMode = PlayMode(i); }); });
    connect(m_reverse, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_refreshing)
            editClip([&](Clip &c) { c.reverse = on; });
    });
    auto clipNumber = [this](QDoubleSpinBox *spin, const std::function<void(Clip &, double)> &set) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this, set](double v) {
            if (!m_refreshing)
                editClip([&](Clip &c) { set(c, v); });
        });
    };
    clipNumber(m_speed, [](Clip &c, double v) { c.speed = v; });
    connect(m_beatSync, &QSpinBox::valueChanged, this, [this](int b) {
        if (!m_refreshing)
            editClip([&](Clip &c) { c.beatSync = b; });
    });
    clipNumber(m_inPoint, [](Clip &c, double v) {
        c.inPoint = v / 100.0;
        c.outPoint = qMax(c.outPoint, c.inPoint);
    });
    clipNumber(m_outPoint, [](Clip &c, double v) {
        c.outPoint = v / 100.0;
        c.inPoint = qMin(c.inPoint, c.outPoint);
    });
    clipNumber(m_clipX, [](Clip &c, double v) { c.posX = v; });
    clipNumber(m_clipY, [](Clip &c, double v) { c.posY = v; });
    clipNumber(m_clipScale, [](Clip &c, double v) { c.scaleBy = v / 100.0; });
    clipNumber(m_clipRotation, [](Clip &c, double v) { c.rotation = v; });
    clipNumber(m_clipOpacity, [](Clip &c, double v) { c.opacity = v / 100.0; });
    connect(resetTransform, &QPushButton::clicked, this, [this] {
        editClip([](Clip &c) {
            c.posX = c.posY = c.rotation = 0.0;
            c.scaleBy = c.opacity = 1.0;
        });
    });
    connect(m_position, &QSlider::sliderMoved, this, [this](int v) {
        const Clip *c = m_doc->selectedClip();
        if (MediaSource *m = c ? m_engine->mediaFor(c->id) : nullptr)
            m->seek(v / 1000.0);
    });
    auto *positionTimer = new QTimer(this);
    positionTimer->setInterval(100);
    connect(positionTimer, &QTimer::timeout, this, &PropertyPanel::refreshPosition);
    positionTimer->start();

    connect(addPattern, &QPushButton::clicked, this, [this, newPattern] {
        const Selection s = m_doc->selection();
        m_doc->setClip(s.layer, s.column, Clip::fromPattern(TestPattern(newPattern->currentIndex())));
    });
    connect(addMedia, &QPushButton::clicked, this, &PropertyPanel::browseMedia);
    connect(browse, &QPushButton::clicked, this, &PropertyPanel::browseMedia);
    connect(m_clipName, &QLineEdit::textEdited, this, [this](const QString &t) {
        editClip([&](Clip &c) { c.name = t; });
    });
    connect(m_source, &QComboBox::activated, this, [this](int i) {
        editClip([&](Clip &c) {
            if (c.source != ContentSource(i))
                c.inputId.clear(); // device ids and NDI names do not carry over
            c.source = ContentSource(i);
        });
    });
    connect(m_input, &QComboBox::activated, this, [this](int i) {
        const QString id = m_input->itemData(i).toString();
        const QString label = m_input->itemText(i);
        editClip([&](Clip &c) {
            c.inputId = id;
            if (c.source == ContentSource::Capture)
                c.name = label;
        });
    });
    connect(refreshInputs, &QPushButton::clicked, this, &PropertyPanel::refresh);
    connect(m_pattern, &QComboBox::activated, this, [this](int i) {
        editClip([&](Clip &c) {
            c.pattern = TestPattern(i);
            c.source = ContentSource::TestPattern;
        });
    });
    connect(m_gridSpacing, &QSpinBox::valueChanged, this, [this](int v) {
        if (!m_refreshing)
            editClip([&](Clip &c) { c.gridSpacing = v; });
    });
    connect(m_solidColor, &QPushButton::clicked, this, [this] {
        const Clip *clip = m_doc->selectedClip();
        if (!clip)
            return;
        const QColor col = QColorDialog::getColor(clip->solidColor, this, tr("Solid colour"));
        if (col.isValid())
            editClip([&](Clip &c) { c.solidColor = col; });
    });
    connect(m_scale, &QComboBox::activated, this, [this](int i) {
        editClip([&](Clip &c) { c.scale = ScaleMode(i); });
    });
    connect(m_clipVolume, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (!m_refreshing)
            editClip([&](Clip &c) { c.volume = v / 100.0; });
    });
    connect(trigger, &QPushButton::clicked, this, [this] {
        const Selection s = m_doc->selection();
        m_doc->triggerClip(s.layer, s.column);
    });
    auto withMedia = [this](void (MediaSource::*fn)()) {
        return [this, fn] {
            if (const Clip *c = m_doc->selectedClip())
                if (MediaSource *m = m_engine->mediaFor(c->id))
                    (m->*fn)();
        };
    };
    connect(play, &QPushButton::clicked, this, withMedia(&MediaSource::play));
    connect(pause, &QPushButton::clicked, this, withMedia(&MediaSource::pause));
    connect(restart, &QPushButton::clicked, this, withMedia(&MediaSource::restart));
    return page;
}

void PropertyPanel::editClip(const std::function<void(Clip &)> &fn)
{
    Clip *c = m_doc->selectedClip();
    if (!c)
        return;
    fn(*c);
    m_doc->notifyChanged(true); // the deck shows names and thumbnails
}

void PropertyPanel::browseMedia()
{
    const Selection s = m_doc->selection();
    if (s.kind != Selection::Clip)
        return;
    const Clip *existing = m_doc->selectedClip();
    const QString start = existing ? QFileInfo(existing->mediaPath).absolutePath() : QString();
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open media"), start,
        tr("Media (*.mp4 *.mov *.mkv *.avi *.webm *.wmv *.m4v *.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff);;All files (*)"));
    if (path.isEmpty())
        return;
    Clip c = existing ? *existing : Clip::fromMedia(path);
    c.source = ContentSource::Media;
    c.mediaPath = path;
    m_doc->setClip(s.layer, s.column, c);
}

void PropertyPanel::refreshClip()
{
    const Clip *c = m_doc->selectedClip();
    m_clipStack->setCurrentIndex(c ? 1 : 0);
    if (!c)
        return;
    setText(m_clipName, c->name);
    m_clipName->setPlaceholderText(c->displayName());
    m_source->setCurrentIndex(int(c->source));
    m_pattern->setCurrentIndex(int(c->pattern));
    setValue(m_gridSpacing, c->gridSpacing);
    setButtonColor(m_solidColor, c->solidColor);
    setText(m_mediaPath, QFileInfo(c->mediaPath).fileName());
    m_mediaPath->setToolTip(c->mediaPath);
    m_trigger->setCurrentIndex(int(c->trigger));
    m_playMode->setCurrentIndex(int(c->playMode));
    setValue(m_speed, c->speed);
    setValue(m_beatSync, c->beatSync);
    m_reverse->setChecked(c->reverse);
    setValue(m_inPoint, c->inPoint * 100.0);
    setValue(m_outPoint, c->outPoint * 100.0);
    setValue(m_clipX, c->posX);
    setValue(m_clipY, c->posY);
    setValue(m_clipScale, c->scaleBy * 100.0);
    setValue(m_clipRotation, c->rotation);
    setValue(m_clipOpacity, c->opacity * 100.0);
    m_clipFx->setClip(m_doc->selection().layer, m_doc->selection().column);
    m_scale->setCurrentIndex(int(c->scale));
    setValue(m_clipVolume, qRound(c->volume * 100.0));
    m_clipVolume->setEnabled(c->source == ContentSource::Media);

    // Live input list for the clip's source type.
    m_input->clear();
    if (c->source == ContentSource::Capture) {
        for (const QCameraDevice &cam : QMediaDevices::videoInputs())
            m_input->addItem(cam.description(), QString::fromUtf8(cam.id()));
        if (m_input->count() == 0)
            m_input->addItem(tr("No capture devices found"), QString());
    } else if (c->source == ContentSource::Share) {
        for (const QString &name : TextureShare::sources())
            m_input->addItem(name, name);
        if (m_input->count() == 0)
            m_input->addItem(TextureShare::technology().isEmpty() ? tr("Not available on this platform")
                                                                  : tr("No %1 sources found").arg(TextureShare::technology()),
                             QString());
    } else if (c->source == ContentSource::Ndi) {
        for (const QString &name : Ndi::sources())
            m_input->addItem(name, name);
        if (m_input->count() == 0)
            m_input->addItem(Ndi::available() ? tr("No NDI sources found yet") : tr("NDI runtime not installed"), QString());
    }
    int current = m_input->findData(c->inputId);
    if (current < 0 && !c->inputId.isEmpty()) { // remembered but not present right now
        m_input->addItem(tr("%1 (not available)").arg(c->source == ContentSource::Capture ? c->name : c->inputId), c->inputId);
        current = m_input->count() - 1;
    }
    m_input->setCurrentIndex(qMax(0, current));
    m_input->setEnabled(c->isLive());

    const bool media = c->source != ContentSource::TestPattern;
    m_pattern->setEnabled(!media);
    m_gridSpacing->setEnabled(!media && (c->pattern == TestPattern::Grid || c->pattern == TestPattern::Checker));
    m_solidColor->setEnabled(!media && c->pattern == TestPattern::Solid);
    m_scale->setEnabled(media);
    const bool movie = c->source == ContentSource::Media;
    for (QWidget *w : {static_cast<QWidget *>(m_playMode), static_cast<QWidget *>(m_speed), static_cast<QWidget *>(m_beatSync),
                       static_cast<QWidget *>(m_reverse), static_cast<QWidget *>(m_inPoint),
                       static_cast<QWidget *>(m_outPoint), static_cast<QWidget *>(m_position)})
        w->setEnabled(movie);
    refreshPosition();
    m_mediaPath->setEnabled(c->source == ContentSource::Media);

    QString status;
    if (media) {
        const MediaSource *m = m_engine->mediaFor(c->id);
        if (!m)
            status = tr("Not playing. Trigger the clip to load it.");
        else if (!m->errorString().isEmpty())
            status = tr("Error: %1").arg(m->errorString());
        else if (m->isVideo())
            status = (m->isPlaying() ? tr("Video playing") : tr("Video paused"))
                     + (m->isLive() ? QString() : m->hasAudio() ? tr(", with sound") : tr(", no audio track"));
        else if (m->isOpen())
            status = tr("Still image");
        else
            status = tr("Loading...");
    }
    m_mediaStatus->setText(status);
}

void PropertyPanel::refreshPosition()
{
    if (!m_position || !m_position->isVisible() || m_position->isSliderDown())
        return;
    const Clip *c = m_doc->selectedClip();
    const MediaSource *m = c ? m_engine->mediaFor(c->id) : nullptr;
    const double pos = m ? m->position() : -1.0;
    if (pos < 0) {
        m_positionText->setText(QStringLiteral("--:--"));
        return;
    }
    const QSignalBlocker b(m_position);
    m_position->setValue(qRound(pos * 1000));
    const double total = m->durationSeconds();
    auto mmss = [](double s) {
        const int t = qMax(0, int(s));
        return QStringLiteral("%1:%2").arg(t / 60).arg(t % 60, 2, 10, QLatin1Char('0'));
    };
    m_positionText->setText(QStringLiteral("%1 / %2").arg(mmss(pos * total), mmss(total)));
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
    m_wallAngle = makeDouble(-180, 180, 1, QStringLiteral("°"));
    m_wallAngle->setWrapping(true);
    m_wallAngle->setToolTip(tr("Rotates the whole wall about its centre, clockwise: for diamond or tilted walls. "
                               "Panel maps, masks, panel effects, slices and cabinet layouts follow it."));
    f->addRow(tr("Angle"), m_wallAngle);
    f->addRow(tr("Colour"), m_wallColor);
    v->addWidget(box);

    auto *panelsBox = new QGroupBox(tr("Fitted panels"));
    auto *pl = new QVBoxLayout(panelsBox);
    m_panelGrid = new PanelGridEditor;
    m_panelCount = new QLabel;
    auto *allOn = new QPushButton(tr("All panels fitted"));
    pl->addWidget(new QLabel(tr("Click or drag to mark cabinets that are not fitted (stepped or irregular walls).")));
    pl->addWidget(m_panelGrid, 0, Qt::AlignLeft);
    pl->addWidget(row({m_panelCount, allOn}));
    v->addWidget(panelsBox);
    connect(m_panelGrid, &PanelGridEditor::disabledChanged, this, [this](const QVector<QPoint> &off) {
        editWall([&](LedWall &w) { w.disabledPanels = off; });
    });
    connect(allOn, &QPushButton::clicked, this, [this] {
        editWall([](LedWall &w) { w.disabledPanels.clear(); });
    });

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
    auto *mapCabinets = new QPushButton(tr("Map each cabinet on screen"));
    mapCabinets->setToolTip(tr("One slice per fitted cabinet, packed and chained onto processor ports "
                               "(edit the layout on the screen's page)"));
    auto *fillContent = new QPushButton(tr("Fit all layers into this wall"));
    mf->addRow(tr("Screen"), m_wallScreen);
    mf->addRow(createSlice);
    mf->addRow(mapCabinets);
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
    connect(m_wallAngle, &QDoubleSpinBox::valueChanged, this, [this](double a) {
        if (!m_refreshing)
            editWall([&](LedWall &w) { w.angle = a; });
    });
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
    connect(mapCabinets, &QPushButton::clicked, this, [this] {
        const Selection s = m_doc->selection();
        if (s.kind != Selection::Wall)
            return;
        int screen = m_wallScreen->currentData().toInt();
        if (m_doc->project().screens.isEmpty())
            screen = m_doc->addScreen();
        CabinetLayout layout;
        layout.wall = s.wall;
        if (m_doc->addCabinetLayout(screen, layout) >= 0)
            m_doc->select(Selection::ofScreen(screen));
    });
    connect(fillContent, &QPushButton::clicked, this, [this] {
        const Selection s = m_doc->selection();
        if (s.kind != Selection::Wall)
            return;
        for (Layer &l : m_doc->project().layers)
            l.targetWall = s.wall;
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
    setValue(m_wallAngle, w->angle);
    setButtonColor(m_wallColor, w->color);
    m_panelGrid->setWall(w->cols, w->rows, w->disabledPanels, w->color);
    m_panelCount->setText(tr("%1 of %2 fitted").arg(w->enabledPanelCount()).arg(w->cols * w->rows));

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
            .arg(w->enabledPanelCount() == w->cols * w->rows
                     ? QString::number(w->cols * w->rows)
                     : tr("%1 fitted of %2").arg(w->enabledPanelCount()).arg(w->cols * w->rows))
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

    auto *cab = new QGroupBox(tr("Cabinet layouts"));
    auto *cabl = new QVBoxLayout(cab);
    m_cabinets = new CabinetLayoutEditor(m_doc);
    cabl->addWidget(m_cabinets);
    v->addWidget(cab);

    auto *masks = new QGroupBox(tr("Projection masks"));
    auto *ml = new QVBoxLayout(masks);
    m_masks = new MaskEditor(m_doc);
    ml->addWidget(m_masks);
    v->addWidget(masks);
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
    m_display->addItem(Ndi::available() ? tr("NDI (network video source)") : tr("NDI (runtime not installed)"),
                       QStringLiteral("ndi"));
    if (!TextureShare::technology().isEmpty())
        m_display->addItem(tr("%1 (share with other apps on this machine)").arg(TextureShare::technology()),
                           QStringLiteral("share"));
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
    if (!s->cabinetLayouts.isEmpty())
        CabinetMapping::regenerate(m_doc->project()); // packed layouts wrap at the screen's size
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
    m_cabinets->setScreen(m_doc->selection().screen);
    m_masks->setScreen(m_doc->selection().screen);
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
    m_angle = makeDouble(-180, 180, 1, QStringLiteral("\u00B0"));
    m_angle->setSingleStep(1.0);
    m_angle->setWrapping(true);
    m_angle->setToolTip(tr("Turns the sampled area of the composition about its centre (clockwise), for walls "
                           "hung at an angle such as diamonds. The output stays upright."));
    inf->addRow(tr("Angle"), m_angle);
    m_cabinetInfo = new QLabel;
    m_cabinetInfo->setWordWrap(true);
    inf->addRow(m_cabinetInfo);
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

    // Projection: warping onto the surface and soft edges where projectors overlap.
    auto *proj = new QGroupBox(tr("Warp and edge blend"));
    auto *pf = new QFormLayout(proj);
    m_warpMode = new QComboBox;
    m_warpMode->addItems({tr("None"), tr("Corner pin"), tr("Mesh")});
    m_warpMode->setToolTip(tr("Corner pin: drag the four numbered corners on the output map (with perspective).\n"
                              "Mesh: drag the grid points to fit curved or uneven surfaces."));
    m_meshW = makeSpin(2, 33, QString());
    m_meshH = makeSpin(2, 33, QString());
    m_meshW->setToolTip(tr("Mesh points across"));
    m_meshH->setToolTip(tr("Mesh points down"));
    m_meshReset = new QPushButton(tr("Flatten"));
    m_meshReset->setToolTip(tr("Reset the mesh to the corner pin / output rect, keeping its size"));
    pf->addRow(tr("Warp"), m_warpMode);
    pf->addRow(tr("Mesh points"), row({m_meshW, m_meshH, m_meshReset}));
    auto blendSpin = [] {
        QDoubleSpinBox *s = makeDouble(0, 8192, 0, QStringLiteral(" px"));
        s->setToolTip(QObject::tr("Soft-edge width on this side, in output pixels (the overlap with the neighbouring projector)"));
        return s;
    };
    m_blendL = blendSpin();
    m_blendR = blendSpin();
    m_blendT = blendSpin();
    m_blendB = blendSpin();
    m_blendCurve = makeDouble(0.5, 8, 2, QString());
    m_blendCurve->setSingleStep(0.1);
    m_blendCurve->setToolTip(tr("Shape of the ramp: 1 = linear, higher = steeper S-curve"));
    m_blendGamma = makeDouble(1, 3.5, 2, QString());
    m_blendGamma->setSingleStep(0.05);
    m_blendGamma->setToolTip(tr("Projector gamma the ramp compensates, so overlaps add up to even brightness"));
    pf->addRow(tr("Blend left / right"), row({m_blendL, m_blendR}));
    pf->addRow(tr("Blend top / bottom"), row({m_blendT, m_blendB}));
    pf->addRow(tr("Curve / gamma"), row({m_blendCurve, m_blendGamma}));
    v->addWidget(proj);

    auto *colour = new QGroupBox(tr("Colour correction"));
    colour->setToolTip(tr("Match cabinets from different batches, or this wall to its neighbours"));
    auto *cf = new QFormLayout(colour);
    m_gainR = makeDouble(0, 200, 1, QStringLiteral(" %"));
    m_gainG = makeDouble(0, 200, 1, QStringLiteral(" %"));
    m_gainB = makeDouble(0, 200, 1, QStringLiteral(" %"));
    m_gamma = makeDouble(0.2, 3.0, 2, QString());
    m_gamma->setSingleStep(0.05);
    m_gamma->setToolTip(tr("Exponent applied after the gains: above 1 darkens mid-tones, below 1 lifts them"));
    m_blackLevel = makeDouble(0, 50, 1, QStringLiteral(" %"));
    auto *resetColour = new QPushButton(tr("Reset"));
    cf->addRow(tr("Red gain"), m_gainR);
    cf->addRow(tr("Green gain"), m_gainG);
    cf->addRow(tr("Blue gain"), m_gainB);
    cf->addRow(tr("Gamma"), m_gamma);
    cf->addRow(tr("Black level"), m_blackLevel);
    cf->addRow(resetColour);
    v->addWidget(colour);
    v->addStretch();

    auto colourField = [this](QDoubleSpinBox *s, double Slice::*field, double scale) {
        connect(s, &QDoubleSpinBox::valueChanged, this, [this, field, scale](double value) {
            if (!m_refreshing)
                editSlice([&](Slice &sl) { sl.*field = value / scale; });
        });
    };
    colourField(m_gainR, &Slice::gainR, 100.0);
    colourField(m_gainG, &Slice::gainG, 100.0);
    colourField(m_gainB, &Slice::gainB, 100.0);
    colourField(m_gamma, &Slice::gamma, 1.0);
    colourField(m_blackLevel, &Slice::blackLevel, 100.0);
    connect(resetColour, &QPushButton::clicked, this, [this] {
        editSlice([](Slice &sl) {
            sl.gainR = sl.gainG = sl.gainB = 1.0;
            sl.gamma = 1.0;
            sl.blackLevel = 0.0;
        });
    });

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

    connect(m_angle, &QDoubleSpinBox::valueChanged, this, [this](double a) {
        if (!m_refreshing)
            editSlice([&](Slice &s) { s.angle = a; });
    });
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
    connect(m_warpMode, &QComboBox::activated, this, [this](int mode) {
        editSlice([&](Slice &s) {
            const QRectF r(s.output);
            const QPolygonF rect({r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()});
            if (mode == 0) {
                s.warp.clear();
                s.mesh.clear();
                s.meshW = s.meshH = 0;
            } else if (mode == 1) {
                if (!s.isWarped())
                    s.warp = s.isMeshed() ? s.outputQuad() : rect; // a mesh hands over its corners
                s.mesh.clear();
                s.meshW = s.meshH = 0;
            } else if (!s.isMeshed()) {
                s.resetMesh(5, 5); // starts flat over the current shape (corner pin included)
            }
        });
    });
    auto meshSize = [this] {
        if (m_refreshing)
            return;
        editSlice([&](Slice &s) {
            if (s.isMeshed())
                s.resizeMesh(m_meshW->value(), m_meshH->value()); // keeps the current shape
        });
    };
    connect(m_meshW, &QSpinBox::valueChanged, this, meshSize);
    connect(m_meshH, &QSpinBox::valueChanged, this, meshSize);
    connect(m_meshReset, &QPushButton::clicked, this, [this] {
        editSlice([&](Slice &s) {
            if (s.isMeshed())
                s.resetMesh(s.meshW, s.meshH);
        });
    });
    auto blendField = [this](QDoubleSpinBox *spin, double Slice::*field) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this, field](double v) {
            if (!m_refreshing)
                editSlice([&](Slice &s) { s.*field = v; });
        });
    };
    blendField(m_blendL, &Slice::blendLeft);
    blendField(m_blendR, &Slice::blendRight);
    blendField(m_blendT, &Slice::blendTop);
    blendField(m_blendB, &Slice::blendBottom);
    blendField(m_blendCurve, &Slice::blendCurve);
    blendField(m_blendGamma, &Slice::blendGamma);
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
        const double angle = p.walls[w].angle;
        editSlice([&](Slice &s) {
            s.input = r;
            s.angle = angle;
        });
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
    setValue(m_angle, s->angle);
    m_cabinetInfo->setVisible(s->isCabinet());
    if (s->isCabinet())
        m_cabinetInfo->setText(tr("Cabinet C%1 R%2%3, generated by the screen's cabinet layout: position, rotation "
                                  "and flips follow the layout; brightness, colour and angle are kept per cabinet.")
                                   .arg(s->cabinet.x() + 1)
                                   .arg(s->cabinet.y() + 1)
                                   .arg(s->port > 0 ? tr(" on port %1").arg(s->port) : QString()));
    m_flipH->setChecked(s->flipH);
    m_flipV->setChecked(s->flipV);
    m_warpMode->setCurrentIndex(s->isMeshed() ? 2 : (s->isWarped() ? 1 : 0));
    setValue(m_meshW, s->isMeshed() ? s->meshW : 5);
    setValue(m_meshH, s->isMeshed() ? s->meshH : 5);
    m_meshW->setEnabled(s->isMeshed());
    m_meshH->setEnabled(s->isMeshed());
    m_meshReset->setEnabled(s->isMeshed());
    setValue(m_blendL, s->blendLeft);
    setValue(m_blendR, s->blendRight);
    setValue(m_blendT, s->blendTop);
    setValue(m_blendB, s->blendBottom);
    setValue(m_blendCurve, s->blendCurve);
    setValue(m_blendGamma, s->blendGamma);
    setValue(m_brightness, qRound(s->brightness * 100.0));
    setValue(m_gainR, s->gainR * 100.0);
    setValue(m_gainG, s->gainG * 100.0);
    setValue(m_gainB, s->gainB * 100.0);
    setValue(m_gamma, s->gamma);
    setValue(m_blackLevel, s->blackLevel * 100.0);

    const Project &p = m_doc->project();
    const int prev = m_sliceWall->currentData().toInt();
    m_sliceWall->clear();
    for (int i = 0; i < p.walls.size(); ++i)
        m_sliceWall->addItem(p.walls[i].name, i);
    int match = -1;
    for (int i = 0; i < p.walls.size(); ++i)
        if (p.walls[i].rect() == s->input && p.walls[i].angle == s->angle)
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
    case Selection::Layer:
        m_stack->setCurrentIndex(LayerPage);
        refreshLayer();
        break;
    case Selection::Clip:
        m_stack->setCurrentIndex(ClipPage);
        refreshLayer();
        refreshClip();
        break;
    default:
        m_stack->setCurrentIndex(CompositionPage);
        refreshComposition();
        break;
    }
    m_refreshing = false;
}