#include "ui/MaskEditor.h"

#include "model/Document.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

MaskEditor::MaskEditor(Document *doc, QWidget *parent)
    : QWidget(parent)
    , m_doc(doc)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    m_list = new QVBoxLayout;
    m_list->setSpacing(4);
    v->addLayout(m_list);
    auto *add = new QPushButton(tr("Add mask"));
    add->setToolTip(tr("Black out part of this output (or everything outside a shape) with a soft edge"));
    v->addWidget(add);
    auto *help = new QLabel(tr("Edit a mask to drag its points on the output map; double-click an edge to add a "
                               "point, Alt+click a point to remove it, drag inside to move the whole mask."));
    help->setWordWrap(true);
    help->setEnabled(false);
    v->addWidget(help);

    connect(add, &QPushButton::clicked, this, [this] {
        Project &p = m_doc->project();
        if (m_screen < 0 || m_screen >= p.screens.size())
            return;
        Screen &s = p.screens[m_screen];
        ProjectionMask m;
        m.name = tr("Mask %1").arg(s.masks.size() + 1);
        const QRectF r(s.width / 4.0, s.height / 4.0, s.width / 2.0, s.height / 2.0);
        m.points = QPolygonF({r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()});
        s.masks.append(m);
        m_doc->notifyChanged(true);
        m_doc->setActiveMask(m_screen, int(s.masks.size()) - 1);
    });
    connect(m_doc, &Document::selectionChanged, this, &MaskEditor::refresh);
}

void MaskEditor::setScreen(int screen)
{
    if (screen != m_screen && m_doc->activeMaskScreen() != screen)
        m_doc->setActiveMask(-1, -1); // stop editing another screen's mask
    m_screen = screen;
    refresh();
}

void MaskEditor::edit(int index, const std::function<void(ProjectionMask &)> &fn)
{
    Project &p = m_doc->project();
    if (m_refreshing || m_screen < 0 || m_screen >= p.screens.size() || index < 0
        || index >= p.screens[m_screen].masks.size())
        return;
    fn(p.screens[m_screen].masks[index]);
    m_doc->notifyChanged();
}

void MaskEditor::refresh()
{
    const Project &p = m_doc->project();
    const int count = m_screen >= 0 && m_screen < p.screens.size() ? int(p.screens[m_screen].masks.size()) : 0;
    if (count != m_builtCount || m_screen != m_builtScreen) {
        rebuild();
        return;
    }
    m_refreshing = true;
    for (const auto &u : std::as_const(m_updaters))
        u();
    m_refreshing = false;
}

void MaskEditor::rebuild()
{
    while (QLayoutItem *item = m_list->takeAt(0)) {
        if (QWidget *w = item->widget()) { // deferred: this can run from one of their signals
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
    m_updaters.clear();
    const Project &p = m_doc->project();
    m_builtScreen = m_screen;
    m_builtCount = m_screen >= 0 && m_screen < p.screens.size() ? int(p.screens[m_screen].masks.size()) : 0;
    m_refreshing = true;
    for (int i = 0; i < m_builtCount; ++i) {
        auto *row = new QWidget;
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto *name = new QLineEdit;
        auto *enabled = new QCheckBox;
        enabled->setToolTip(tr("On"));
        auto *invert = new QCheckBox(tr("Invert"));
        invert->setToolTip(tr("Black out everything outside the shape instead of inside it"));
        auto *feather = new QDoubleSpinBox;
        feather->setRange(0, 500);
        feather->setDecimals(0);
        feather->setSuffix(tr(" px"));
        feather->setToolTip(tr("Soft edge width"));
        auto *editBtn = new QToolButton;
        editBtn->setText(tr("Edit"));
        editBtn->setCheckable(true);
        editBtn->setToolTip(tr("Edit this mask's points on the output map"));
        auto *remove = new QToolButton;
        remove->setText(QStringLiteral("✕"));
        remove->setAutoRaise(true);
        h->addWidget(enabled);
        h->addWidget(name, 1);
        h->addWidget(invert);
        h->addWidget(feather);
        h->addWidget(editBtn);
        h->addWidget(remove);

        connect(name, &QLineEdit::textEdited, this, [this, i](const QString &t) { edit(i, [&](ProjectionMask &m) { m.name = t; }); });
        connect(enabled, &QCheckBox::toggled, this, [this, i](bool on) { edit(i, [&](ProjectionMask &m) { m.enabled = on; }); });
        connect(invert, &QCheckBox::toggled, this, [this, i](bool on) { edit(i, [&](ProjectionMask &m) { m.invert = on; }); });
        connect(feather, &QDoubleSpinBox::valueChanged, this, [this, i](double v) { edit(i, [&](ProjectionMask &m) { m.feather = v; }); });
        connect(editBtn, &QToolButton::toggled, this, [this, i](bool on) {
            if (!m_refreshing)
                m_doc->setActiveMask(on ? m_screen : -1, on ? i : -1);
        });
        connect(remove, &QToolButton::clicked, this, [this, i] {
            Project &p = m_doc->project();
            if (m_screen < 0 || m_screen >= p.screens.size() || i >= p.screens[m_screen].masks.size())
                return;
            m_doc->setActiveMask(-1, -1);
            p.screens[m_screen].masks.removeAt(i);
            m_doc->notifyChanged(true);
        });
        auto update = [=] {
            const Project &pr = m_doc->project();
            if (m_screen < 0 || m_screen >= pr.screens.size() || i >= pr.screens[m_screen].masks.size())
                return;
            const ProjectionMask &m = pr.screens[m_screen].masks[i];
            if (name->text() != m.name)
                name->setText(m.name);
            const QSignalBlocker b1(enabled), b2(invert), b3(feather), b4(editBtn);
            enabled->setChecked(m.enabled);
            invert->setChecked(m.invert);
            feather->setValue(m.feather);
            editBtn->setChecked(m_doc->activeMaskScreen() == m_screen && m_doc->activeMask() == i);
        };
        update();
        m_updaters.append(update);
        m_list->addWidget(row);
    }
    m_refreshing = false;
}
