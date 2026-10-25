#include "ui/CueListDialog.h"

#include "control/CueList.h"
#include "control/Timecode.h"
#include "model/Document.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>

CueListDialog::CueListDialog(Document *doc, CueList *cues, QWidget *parent)
    : QDialog(parent)
    , m_doc(doc)
    , m_cues(cues)
{
    setWindowTitle(tr("Cue list"));
    resize(900, 480);
    auto *v = new QVBoxLayout(this);

    // Timecode.
    auto *tc = new QHBoxLayout;
    m_source = new QComboBox;
    m_source->addItems({tr("No timecode (GO only)"), tr("MIDI timecode (MTC)"), tr("LTC on an audio input")});
    m_ltcDevice = new QComboBox;
    m_ltcDevice->addItem(tr("Default input"), QByteArray());
    for (const QAudioDevice &d : QMediaDevices::audioInputs())
        m_ltcDevice->addItem(d.description(), d.id());
    m_ltcDevice->setCurrentIndex(qMax(0, m_ltcDevice->findData(QSettings().value(QStringLiteral("timecode/ltcDevice")).toByteArray())));
    m_fps = new QComboBox;
    for (double f : {24.0, 25.0, 29.97, 30.0})
        m_fps->addItem(QStringLiteral("%1 fps").arg(f), f);
    m_clock = new QLabel;
    QFont mono(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::Monospace);
    mono.setPixelSize(22);
    mono.setBold(true);
    m_clock->setFont(mono);
    m_status = new QLabel;
    tc->addWidget(new QLabel(tr("Timecode")));
    tc->addWidget(m_source);
    tc->addWidget(m_ltcDevice);
    tc->addWidget(m_fps);
    tc->addStretch();
    tc->addWidget(m_status);
    tc->addWidget(m_clock);
    v->addLayout(tc);

    m_table = new QTableWidget(0, 8);
    m_table->setHorizontalHeaderLabels({tr("Name"), tr("Timecode"), tr("Action"), tr("Layer"), tr("Column"), tr("Value"),
                                        QString(), QString()});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    v->addWidget(m_table, 1);

    auto *buttons = new QHBoxLayout;
    auto *add = new QPushButton(tr("Add cue"));
    auto *back = new QPushButton(tr("Back"));
    auto *go = new QPushButton(tr("GO"));
    go->setMinimumSize(140, 44);
    go->setStyleSheet(QStringLiteral("QPushButton { background: #2e7d32; color: white; font-weight: bold; font-size: 18px; }"));
    go->setToolTip(tr("Fire the next cue (OSC /dh/cue/go; map a key or MIDI control to Cue GO)"));
    buttons->addWidget(add);
    buttons->addStretch();
    buttons->addWidget(back);
    buttons->addWidget(go);
    v->addLayout(buttons);

    connect(add, &QPushButton::clicked, this, [this] {
        Cue c;
        c.name = tr("Cue %1").arg(m_doc->project().cues.size() + 1);
        c.column = qMin(int(m_doc->project().cues.size()), qMax(0, m_doc->project().columns - 1));
        m_doc->project().cues.append(c);
        m_doc->notifyChanged(true);
    });
    connect(go, &QPushButton::clicked, m_cues, &CueList::go);
    connect(back, &QPushButton::clicked, m_cues, &CueList::back);
    connect(m_source, &QComboBox::activated, this, [this](int i) {
        m_doc->project().timecodeSource = TimecodeSource(i);
        m_doc->notifyChanged();
    });
    connect(m_ltcDevice, &QComboBox::activated, this, [this](int i) {
        QSettings().setValue(QStringLiteral("timecode/ltcDevice"), m_ltcDevice->itemData(i).toByteArray());
        if (m_doc->project().timecodeSource == TimecodeSource::Ltc)
            m_cues->timecode()->setSource(TimecodeInput::Source::Ltc, m_ltcDevice->itemData(i).toByteArray());
    });
    connect(m_fps, &QComboBox::activated, this, [this](int i) {
        m_doc->project().timecodeFps = m_fps->itemData(i).toDouble();
        m_doc->notifyChanged(true);
    });
    connect(m_doc, &Document::structureChanged, this, &CueListDialog::rebuild);
    connect(m_cues, &CueList::currentChanged, this, &CueListDialog::rebuild);
    connect(m_cues->timecode(), &TimecodeInput::statusChanged, this, &CueListDialog::refreshTimecode);
    auto *timer = new QTimer(this);
    timer->setInterval(40);
    connect(timer, &QTimer::timeout, this, &CueListDialog::refreshTimecode);
    timer->start();
    rebuild();
}

void CueListDialog::refreshTimecode()
{
    const Project &p = m_doc->project();
    m_source->setCurrentIndex(int(p.timecodeSource));
    m_ltcDevice->setVisible(p.timecodeSource == TimecodeSource::Ltc);
    m_fps->setCurrentIndex(qMax(0, m_fps->findData(p.timecodeFps)));
    TimecodeInput *tc = m_cues->timecode();
    const double fps = tc->running() ? tc->fps() : p.timecodeFps;
    m_clock->setText(timecodeText(tc->seconds(), fps));
    m_clock->setStyleSheet(tc->running() ? QStringLiteral("color: #60e070;") : QStringLiteral("color: #888;"));
    m_status->setText(tc->status());
}

void CueListDialog::rebuild()
{
    const Project &p = m_doc->project();
    m_refreshing = true;
    m_table->setRowCount(int(p.cues.size()));
    for (int i = 0; i < p.cues.size(); ++i) {
        const Cue &c = p.cues[i];
        auto edit = [this, i](const std::function<void(Cue &)> &fn) {
            QVector<Cue> &list = m_doc->project().cues;
            if (!m_refreshing && i < list.size()) {
                fn(list[i]);
                m_doc->notifyChanged();
            }
        };
        auto *name = new QLineEdit(c.name);
        connect(name, &QLineEdit::textEdited, this, [edit](const QString &t) { edit([&](Cue &x) { x.name = t; }); });
        auto *time = new QLineEdit(c.time >= 0 ? timecodeText(c.time, p.timecodeFps) : QString());
        time->setPlaceholderText(tr("GO only"));
        time->setToolTip(tr("HH:MM:SS:FF; empty = fired by GO only"));
        connect(time, &QLineEdit::editingFinished, this, [this, edit, time] {
            bool ok = false;
            const double s = timecodeFromText(time->text(), m_doc->project().timecodeFps, &ok);
            edit([&](Cue &x) { x.time = ok ? s : -1.0; });
        });
        auto *action = new QComboBox;
        for (int a = 0; a < kCueActionCount; ++a)
            action->addItem(cueActionName(CueAction(a)));
        action->setCurrentIndex(int(c.action));
        connect(action, &QComboBox::activated, this, [this, edit](int a) {
            edit([a](Cue &x) { x.action = CueAction(a); });
            QTimer::singleShot(0, this, &CueListDialog::rebuild); // which fields apply changes
        });
        auto *layer = new QSpinBox;
        layer->setRange(1, 64);
        layer->setValue(c.layer + 1);
        connect(layer, &QSpinBox::valueChanged, this, [edit](int l) { edit([l](Cue &x) { x.layer = l - 1; }); });
        auto *column = new QSpinBox;
        column->setRange(1, 999);
        column->setValue(c.column + 1);
        connect(column, &QSpinBox::valueChanged, this, [edit](int col) { edit([col](Cue &x) { x.column = col - 1; }); });
        auto *value = new QDoubleSpinBox;
        value->setRange(0, 300);
        value->setDecimals(2);
        value->setValue(c.value);
        connect(value, &QDoubleSpinBox::valueChanged, this, [edit](double val) { edit([val](Cue &x) { x.value = val; }); });
        layer->setEnabled(c.action == CueAction::TriggerClip || c.action == CueAction::ClearLayer);
        column->setEnabled(c.action == CueAction::TriggerClip || c.action == CueAction::TriggerColumn);
        value->setEnabled(c.action == CueAction::Master || c.action == CueAction::Blackout || c.action == CueAction::Crossfader
                          || c.action == CueAction::Bpm);
        auto *fire = new QPushButton(tr("Fire"));
        connect(fire, &QPushButton::clicked, this, [this, i] { m_cues->fire(i); });
        auto *remove = new QPushButton(QStringLiteral("✕"));
        remove->setFixedWidth(28);
        connect(remove, &QPushButton::clicked, this, [this, i] {
            QVector<Cue> &list = m_doc->project().cues;
            if (i < list.size()) {
                list.removeAt(i);
                if (m_cues->current() >= list.size())
                    m_cues->setCurrent(int(list.size()) - 1);
                m_doc->notifyChanged(true);
            }
        });
        m_table->setCellWidget(i, 0, name);
        m_table->setCellWidget(i, 1, time);
        m_table->setCellWidget(i, 2, action);
        m_table->setCellWidget(i, 3, layer);
        m_table->setCellWidget(i, 4, column);
        m_table->setCellWidget(i, 5, value);
        m_table->setCellWidget(i, 6, fire);
        m_table->setCellWidget(i, 7, remove);
        // The last cue fired is marked; GO fires the one after it.
        auto *header = new QTableWidgetItem(i == m_cues->current() ? QStringLiteral("▶ %1").arg(i + 1) : QString::number(i + 1));
        if (i == m_cues->current())
            header->setForeground(QColor(96, 224, 112));
        m_table->setVerticalHeaderItem(i, header);
    }
    m_refreshing = false;
    refreshTimecode();
}
