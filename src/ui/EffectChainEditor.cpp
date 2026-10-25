#include "ui/EffectChainEditor.h"

#include "model/Document.h"
#include "render/Effects.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace {

constexpr int kSliderSteps = 1000;

int decimalsFor(const EffectParamDef &d)
{
    const double span = d.max - d.min;
    return span > 50 ? 0 : (span > 5 ? 1 : 2);
}

// Index of a Red, Green, Blue parameter run, shown as one colour button; -1 if none.
int colourRunAt(const EffectDef &def, int i)
{
    if (i + 2 < def.params.size() && def.params[i].name == QLatin1String("Red")
        && def.params[i + 1].name == QLatin1String("Green") && def.params[i + 2].name == QLatin1String("Blue"))
        return i;
    return -1;
}

void setButtonColour(QPushButton *b, const QColor &c)
{
    b->setText(c.name().toUpper());
    b->setStyleSheet(QStringLiteral("QPushButton { background: %1; color: %2; }")
                         .arg(c.name(), c.lightness() > 128 ? QStringLiteral("black") : QStringLiteral("white")));
}

} // namespace

EffectChainEditor::EffectChainEditor(Document *doc, QWidget *parent)
    : QWidget(parent)
    , m_doc(doc)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    m_list = new QVBoxLayout;
    m_list->setSpacing(6);
    v->addLayout(m_list);

    auto *add = new QPushButton(tr("Add effect"));
    auto *menu = new QMenu(add);
    QString category;
    QMenu *sub = nullptr;
    for (const EffectDef &d : effectDefs()) {
        if (d.category != category) {
            category = d.category;
            sub = menu->addMenu(category);
        }
        sub->addAction(d.name, this, [this, id = d.id] {
            edit([&](QVector<EffectInstance> &c) { c.append(makeEffect(id)); }, true);
        });
    }
    add->setMenu(menu);
    v->addWidget(add);
}

void EffectChainEditor::setLayer(int layer)
{
    m_layer = layer;
    refresh();
}

QVector<EffectInstance> *EffectChainEditor::chain()
{
    Project &p = m_doc->project();
    if (m_layer < 0)
        return &p.compEffects;
    return m_layer < p.layers.size() ? &p.layers[m_layer].effects : nullptr;
}

void EffectChainEditor::edit(const std::function<void(QVector<EffectInstance> &)> &fn, bool structural)
{
    if (m_refreshing)
        return;
    QVector<EffectInstance> *c = chain();
    if (!c)
        return;
    fn(*c);
    m_doc->notifyChanged(structural);
}

QString EffectChainEditor::oscAddress(int effect, int param) const
{
    const QString owner = m_layer < 0 ? QStringLiteral("/dh/comp") : QStringLiteral("/dh/layer/%1").arg(m_layer + 1);
    return QStringLiteral("%1/effect/%2/%3").arg(owner).arg(effect + 1).arg(param + 1);
}

void EffectChainEditor::refresh()
{
    QVector<EffectInstance> *c = chain();
    QStringList types;
    if (c)
        for (const EffectInstance &e : std::as_const(*c))
            types << e.type;
    if (types != m_builtFor || m_layer != m_builtLayer) {
        rebuild();
        return;
    }
    m_refreshing = true;
    for (int i = 0; i < c->size() && i < m_setters.size(); ++i) {
        m_enabledSetters[i]((*c)[i].enabled);
        for (int k = 0; k < m_setters[i].size(); ++k)
            if (m_setters[i][k])
                m_setters[i][k](effectParam((*c)[i], k));
    }
    m_refreshing = false;
}

void EffectChainEditor::rebuild()
{
    // Deferred: this can run from inside a signal of one of these widgets.
    while (QLayoutItem *item = m_list->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
    m_setters.clear();
    m_enabledSetters.clear();
    m_builtFor.clear();
    m_builtLayer = m_layer;
    QVector<EffectInstance> *c = chain();
    if (!c)
        return;

    m_refreshing = true;
    if (c->isEmpty()) {
        auto *none = new QLabel(tr("No effects."));
        none->setEnabled(false);
        m_list->addWidget(none);
    }
    for (int i = 0; i < c->size(); ++i) {
        const EffectInstance &inst = (*c)[i];
        m_builtFor << inst.type;
        m_setters.append(QVector<std::function<void(double)>>());
        const EffectDef *def = effectDef(inst.type);

        auto *frame = new QFrame;
        frame->setFrameShape(QFrame::StyledPanel);
        auto *fl = new QVBoxLayout(frame);
        fl->setContentsMargins(6, 4, 6, 6);
        auto *header = new QHBoxLayout;
        auto *enabled = new QCheckBox(def ? def->name : tr("Unknown effect (%1)").arg(inst.type));
        QFont bold = enabled->font();
        bold.setBold(true);
        enabled->setFont(bold);
        enabled->setChecked(inst.enabled);
        enabled->setToolTip(tr("Turn this effect on or off"));
        connect(enabled, &QCheckBox::toggled, this, [this, i](bool on) {
            edit([&](QVector<EffectInstance> &ch) {
                if (i < ch.size())
                    ch[i].enabled = on;
            }, false);
        });
        m_enabledSetters.append([enabled](bool on) {
            if (enabled->isChecked() != on)
                enabled->setChecked(on);
        });
        header->addWidget(enabled, 1);
        auto tool = [&](const QString &text, const QString &tip, bool on, auto fn) {
            auto *b = new QToolButton;
            b->setText(text);
            b->setToolTip(tip);
            b->setEnabled(on);
            b->setAutoRaise(true);
            connect(b, &QToolButton::clicked, this, [this, fn] { edit(fn, true); });
            header->addWidget(b);
        };
        tool(QStringLiteral("▲"), tr("Move up"), i > 0, [i](QVector<EffectInstance> &ch) {
            if (i > 0 && i < ch.size())
                ch.swapItemsAt(i, i - 1);
        });
        tool(QStringLiteral("▼"), tr("Move down"), i + 1 < c->size(), [i](QVector<EffectInstance> &ch) {
            if (i + 1 < ch.size())
                ch.swapItemsAt(i, i + 1);
        });
        tool(QStringLiteral("✕"), tr("Remove"), true, [i](QVector<EffectInstance> &ch) {
            if (i < ch.size())
                ch.removeAt(i);
        });
        fl->addLayout(header);

        if (!def) {
            m_list->addWidget(frame);
            continue;
        }
        auto *form = new QFormLayout;
        form->setContentsMargins(0, 0, 0, 0);
        fl->addLayout(form);
        m_setters.last().resize(def->params.size());
        for (int k = 0; k < def->params.size(); ++k) {
            const EffectParamDef &pd = def->params[k];
            auto setParam = [this, i](int param, double v) {
                edit([&](QVector<EffectInstance> &ch) {
                    if (i >= ch.size())
                        return;
                    while (ch[i].params.size() <= param)
                        ch[i].params.append(effectParam(ch[i], int(ch[i].params.size())));
                    ch[i].params[param] = v;
                }, false);
            };

            if (colourRunAt(*def, k) == k) {
                auto *button = new QPushButton;
                auto colourOf = [i, k, this] {
                    const QVector<EffectInstance> *ch = chain();
                    if (!ch || i >= ch->size())
                        return QColor(Qt::white);
                    const EffectInstance &e = (*ch)[i];
                    return QColor::fromRgbF(float(effectParam(e, k)), float(effectParam(e, k + 1)), float(effectParam(e, k + 2)));
                };
                setButtonColour(button, colourOf());
                connect(button, &QPushButton::clicked, this, [this, button, colourOf, i, k, name = def->name] {
                    const QColor col = QColorDialog::getColor(colourOf(), this, tr("%1 colour").arg(name));
                    if (!col.isValid())
                        return;
                    edit([&](QVector<EffectInstance> &ch) {
                        if (i >= ch.size())
                            return;
                        while (ch[i].params.size() <= k + 2)
                            ch[i].params.append(effectParam(ch[i], int(ch[i].params.size())));
                        ch[i].params[k] = col.redF();
                        ch[i].params[k + 1] = col.greenF();
                        ch[i].params[k + 2] = col.blueF();
                    }, false);
                    setButtonColour(button, col);
                });
                auto update = [button, colourOf](double) { setButtonColour(button, colourOf()); };
                m_setters.last()[k] = update;
                m_setters.last()[k + 1] = update;
                m_setters.last()[k + 2] = update;
                button->setToolTip(tr("Red, green and blue are parameters %1-%3 for OSC/MIDI (%2)")
                                       .arg(k + 1).arg(oscAddress(i, k)).arg(k + 3));
                form->addRow(def->name == QLatin1String("Colour Key") ? tr("Key colour") : tr("Colour"), button);
                k += 2;
                continue;
            }

            QWidget *field = nullptr;
            if (!pd.choices.isEmpty()) {
                auto *combo = new QComboBox;
                combo->addItems(pd.choices);
                combo->setCurrentIndex(qBound(0, int(std::lround(effectParam(inst, k))), int(pd.choices.size()) - 1));
                connect(combo, &QComboBox::activated, this, [setParam, k](int index) { setParam(k, index); });
                m_setters.last()[k] = [combo](double v) {
                    const int index = int(std::lround(v));
                    if (combo->currentIndex() != index)
                        combo->setCurrentIndex(index);
                };
                field = combo;
            } else {
                auto *slider = new QSlider(Qt::Horizontal);
                slider->setRange(0, kSliderSteps);
                auto *spin = new QDoubleSpinBox;
                spin->setRange(pd.min, pd.max);
                spin->setDecimals(decimalsFor(pd));
                spin->setSingleStep(decimalsFor(pd) == 0 ? 1.0 : (decimalsFor(pd) == 1 ? 0.1 : 0.01));
                spin->setSuffix(pd.suffix);
                spin->setMinimumWidth(80);
                auto toSlider = [pd](double v) { return int(std::lround((v - pd.min) / (pd.max - pd.min) * kSliderSteps)); };
                auto show = [slider, spin, toSlider](double v) {
                    const QSignalBlocker b1(slider), b2(spin);
                    slider->setValue(toSlider(v));
                    spin->setValue(v);
                };
                show(effectParam(inst, k));
                connect(slider, &QSlider::valueChanged, this, [=](int s) {
                    const double v = pd.min + (pd.max - pd.min) * s / kSliderSteps;
                    const QSignalBlocker b(spin);
                    spin->setValue(v);
                    setParam(k, spin->value());
                });
                connect(spin, &QDoubleSpinBox::valueChanged, this, [=](double v) {
                    const QSignalBlocker b(slider);
                    slider->setValue(toSlider(v));
                    setParam(k, v);
                });
                connect(slider, &QSlider::sliderReleased, m_doc, &Document::commitUndo);
                m_setters.last()[k] = show;
                auto *w = new QWidget;
                auto *h = new QHBoxLayout(w);
                h->setContentsMargins(0, 0, 0, 0);
                h->addWidget(slider, 1);
                h->addWidget(spin);
                field = w;
            }
            field->setToolTip(tr("OSC: %1 (0..1 spans the range)\nRight-click to learn a MIDI control")
                                  .arg(oscAddress(i, k)));
            field->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(field, &QWidget::customContextMenuRequested, this, [this, field, i, k](const QPoint &pos) {
                QMenu m;
                m.addAction(tr("Learn MIDI control..."), this, [this, i, k] {
                    emit midiLearnRequested(int(ControlAction::EffectParam), m_layer, i, k);
                });
                m.exec(field->mapToGlobal(pos));
            });
            form->addRow(pd.name, field);
        }
        m_list->addWidget(frame);
    }
    m_refreshing = false;
}
