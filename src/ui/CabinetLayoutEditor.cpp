#include "ui/CabinetLayoutEditor.h"

#include "model/CabinetMapping.h"
#include "model/Document.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

struct PortPreset {
    const char *label;
    int pixels;
};
// Typical pixel capacity per port of LED processors at 60 Hz, 8-bit.
const PortPreset kPortPresets[] = {
    {"No port split", 0},
    {"1G port (655,360 px)", 655360},
    {"5G port (3,276,800 px)", 3276800},
    {"10G port (6,553,600 px)", 6553600},
};

const char *const kCorners[] = {"Top-left", "Top-right", "Bottom-left", "Bottom-right"};

QSpinBox *spin(int min, int max, const QString &suffix = QStringLiteral(" px"))
{
    auto *s = new QSpinBox;
    s->setRange(min, max);
    s->setSuffix(suffix);
    s->setAccelerated(true);
    return s;
}

template <typename T>
void quietSet(T *w, const std::function<void(T *)> &fn)
{
    const QSignalBlocker b(w);
    fn(w);
}

} // namespace

CabinetLayoutEditor::CabinetLayoutEditor(Document *doc, QWidget *parent)
    : QWidget(parent)
    , m_doc(doc)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    m_list = new QVBoxLayout;
    m_list->setSpacing(6);
    v->addLayout(m_list);

    auto *addRow = new QHBoxLayout;
    m_addWall = new QComboBox;
    auto *add = new QPushButton(tr("Map cabinets"));
    add->setToolTip(tr("One slice per fitted cabinet of the wall, laid out on this screen and chained onto ports"));
    addRow->addWidget(m_addWall, 1);
    addRow->addWidget(add);
    v->addLayout(addRow);
    connect(add, &QPushButton::clicked, this, [this] {
        const int wall = m_addWall->currentData().toInt();
        if (m_screen < 0 || wall < 0)
            return;
        CabinetLayout layout;
        layout.wall = wall;
        m_doc->addCabinetLayout(m_screen, layout);
    });
}

void CabinetLayoutEditor::setScreen(int screen)
{
    m_screen = screen;
    refresh();
}

void CabinetLayoutEditor::edit(int index, const std::function<void(CabinetLayout &)> &fn)
{
    if (m_refreshing || m_screen < 0 || m_screen >= m_doc->project().screens.size())
        return;
    const QVector<CabinetLayout> &layouts = m_doc->project().screens[m_screen].cabinetLayouts;
    if (index < 0 || index >= layouts.size())
        return;
    CabinetLayout layout = layouts[index];
    fn(layout);
    m_doc->setCabinetLayout(m_screen, index, layout);
}

void CabinetLayoutEditor::refresh()
{
    const Project &p = m_doc->project();
    {
        const QSignalBlocker b(m_addWall);
        const int prev = m_addWall->currentData().toInt();
        m_addWall->clear();
        for (int i = 0; i < p.walls.size(); ++i)
            m_addWall->addItem(p.walls[i].name, i);
        if (p.walls.isEmpty())
            m_addWall->addItem(tr("(no LED walls)"), -1);
        m_addWall->setCurrentIndex(qMax(0, m_addWall->findData(prev)));
    }
    QVector<int> walls;
    if (m_screen >= 0 && m_screen < p.screens.size())
        for (const CabinetLayout &c : p.screens[m_screen].cabinetLayouts)
            walls << c.wall;
    if (walls != m_builtFor || m_screen != m_builtScreen) {
        rebuild();
        return;
    }
    m_refreshing = true;
    for (const auto &u : std::as_const(m_updaters))
        u();
    m_refreshing = false;
}

void CabinetLayoutEditor::rebuild()
{
    while (QLayoutItem *item = m_list->takeAt(0)) {
        if (QWidget *w = item->widget()) { // deferred: this can run from one of their signals
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
    m_updaters.clear();
    m_builtFor.clear();
    m_builtScreen = m_screen;
    const Project &p = m_doc->project();
    if (m_screen < 0 || m_screen >= p.screens.size())
        return;
    const QVector<CabinetLayout> &layouts = p.screens[m_screen].cabinetLayouts;
    m_refreshing = true;
    if (layouts.isEmpty()) {
        auto *none = new QLabel(tr("No cabinet layouts. Map a wall's cabinets to place each one on this output "
                                   "individually: packed for the processor, rotated, chained onto ports."));
        none->setWordWrap(true);
        none->setEnabled(false);
        m_list->addWidget(none);
    }
    for (int i = 0; i < layouts.size(); ++i) {
        m_builtFor << layouts[i].wall;
        auto *frame = new QFrame;
        frame->setFrameShape(QFrame::StyledPanel);
        auto *fl = new QVBoxLayout(frame);
        fl->setContentsMargins(6, 4, 6, 6);
        auto *header = new QHBoxLayout;
        auto *title = new QLabel;
        QFont bold = title->font();
        bold.setBold(true);
        title->setFont(bold);
        auto *remove = new QToolButton;
        remove->setText(QStringLiteral("✕"));
        remove->setAutoRaise(true);
        remove->setToolTip(tr("Remove this layout and its cabinet slices"));
        connect(remove, &QToolButton::clicked, this, [this, i] { m_doc->removeCabinetLayout(m_screen, i); });
        header->addWidget(title, 1);
        header->addWidget(remove);
        fl->addLayout(header);

        auto *form = new QFormLayout;
        form->setContentsMargins(0, 0, 0, 0);
        fl->addLayout(form);
        auto *arrange = new QComboBox;
        for (int a = 0; a < 3; ++a)
            arrange->addItem(cabinetArrangeName(CabinetArrange(a)));
        auto *order = new QComboBox;
        for (int o = 0; o < 4; ++o)
            order->addItem(cabinetOrderName(CabinetOrder(o)));
        auto *corner = new QComboBox;
        for (const char *c : kCorners)
            corner->addItem(tr(c));
        auto *x = spin(-16384, 16384), *y = spin(-16384, 16384), *gap = spin(0, 1024);
        auto *rotation = new QComboBox;
        rotation->addItems({tr("0°"), tr("90° clockwise"), tr("180°"), tr("90° anticlockwise")});
        auto *flipH = new QCheckBox(tr("Flip H"));
        auto *flipV = new QCheckBox(tr("Flip V"));
        auto *portPreset = new QComboBox;
        for (const PortPreset &pp : kPortPresets)
            portPreset->addItem(tr(pp.label), pp.pixels);
        portPreset->addItem(tr("Custom"), -1);
        auto *capacity = spin(0, 100000000);
        capacity->setSingleStep(10000);
        auto *firstPort = spin(1, 999, QString());
        auto *newLine = new QCheckBox(tr("Each port starts a new row"));
        auto *summary = new QLabel;
        summary->setWordWrap(true);

        auto pair = [](QWidget *a, QWidget *b) {
            auto *w = new QWidget;
            auto *h = new QHBoxLayout(w);
            h->setContentsMargins(0, 0, 0, 0);
            h->addWidget(a);
            h->addWidget(b);
            return w;
        };
        form->addRow(tr("Arrange"), arrange);
        form->addRow(tr("Chain"), pair(order, corner));
        form->addRow(tr("Position"), pair(x, y));
        form->addRow(tr("Gap"), gap);
        form->addRow(tr("Rotate"), rotation);
        form->addRow(QString(), pair(flipH, flipV));
        form->addRow(tr("Port size"), pair(portPreset, capacity));
        form->addRow(tr("First port"), firstPort);
        form->addRow(QString(), newLine);
        form->addRow(summary);
        order->setToolTip(tr("The order the processor's data runs through the cabinets, from the chosen corner of the wall"));
        corner->setToolTip(tr("Where the data cable enters the wall"));
        gap->setToolTip(tr("Space between cabinets on the output (some processors need it)"));
        capacity->setToolTip(tr("Pixels one port can drive; cabinets are split onto ports in chain order"));

        connect(arrange, &QComboBox::activated, this, [this, i](int a) { edit(i, [a](CabinetLayout &c) { c.arrange = CabinetArrange(a); }); });
        connect(order, &QComboBox::activated, this, [this, i](int o) { edit(i, [o](CabinetLayout &c) { c.order = CabinetOrder(o); }); });
        connect(corner, &QComboBox::activated, this, [this, i](int k) { edit(i, [k](CabinetLayout &c) { c.startCorner = k; }); });
        connect(x, &QSpinBox::valueChanged, this, [this, i](int vx) { edit(i, [vx](CabinetLayout &c) { c.x = vx; }); });
        connect(y, &QSpinBox::valueChanged, this, [this, i](int vy) { edit(i, [vy](CabinetLayout &c) { c.y = vy; }); });
        connect(gap, &QSpinBox::valueChanged, this, [this, i](int g) { edit(i, [g](CabinetLayout &c) { c.gap = g; }); });
        connect(rotation, &QComboBox::activated, this, [this, i](int r) { edit(i, [r](CabinetLayout &c) { c.rotation = r; }); });
        connect(flipH, &QCheckBox::toggled, this, [this, i](bool on) { edit(i, [on](CabinetLayout &c) { c.flipH = on; }); });
        connect(flipV, &QCheckBox::toggled, this, [this, i](bool on) { edit(i, [on](CabinetLayout &c) { c.flipV = on; }); });
        connect(portPreset, &QComboBox::activated, this, [this, i, portPreset](int k) {
            const int px = portPreset->itemData(k).toInt();
            if (px >= 0)
                edit(i, [px](CabinetLayout &c) { c.portCapacity = px; });
        });
        connect(capacity, &QSpinBox::valueChanged, this, [this, i](int px) { edit(i, [px](CabinetLayout &c) { c.portCapacity = px; }); });
        connect(firstPort, &QSpinBox::valueChanged, this, [this, i](int fp) { edit(i, [fp](CabinetLayout &c) { c.firstPort = fp; }); });
        connect(newLine, &QCheckBox::toggled, this, [this, i](bool on) { edit(i, [on](CabinetLayout &c) { c.newLinePerPort = on; }); });

        auto update = [=] {
            const Project &pr = m_doc->project();
            if (m_screen < 0 || m_screen >= pr.screens.size() || i >= pr.screens[m_screen].cabinetLayouts.size())
                return;
            const Screen &sc = pr.screens[m_screen];
            const CabinetLayout &c = sc.cabinetLayouts[i];
            const QString wallName = c.wall >= 0 && c.wall < pr.walls.size() ? pr.walls[c.wall].name : tr("(missing wall)");
            title->setText(wallName);
            quietSet<QComboBox>(arrange, [&](QComboBox *w) { w->setCurrentIndex(int(c.arrange)); });
            quietSet<QComboBox>(order, [&](QComboBox *w) { w->setCurrentIndex(int(c.order)); });
            quietSet<QComboBox>(corner, [&](QComboBox *w) { w->setCurrentIndex(c.startCorner); });
            quietSet<QSpinBox>(x, [&](QSpinBox *w) { w->setValue(c.x); });
            quietSet<QSpinBox>(y, [&](QSpinBox *w) { w->setValue(c.y); });
            quietSet<QSpinBox>(gap, [&](QSpinBox *w) { w->setValue(c.gap); });
            quietSet<QComboBox>(rotation, [&](QComboBox *w) { w->setCurrentIndex(c.rotation & 3); });
            quietSet<QCheckBox>(flipH, [&](QCheckBox *w) { w->setChecked(c.flipH); });
            quietSet<QCheckBox>(flipV, [&](QCheckBox *w) { w->setChecked(c.flipV); });
            quietSet<QComboBox>(portPreset, [&](QComboBox *w) {
                const int k = w->findData(c.portCapacity);
                w->setCurrentIndex(k >= 0 ? k : w->count() - 1);
            });
            quietSet<QSpinBox>(capacity, [&](QSpinBox *w) { w->setValue(c.portCapacity); });
            quietSet<QSpinBox>(firstPort, [&](QSpinBox *w) { w->setValue(c.firstPort); });
            quietSet<QCheckBox>(newLine, [&](QCheckBox *w) {
                w->setChecked(c.newLinePerPort);
                w->setText(c.arrange == CabinetArrange::PackColumns ? tr("Each port starts a new column")
                                                                    : tr("Each port starts a new row"));
                w->setEnabled(c.arrange != CabinetArrange::WallShape && c.portCapacity > 0);
            });
            capacity->setEnabled(c.portCapacity != 0);
            firstPort->setEnabled(c.portCapacity > 0);
            const CabinetMapping::Summary s = CabinetMapping::summarize(sc, c.wall);
            QString text = tr("%n cabinet(s)", nullptr, s.cabinets);
            if (s.ports > 0)
                text += tr(", %n port(s)", nullptr, s.ports);
            if (s.outside > 0)
                text += QStringLiteral(" — <span style='color:#ff6060'>%1</span>")
                            .arg(tr("%n cabinet(s) do not fit on this screen", nullptr, s.outside));
            summary->setText(text);
        };
        update();
        m_updaters.append(update);
        m_list->addWidget(frame);
    }
    m_refreshing = false;
}
