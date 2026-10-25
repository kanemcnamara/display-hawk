#include "ui/PixelMapDialog.h"

#include "control/PixelMapper.h"
#include "model/Document.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {

QDoubleSpinBox *coord()
{
    auto *s = new QDoubleSpinBox;
    s->setRange(-100000, 100000);
    s->setDecimals(0);
    s->setSuffix(QStringLiteral(" px"));
    return s;
}

QWidget *pair(QWidget *a, QWidget *b)
{
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->addWidget(a);
    h->addWidget(b);
    return w;
}

} // namespace

PixelMapDialog::PixelMapDialog(Document *doc, PixelMapper *mapper, QWidget *parent)
    : QDialog(parent)
    , m_doc(doc)
    , m_mapper(mapper)
{
    setWindowTitle(tr("Pixel mapping (DMX out)"));
    resize(820, 520);
    auto *v = new QVBoxLayout(this);

    auto *top = new QHBoxLayout;
    m_enabled = new QCheckBox(tr("Send"));
    m_protocol = new QComboBox;
    m_protocol->addItems({tr("Art-Net"), tr("sACN (E1.31)")});
    m_target = new QLineEdit;
    m_target->setPlaceholderText(tr("broadcast / multicast"));
    m_target->setToolTip(tr("A node's IP address (optionally :port). Empty: Art-Net broadcast, or each universe's sACN multicast group."));
    m_rate = new QSpinBox;
    m_rate->setRange(1, 60);
    m_rate->setSuffix(tr(" fps"));
    m_status = new QLabel;
    top->addWidget(m_enabled);
    top->addWidget(m_protocol);
    top->addWidget(new QLabel(tr("to")));
    top->addWidget(m_target, 1);
    top->addWidget(m_rate);
    top->addWidget(m_status);
    v->addLayout(top);

    auto *body = new QHBoxLayout;
    auto *left = new QVBoxLayout;
    m_list = new QListWidget;
    auto *addStrip = new QPushButton(tr("+ Strip"));
    auto *addMatrix = new QPushButton(tr("+ Matrix"));
    auto *addPoint = new QPushButton(tr("+ Fixture"));
    auto *remove = new QPushButton(tr("Remove"));
    auto *adds = new QHBoxLayout;
    adds->addWidget(addStrip);
    adds->addWidget(addMatrix);
    adds->addWidget(addPoint);
    left->addWidget(m_list, 1);
    left->addLayout(adds);
    left->addWidget(remove);
    body->addLayout(left, 1);

    m_form = new QWidget;
    auto *f = new QFormLayout(m_form);
    m_name = new QLineEdit;
    m_fixtureOn = new QCheckBox(tr("On"));
    m_kind = new QComboBox;
    for (int k = 0; k < 3; ++k)
        m_kind->addItem(fixtureKindName(FixtureKind(k)));
    m_x1 = coord(), m_y1 = coord(), m_x2 = coord(), m_y2 = coord(), m_w = coord(), m_h = coord();
    m_pixels = new QSpinBox;
    m_pixels->setRange(1, 100000);
    m_columns = new QSpinBox;
    m_columns->setRange(1, 4096);
    m_rows = new QSpinBox;
    m_rows->setRange(1, 4096);
    m_serpentine = new QCheckBox(tr("Serpentine (rows alternate direction)"));
    m_universe = new QSpinBox;
    m_universe->setRange(0, 63999);
    m_address = new QSpinBox;
    m_address->setRange(1, 512);
    m_order = new QComboBox;
    for (int o = 0; o < kColorOrderCount; ++o)
        m_order->addItem(colorOrderName(ColorOrder(o)));
    m_channels = new QLabel;
    m_channels->setWordWrap(true);
    f->addRow(tr("Name"), pair(m_name, m_fixtureOn));
    f->addRow(tr("Type"), m_kind);
    f->addRow(tr("Start X / Y"), pair(m_x1, m_y1));
    f->addRow(tr("End X / Y"), pair(m_x2, m_y2));
    f->addRow(tr("Width / height"), pair(m_w, m_h));
    f->addRow(tr("Pixels"), m_pixels);
    f->addRow(tr("Columns / rows"), pair(m_columns, m_rows));
    f->addRow(QString(), m_serpentine);
    f->addRow(tr("Universe / address"), pair(m_universe, m_address));
    f->addRow(tr("Colour order"), m_order);
    f->addRow(m_channels);
    body->addWidget(m_form, 2);
    v->addLayout(body, 1);

    auto *help = new QLabel(tr("Fixtures sample the composition as the outputs show it (master and blackout included). "
                               "A fixture longer than a universe carries on in the next one. They show on the input map."));
    help->setWordWrap(true);
    help->setEnabled(false);
    v->addWidget(help);

    auto settings = [this](const std::function<void(DmxOutputSettings &)> &fn) {
        if (m_refreshing)
            return;
        fn(m_doc->project().dmxOut);
        m_doc->notifyChanged();
    };
    connect(m_enabled, &QCheckBox::toggled, this, [settings](bool on) { settings([on](DmxOutputSettings &o) { o.enabled = on; }); });
    connect(m_protocol, &QComboBox::activated, this, [settings](int i) { settings([i](DmxOutputSettings &o) { o.protocol = DmxProtocol(i); }); });
    connect(m_target, &QLineEdit::editingFinished, this, [this, settings] {
        const QString t = m_target->text();
        settings([t](DmxOutputSettings &o) { o.target = t; });
    });
    connect(m_rate, &QSpinBox::valueChanged, this, [settings](int r) { settings([r](DmxOutputSettings &o) { o.rate = r; }); });

    connect(addStrip, &QPushButton::clicked, this, [this] { addFixture(0); });
    connect(addMatrix, &QPushButton::clicked, this, [this] { addFixture(1); });
    connect(addPoint, &QPushButton::clicked, this, [this] { addFixture(2); });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int i = m_list->currentRow();
        QVector<PixelFixture> &list = m_doc->project().fixtures;
        if (i >= 0 && i < list.size()) {
            list.removeAt(i);
            m_doc->notifyChanged(true);
        }
    });
    connect(m_list, &QListWidget::currentRowChanged, this, &PixelMapDialog::refreshFixture);

    connect(m_name, &QLineEdit::textEdited, this, [this](const QString &t) { edit([&](PixelFixture &x) { x.name = t; }, true); });
    connect(m_fixtureOn, &QCheckBox::toggled, this, [this](bool on) { edit([on](PixelFixture &x) { x.enabled = on; }); });
    connect(m_kind, &QComboBox::activated, this, [this](int k) { edit([k](PixelFixture &x) { x.kind = FixtureKind(k); }, true); });
    auto number = [this](QDoubleSpinBox *s, const std::function<void(PixelFixture &, double)> &set) {
        connect(s, &QDoubleSpinBox::valueChanged, this, [this, set](double v) { edit([&](PixelFixture &x) { set(x, v); }); });
    };
    // Start X/Y: a strip's first pixel or a fixture's point; a matrix's top-left corner.
    number(m_x1, [](PixelFixture &x, double v) {
        if (x.kind == FixtureKind::Matrix)
            x.rect.moveLeft(v);
        else
            x.start.setX(v);
    });
    number(m_y1, [](PixelFixture &x, double v) {
        if (x.kind == FixtureKind::Matrix)
            x.rect.moveTop(v);
        else
            x.start.setY(v);
    });
    number(m_x2, [](PixelFixture &x, double v) { x.end.setX(v); });
    number(m_y2, [](PixelFixture &x, double v) { x.end.setY(v); });
    number(m_w, [](PixelFixture &x, double v) { x.rect.setWidth(v); });
    number(m_h, [](PixelFixture &x, double v) { x.rect.setHeight(v); });
    auto integer = [this](QSpinBox *s, const std::function<void(PixelFixture &, int)> &set) {
        connect(s, &QSpinBox::valueChanged, this, [this, set](int v) { edit([&](PixelFixture &x) { set(x, v); }); });
    };
    integer(m_pixels, [](PixelFixture &x, int v) { x.pixels = v; });
    integer(m_columns, [](PixelFixture &x, int v) { x.columns = v; });
    integer(m_rows, [](PixelFixture &x, int v) { x.rows = v; });
    integer(m_universe, [](PixelFixture &x, int v) { x.universe = v; });
    integer(m_address, [](PixelFixture &x, int v) { x.address = v; });
    connect(m_serpentine, &QCheckBox::toggled, this, [this](bool on) { edit([on](PixelFixture &x) { x.serpentine = on; }); });
    connect(m_order, &QComboBox::activated, this, [this](int o) { edit([o](PixelFixture &x) { x.order = ColorOrder(o); }); });
    connect(m_doc, &Document::changed, this, &PixelMapDialog::refresh);
    connect(m_mapper, &PixelMapper::sent, this, [this](int universes) {
        m_status->setText(tr("%n universe(s)", nullptr, universes));
    });
    refresh();
}

void PixelMapDialog::addFixture(int kind)
{
    const Project &p = m_doc->project();
    PixelFixture f;
    f.kind = FixtureKind(kind);
    f.name = tr("%1 %2").arg(fixtureKindName(f.kind)).arg(p.fixtures.size() + 1);
    // Next free channels: after the last fixture.
    int universe = 1, channel = 0;
    for (const PixelFixture &o : p.fixtures) {
        const int end = (o.universe - 1) * 512 + (o.address - 1) + o.pixelCount() * channelsPerPixel(o.order);
        if (end > (universe - 1) * 512 + channel) {
            universe = end / 512 + 1;
            channel = end % 512;
        }
    }
    f.universe = universe;
    f.address = channel + 1;
    const QSizeF c = p.compSize();
    f.start = QPointF(c.width() * 0.1, c.height() * 0.5);
    f.end = QPointF(c.width() * 0.9, c.height() * 0.5);
    f.rect = QRectF(c.width() * 0.25, c.height() * 0.25, c.width() * 0.5, c.height() * 0.5);
    if (f.kind == FixtureKind::Point)
        f.start = QPointF(c.width() / 2, c.height() / 2);
    m_doc->project().fixtures.append(f);
    m_doc->notifyChanged(true);
    m_list->setCurrentRow(int(m_doc->project().fixtures.size()) - 1);
}

void PixelMapDialog::edit(const std::function<void(PixelFixture &)> &fn, bool structural)
{
    const int i = m_list->currentRow();
    QVector<PixelFixture> &list = m_doc->project().fixtures;
    if (m_refreshing || i < 0 || i >= list.size())
        return;
    fn(list[i]);
    m_doc->notifyChanged(structural);
}

void PixelMapDialog::refresh()
{
    const Project &p = m_doc->project();
    m_refreshing = true;
    m_enabled->setChecked(p.dmxOut.enabled);
    m_protocol->setCurrentIndex(int(p.dmxOut.protocol));
    if (!m_target->hasFocus())
        m_target->setText(p.dmxOut.target);
    m_rate->setValue(p.dmxOut.rate);
    const int row = m_list->currentRow();
    if (m_list->count() != p.fixtures.size()) {
        m_list->clear();
        for (const PixelFixture &f : p.fixtures)
            m_list->addItem(f.name);
        m_list->setCurrentRow(qBound(-1, row, int(p.fixtures.size()) - 1));
    } else {
        for (int i = 0; i < p.fixtures.size(); ++i)
            m_list->item(i)->setText(p.fixtures[i].name);
    }
    m_refreshing = false;
    refreshFixture();
}

void PixelMapDialog::refreshFixture()
{
    const int i = m_list->currentRow();
    const QVector<PixelFixture> &list = m_doc->project().fixtures;
    m_form->setEnabled(i >= 0 && i < list.size());
    if (i < 0 || i >= list.size())
        return;
    const PixelFixture &f = list[i];
    m_refreshing = true;
    if (m_name->text() != f.name)
        m_name->setText(f.name);
    m_fixtureOn->setChecked(f.enabled);
    m_kind->setCurrentIndex(int(f.kind));
    const bool strip = f.kind == FixtureKind::Strip, matrix = f.kind == FixtureKind::Matrix;
    m_x1->setValue(matrix ? f.rect.x() : f.start.x());
    m_y1->setValue(matrix ? f.rect.y() : f.start.y());
    m_x2->setValue(f.end.x());
    m_y2->setValue(f.end.y());
    m_w->setValue(f.rect.width());
    m_h->setValue(f.rect.height());
    m_x2->setEnabled(strip);
    m_y2->setEnabled(strip);
    m_w->setEnabled(matrix);
    m_h->setEnabled(matrix);
    m_pixels->setValue(f.pixels);
    m_pixels->setEnabled(strip);
    m_columns->setValue(f.columns);
    m_rows->setValue(f.rows);
    m_columns->setEnabled(matrix);
    m_rows->setEnabled(matrix);
    m_serpentine->setChecked(f.serpentine);
    m_serpentine->setEnabled(matrix);
    m_universe->setValue(f.universe);
    m_address->setValue(f.address);
    m_order->setCurrentIndex(int(f.order));
    const int channels = f.pixelCount() * channelsPerPixel(f.order);
    const int last = (f.address - 1) + channels;
    m_channels->setText(tr("%1 pixels, %2 channels: universe %3 channel %4 to universe %5 channel %6")
                            .arg(f.pixelCount()).arg(channels).arg(f.universe).arg(f.address)
                            .arg(f.universe + (last - 1) / 512).arg((last - 1) % 512 + 1));
    m_refreshing = false;
}
