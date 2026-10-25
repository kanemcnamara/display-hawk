#include "ui/AudioReactiveDialog.h"

#include "audio/AudioReactive.h"
#include "render/Effects.h"
#include "model/Document.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMediaDevices>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <functional>

// ---------------------------------------------------------------- meters

AudioMeters::AudioMeters(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(100);
}

void AudioMeters::setLevels(const AudioLevels &levels)
{
    m_levels = levels;
    m_beatGlow = levels.beat ? 1.0 : m_beatGlow * 0.8;
    update();
}

void AudioMeters::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(24, 24, 27));
    const struct {
        const char *name;
        double value;
        QColor color;
    } bars[] = {{"Level", m_levels.level, QColor(200, 200, 200)},
                {"Bass", m_levels.bass, QColor(230, 80, 60)},
                {"Mid", m_levels.mid, QColor(240, 190, 60)},
                {"Treble", m_levels.treble, QColor(80, 180, 240)}};
    const int labelW = 54, lampW = 70, rowH = (height() - 8) / 4;
    for (int i = 0; i < 4; ++i) {
        const QRect row(4, 4 + i * rowH, width() - lampW - 8, rowH - 4);
        p.setPen(QColor(200, 200, 200));
        p.drawText(QRect(row.x(), row.y(), labelW, row.height()), Qt::AlignVCenter, QString::fromLatin1(bars[i].name));
        const QRect track(row.x() + labelW, row.y() + 2, row.width() - labelW, row.height() - 4);
        p.fillRect(track, QColor(40, 40, 45));
        p.fillRect(QRect(track.x(), track.y(), int(track.width() * qBound(0.0, bars[i].value, 1.0)), track.height()),
                   bars[i].color);
    }
    const QRect lamp(width() - lampW, 8, lampW - 8, height() - 16);
    QColor c(255, 70, 70);
    c.setAlphaF(0.15 + 0.85 * m_beatGlow);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(c);
    p.setPen(Qt::NoPen);
    const int d = qMin(lamp.width(), lamp.height()) - 18;
    p.drawEllipse(QRect(lamp.center().x() - d / 2, lamp.y() + 4, d, d));
    p.setPen(QColor(220, 220, 220));
    p.drawText(QRect(lamp.x(), lamp.bottom() - 16, lamp.width(), 16), Qt::AlignCenter, tr("Beat"));
}

// ---------------------------------------------------------------- dialog

AudioReactiveDialog::AudioReactiveDialog(Document *doc, AudioReactive *reactive, QWidget *parent)
    : QDialog(parent)
    , m_doc(doc)
    , m_reactive(reactive)
{
    setWindowTitle(tr("Audio reactive"));
    resize(760, 620);

    auto *input = new QGroupBox(tr("Audio input"));
    auto *f = new QFormLayout(input);
    m_enabled = new QCheckBox(tr("Drive the show from audio (saved with the setup)"));
    m_device = new QComboBox;
    m_sensitivity = new QDoubleSpinBox;
    m_sensitivity->setRange(0.1, 10.0);
    m_sensitivity->setSingleStep(0.1);
    m_sensitivity->setSuffix(QStringLiteral(" x"));
    m_sensitivity->setToolTip(tr("Input gain before analysis. Levels also adapt automatically to the loudness."));
    m_status = new QLabel;
    m_meters = new AudioMeters;
    f->addRow(m_enabled);
    f->addRow(tr("Device"), m_device);
    f->addRow(tr("Sensitivity"), m_sensitivity);
    f->addRow(m_status);
    f->addRow(m_meters);

    auto *linksBox = new QGroupBox(tr("Links"));
    auto *ll = new QVBoxLayout(linksBox);
    m_links = new QTableWidget(0, 5);
    m_links->setHorizontalHeaderLabels({tr("Signal"), tr("Drives"), tr("Layer / parameter"), tr("Amount"), QString()});
    m_links->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_links->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
    m_links->horizontalHeader()->resizeSection(4, 90);
    m_links->verticalHeader()->hide();
    m_links->setSelectionMode(QAbstractItemView::NoSelection);
    auto *add = new QPushButton(tr("Add link"));
    ll->addWidget(m_links, 1);
    ll->addWidget(add, 0, Qt::AlignLeft);
    auto *help = new QLabel(tr("Opacity and master links pulse between (1 - amount) and the fader value; "
                               "the faders themselves do not move. Beat links step to the next column or clip "
                               "that has something in it."));
    help->setWordWrap(true);
    ll->addWidget(help);

    auto *v = new QVBoxLayout(this);
    v->addWidget(input);
    v->addWidget(linksBox, 1);

    connect(m_enabled, &QCheckBox::toggled, this, [this](bool on) {
        if (m_refreshing)
            return;
        m_doc->project().audioReactive.enabled = on;
        m_doc->notifyChanged();
    });
    connect(m_device, &QComboBox::activated, this, [this](int i) {
        m_reactive->setInputDevice(m_device->itemData(i).toByteArray());
    });
    connect(m_sensitivity, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (m_refreshing)
            return;
        m_doc->project().audioReactive.sensitivity = v;
        m_doc->notifyChanged();
    });
    connect(add, &QPushButton::clicked, this, [this] {
        m_doc->project().audioReactive.links.append(AudioLink());
        m_doc->notifyChanged(true);
    });
    connect(m_reactive->analyzer(), &AudioAnalyzer::levelsChanged, m_meters, &AudioMeters::setLevels);
    connect(m_reactive, &AudioReactive::statusChanged, this, &AudioReactiveDialog::refresh);
    connect(m_doc, &Document::structureChanged, this, &AudioReactiveDialog::refreshLinks);
    refresh();
    refreshLinks();
}

void AudioReactiveDialog::refresh()
{
    const AudioReactiveSettings &ar = m_doc->project().audioReactive;
    m_refreshing = true;
    m_enabled->setChecked(ar.enabled);
    m_sensitivity->setValue(ar.sensitivity);
    m_device->clear();
    const QString defaultName = QMediaDevices::defaultAudioInput().description();
    m_device->addItem(defaultName.isEmpty() ? tr("System default (no audio input found)")
                                            : tr("System default (%1)").arg(defaultName),
                      QByteArray());
    for (const QAudioDevice &d : QMediaDevices::audioInputs())
        m_device->addItem(d.description(), d.id());
    m_device->setCurrentIndex(qMax(0, m_device->findData(m_reactive->inputDeviceId())));
    m_refreshing = false;
    m_status->setText(m_reactive->status());
}

void AudioReactiveDialog::refreshLinks()
{
    const QVector<AudioLink> &links = m_doc->project().audioReactive.links;
    m_links->setRowCount(int(links.size()));
    for (int i = 0; i < links.size(); ++i) {
        const AudioLink &l = links[i];
        // Value edits do not rebuild the table (that would drop focus while typing); target changes do.
        auto edit = [this, i](const std::function<void(AudioLink &)> &fn, bool rebuild = false) {
            QVector<AudioLink> &list = m_doc->project().audioReactive.links;
            if (i < list.size()) {
                fn(list[i]);
                m_doc->notifyChanged(rebuild);
            }
        };
        auto *signal = new QComboBox;
        for (int s = 0; s < kAudioSignalCount; ++s)
            signal->addItem(audioSignalName(AudioSignal(s)));
        signal->setCurrentIndex(int(l.signal));
        connect(signal, &QComboBox::activated, this, [edit](int s) { edit([s](AudioLink &x) { x.signal = AudioSignal(s); }); });
        auto *target = new QComboBox;
        for (int t = 0; t < kAudioTargetCount; ++t)
            target->addItem(audioTargetName(AudioTarget(t)));
        target->setCurrentIndex(int(l.target));
        connect(target, &QComboBox::activated, this, [edit](int t) {
            edit([t](AudioLink &x) {
                x.target = AudioTarget(t);
                if (audioTargetIsBeat(x.target))
                    x.signal = AudioSignal::Beat;
            }, true);
        });
        QWidget *layer = nullptr;
        if (l.target == AudioTarget::EffectParam) {
            // Every parameter of every effect in the project, as "Layer 2 / Blur / Radius".
            auto *param = new QComboBox;
            const Project &p = m_doc->project();
            auto addChain = [&](int li, const QString &owner, const QVector<EffectInstance> &chain) {
                for (int e = 0; e < chain.size(); ++e) {
                    const EffectDef *def = effectDef(chain[e].type);
                    if (!def)
                        continue;
                    for (int k = 0; k < def->params.size(); ++k) {
                        if (!def->params[k].choices.isEmpty())
                            continue; // choices cannot be modulated smoothly
                        param->addItem(QStringLiteral("%1 / %2 / %3").arg(owner, def->name, def->params[k].name),
                                       QVariantList{li, e, k});
                        if (li == l.layer && e == l.effect && k == l.param)
                            param->setCurrentIndex(param->count() - 1);
                    }
                }
            };
            addChain(-1, tr("Composition"), p.compEffects);
            for (int li = 0; li < p.layers.size(); ++li)
                addChain(li, tr("Layer %1").arg(li + 1), p.layers[li].effects);
            if (param->count() == 0) {
                param->addItem(tr("(add an effect first)"));
                param->setEnabled(false);
            } else if (param->currentData().toList() != QVariantList{l.layer, l.effect, l.param}) {
                param->insertItem(0, tr("(choose a parameter)"));
                param->setCurrentIndex(0);
            }
            connect(param, &QComboBox::activated, this, [edit, param](int index) {
                const QVariantList v = param->itemData(index).toList();
                if (v.size() == 3)
                    edit([v](AudioLink &x) {
                        x.layer = v[0].toInt();
                        x.effect = v[1].toInt();
                        x.param = v[2].toInt();
                    });
            });
            layer = param;
        } else {
            auto *spin = new QSpinBox;
            spin->setRange(1, 64);
            spin->setValue(qMax(0, l.layer) + 1);
            spin->setEnabled(audioTargetUsesLayer(l.target));
            connect(spin, &QSpinBox::valueChanged, this, [edit](int v) { edit([v](AudioLink &x) { x.layer = v - 1; }); });
            layer = spin;
        }
        auto *amount = new QDoubleSpinBox;
        amount->setRange(0, 100);
        amount->setDecimals(0);
        amount->setSuffix(QStringLiteral(" %"));
        amount->setValue(l.amount * 100.0);
        amount->setEnabled(!audioTargetIsBeat(l.target));
        connect(amount, &QDoubleSpinBox::valueChanged, this, [edit](double v) { edit([v](AudioLink &x) { x.amount = v / 100.0; }); });
        auto *remove = new QPushButton(tr("Remove"));
        connect(remove, &QPushButton::clicked, this, [this, i] {
            QVector<AudioLink> &list = m_doc->project().audioReactive.links;
            if (i < list.size()) {
                list.removeAt(i);
                m_doc->notifyChanged(true);
            }
        });
        m_links->setCellWidget(i, 0, signal);
        m_links->setCellWidget(i, 1, target);
        m_links->setCellWidget(i, 2, layer);
        m_links->setCellWidget(i, 3, amount);
        m_links->setCellWidget(i, 4, remove);
    }
}
