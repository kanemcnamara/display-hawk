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
    m_draw = new QPushButton(tr("Draw"));
    m_draw->setCheckable(true);
    m_draw->setToolTip(tr("Click points on the map; click the first point, double-click or press Enter to close "
                          "the shape. Backspace removes the last point, Esc cancels."));
    auto *rect = new QPushButton(tr("Rectangle"));
    auto *ellipse = new QPushButton(tr("Ellipse"));
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(new QLabel(tr("Add mask:")));
    buttons->addWidget(m_draw);
    buttons->addWidget(rect);
    buttons->addWidget(ellipse);
    buttons->addStretch();
    v->addLayout(buttons);
    auto *help = new QLabel(tr("Edit a mask to drag its points on the map; double-click an edge to add a point, "
                               "Alt+click a point to remove it, drag inside to move the whole mask. Smooth turns the "
                               "points into a curve through them."));
    help->setWordWrap(true);
    help->setEnabled(false);
    v->addWidget(help);

    auto addShape = [this](bool round) {
        const QSizeF size = canvasSize();
        if (size.isEmpty())
            return;
        const QRectF r(size.width() / 4.0, size.height() / 4.0, size.width() / 2.0, size.height() / 2.0);
        ProjectionMask m = round ? ProjectionMask::ellipse(r) : ProjectionMask::rectangle(r);
        if (m_target == kCompositionMasks)
            m.feather = 0.0; // LED shapes want hard edges; projection masks a soft one
        m_doc->addMask(m_target, m);
    };
    connect(rect, &QPushButton::clicked, this, [addShape] { addShape(false); });
    connect(ellipse, &QPushButton::clicked, this, [addShape] { addShape(true); });
    connect(m_draw, &QPushButton::toggled, this, [this](bool on) {
        if (m_refreshing)
            return;
        m_doc->setActiveMask(-1, -1);
        m_doc->setMaskDrawTarget(on ? m_target : -1);
    });
    connect(m_doc, &Document::selectionChanged, this, &MaskEditor::refresh);
}

QSizeF MaskEditor::canvasSize() const
{
    const Project &p = m_doc->project();
    if (m_target == kCompositionMasks)
        return QSizeF(p.compSize());
    if (m_target >= 0 && m_target < p.screens.size())
        return QSizeF(p.screens[m_target].width, p.screens[m_target].height);
    return {};
}

void MaskEditor::setTarget(int target)
{
    if (target != m_target) {
        // Stop editing or drawing another target's mask.
        if (m_doc->activeMaskScreen() == m_target && m_target != -1)
            m_doc->setActiveMask(-1, -1);
        if (m_doc->maskDrawTarget() == m_target && m_target != -1)
            m_doc->setMaskDrawTarget(-1);
    }
    m_target = target;
    refresh();
}

void MaskEditor::edit(int index, const std::function<void(ProjectionMask &)> &fn)
{
    QVector<ProjectionMask> *masks = m_doc->maskList(m_target);
    if (m_refreshing || !masks || index < 0 || index >= masks->size())
        return;
    fn((*masks)[index]);
    m_doc->notifyChanged();
}

void MaskEditor::refresh()
{
    const QVector<ProjectionMask> *masks = m_doc->maskList(m_target);
    const int count = masks ? int(masks->size()) : 0;
    {
        m_refreshing = true;
        m_draw->setChecked(m_target != -1 && m_doc->maskDrawTarget() == m_target);
        m_refreshing = false;
    }
    if (count != m_builtCount || m_target != m_builtTarget) {
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
    const QVector<ProjectionMask> *masks = m_doc->maskList(m_target);
    m_builtTarget = m_target;
    m_builtCount = masks ? int(masks->size()) : 0;
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
        auto *smooth = new QCheckBox(tr("Smooth"));
        smooth->setToolTip(tr("A curve through the points instead of straight edges"));
        auto *feather = new QDoubleSpinBox;
        feather->setRange(0, 500);
        feather->setDecimals(0);
        feather->setSuffix(tr(" px"));
        feather->setToolTip(tr("Soft edge width"));
        auto *editBtn = new QToolButton;
        editBtn->setText(tr("Edit"));
        editBtn->setCheckable(true);
        editBtn->setToolTip(tr("Edit this mask's points on the map"));
        auto *remove = new QToolButton;
        remove->setText(QStringLiteral("✕"));
        remove->setAutoRaise(true);
        h->addWidget(enabled);
        h->addWidget(name, 1);
        h->addWidget(invert);
        h->addWidget(smooth);
        h->addWidget(feather);
        h->addWidget(editBtn);
        h->addWidget(remove);

        connect(name, &QLineEdit::textEdited, this, [this, i](const QString &t) { edit(i, [&](ProjectionMask &m) { m.name = t; }); });
        connect(enabled, &QCheckBox::toggled, this, [this, i](bool on) { edit(i, [&](ProjectionMask &m) { m.enabled = on; }); });
        connect(invert, &QCheckBox::toggled, this, [this, i](bool on) { edit(i, [&](ProjectionMask &m) { m.invert = on; }); });
        connect(smooth, &QCheckBox::toggled, this, [this, i](bool on) { edit(i, [&](ProjectionMask &m) { m.smooth = on; }); });
        connect(feather, &QDoubleSpinBox::valueChanged, this, [this, i](double v) { edit(i, [&](ProjectionMask &m) { m.feather = v; }); });
        connect(editBtn, &QToolButton::toggled, this, [this, i](bool on) {
            if (m_refreshing)
                return;
            m_doc->setMaskDrawTarget(-1);
            m_doc->setActiveMask(on ? m_target : -1, on ? i : -1);
        });
        connect(remove, &QToolButton::clicked, this, [this, i] {
            QVector<ProjectionMask> *list = m_doc->maskList(m_target);
            if (!list || i >= list->size())
                return;
            m_doc->setActiveMask(-1, -1);
            list->removeAt(i);
            m_doc->notifyChanged(true);
        });
        auto update = [=] {
            const QVector<ProjectionMask> *list = m_doc->maskList(m_target);
            if (!list || i >= list->size())
                return;
            const ProjectionMask &m = (*list)[i];
            if (name->text() != m.name)
                name->setText(m.name);
            const QSignalBlocker b1(enabled), b2(invert), b3(feather), b4(editBtn), b5(smooth);
            enabled->setChecked(m.enabled);
            invert->setChecked(m.invert);
            smooth->setChecked(m.smooth);
            feather->setValue(m.feather);
            editBtn->setChecked(m_doc->activeMaskScreen() == m_target && m_doc->activeMask() == i);
        };
        update();
        m_updaters.append(update);
        m_list->addWidget(row);
    }
    m_refreshing = false;
}
