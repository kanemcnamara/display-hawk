#include "ui/LfoDialog.h"

#include "model/Document.h"
#include "render/Effects.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <functional>

namespace {

// Cycle lengths offered, in beats.
const double kPeriods[] = {0.25, 0.5, 1, 2, 4, 8, 16, 32, 64};

QString periodText(double beats)
{
    if (beats < 1.0)
        return QObject::tr("1/%1 beat").arg(qRound(1.0 / beats));
    if (beats >= 4.0 && qRound(beats) % 4 == 0)
        return beats == 4.0 ? QObject::tr("1 bar") : QObject::tr("%1 bars").arg(qRound(beats / 4.0));
    return beats == 1.0 ? QObject::tr("1 beat") : QObject::tr("%1 beats").arg(beats);
}

} // namespace

LfoDialog::LfoDialog(Document *doc, QWidget *parent)
    : QDialog(parent)
    , m_doc(doc)
{
    setWindowTitle(tr("LFOs"));
    resize(860, 360);
    auto *v = new QVBoxLayout(this);
    auto *help = new QLabel(tr("Waves locked to the tempo. On opacity and the master they dip the fader by up to "
                               "the amount; on an effect parameter they swing it around its set value by up to "
                               "half its range. Faders and undo are untouched; audio-reactive links combine with them."));
    help->setWordWrap(true);
    v->addWidget(help);
    m_table = new QTableWidget(0, 7);
    m_table->setHorizontalHeaderLabels({tr("On"), tr("Drives"), tr("Wave"), tr("Cycle"), tr("Amount"), tr("Phase"), QString()});
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    v->addWidget(m_table, 1);
    auto *add = new QPushButton(tr("Add LFO"));
    v->addWidget(add, 0, Qt::AlignLeft);
    connect(add, &QPushButton::clicked, this, [this] {
        m_doc->project().lfos.append(Lfo());
        m_doc->notifyChanged(true);
    });
    connect(m_doc, &Document::structureChanged, this, &LfoDialog::rebuild);
    rebuild();
}

void LfoDialog::rebuild()
{
    const Project &p = m_doc->project();
    m_table->setRowCount(int(p.lfos.size()));
    for (int i = 0; i < p.lfos.size(); ++i) {
        const Lfo &l = p.lfos[i];
        auto edit = [this, i](const std::function<void(Lfo &)> &fn, bool structural = false) {
            QVector<Lfo> &list = m_doc->project().lfos;
            if (i < list.size()) {
                fn(list[i]);
                m_doc->notifyChanged(structural);
            }
        };
        auto *on = new QCheckBox;
        on->setChecked(l.enabled);
        connect(on, &QCheckBox::toggled, this, [edit](bool b) { edit([b](Lfo &x) { x.enabled = b; }); });

        // Targets: the master, each layer's opacity, then every effect parameter that has a range.
        auto *target = new QComboBox;
        auto addTarget = [&](const QString &text, AudioTarget t, int layer, int effect, int param) {
            target->addItem(text, QVariantList{int(t), layer, effect, param});
            if (l.target == t && (t == AudioTarget::Master || l.layer == layer)
                && (t != AudioTarget::EffectParam || (l.effect == effect && l.param == param)))
                target->setCurrentIndex(target->count() - 1);
        };
        addTarget(tr("Master"), AudioTarget::Master, 0, 0, 0);
        for (int li = 0; li < p.layers.size(); ++li)
            addTarget(tr("Layer %1 opacity (%2)").arg(li + 1).arg(p.layers[li].name), AudioTarget::LayerOpacity, li, 0, 0);
        auto addChain = [&](int li, const QString &owner, const QVector<EffectInstance> &chain) {
            for (int e = 0; e < chain.size(); ++e) {
                const EffectDef *def = effectDef(chain[e].type);
                for (int k = 0; def && k < def->params.size(); ++k)
                    if (def->params[k].choices.isEmpty())
                        addTarget(QStringLiteral("%1 / %2 / %3").arg(owner, def->name, def->params[k].name),
                                  AudioTarget::EffectParam, li, e, k);
            }
        };
        addChain(-1, tr("Composition"), p.compEffects);
        for (int li = 0; li < p.layers.size(); ++li)
            addChain(li, tr("Layer %1").arg(li + 1), p.layers[li].effects);
        connect(target, &QComboBox::activated, this, [edit, target](int index) {
            const QVariantList v = target->itemData(index).toList();
            edit([&v](Lfo &x) {
                x.target = AudioTarget(v[0].toInt());
                x.layer = v[1].toInt();
                x.effect = v[2].toInt();
                x.param = v[3].toInt();
            });
        });

        auto *shape = new QComboBox;
        for (int s = 0; s < kLfoShapeCount; ++s)
            shape->addItem(lfoShapeName(LfoShape(s)));
        shape->setCurrentIndex(int(l.shape));
        connect(shape, &QComboBox::activated, this, [edit](int s) { edit([s](Lfo &x) { x.shape = LfoShape(s); }); });

        auto *cycle = new QComboBox;
        for (double b : kPeriods) {
            cycle->addItem(periodText(b), b);
            if (qFuzzyCompare(b, l.beats))
                cycle->setCurrentIndex(cycle->count() - 1);
        }
        connect(cycle, &QComboBox::activated, this, [edit, cycle](int k) {
            const double b = cycle->itemData(k).toDouble();
            edit([b](Lfo &x) { x.beats = b; });
        });

        auto percent = [](double value) {
            auto *s = new QDoubleSpinBox;
            s->setRange(0, 100);
            s->setDecimals(0);
            s->setSuffix(QStringLiteral(" %"));
            s->setValue(value * 100.0);
            return s;
        };
        auto *amount = percent(l.amount);
        connect(amount, &QDoubleSpinBox::valueChanged, this, [edit](double a) { edit([a](Lfo &x) { x.amount = a / 100.0; }); });
        auto *phase = percent(l.phase);
        phase->setToolTip(tr("Offset within the cycle: LFOs with different phases move in turn"));
        connect(phase, &QDoubleSpinBox::valueChanged, this, [edit](double ph) { edit([ph](Lfo &x) { x.phase = ph / 100.0; }); });

        auto *remove = new QPushButton(tr("Remove"));
        connect(remove, &QPushButton::clicked, this, [this, i] {
            QVector<Lfo> &list = m_doc->project().lfos;
            if (i < list.size()) {
                list.removeAt(i);
                m_doc->notifyChanged(true);
            }
        });
        m_table->setCellWidget(i, 0, on);
        m_table->setCellWidget(i, 1, target);
        m_table->setCellWidget(i, 2, shape);
        m_table->setCellWidget(i, 3, cycle);
        m_table->setCellWidget(i, 4, amount);
        m_table->setCellWidget(i, 5, phase);
        m_table->setCellWidget(i, 6, remove);
    }
}
