#include "ui/ControlDialog.h"

#include "control/ControlRouter.h"
#include "model/Document.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QNetworkInterface>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTime>
#include <QVBoxLayout>

ControlDialog::ControlDialog(Document *doc, ControlRouter *router, QWidget *parent)
    : QDialog(parent)
    , m_doc(doc)
    , m_router(router)
{
    setWindowTitle(tr("OSC & MIDI"));
    resize(900, 720);

    // OSC
    auto *osc = new QGroupBox(tr("OSC (UDP)"));
    auto *of = new QVBoxLayout(osc);
    m_oscEnabled = new QCheckBox(tr("Listen for OSC on port"));
    m_oscPort = new QSpinBox;
    m_oscPort->setRange(1, 65535);
    auto *oscRow = new QHBoxLayout;
    oscRow->addWidget(m_oscEnabled);
    oscRow->addWidget(m_oscPort);
    oscRow->addStretch();
    m_oscStatus = new QLabel;
    m_oscStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *reference = new QLabel(ControlRouter::oscReference());
    reference->setWordWrap(true);
    reference->setTextInteractionFlags(Qt::TextSelectableByMouse);
    of->addLayout(oscRow);
    of->addWidget(m_oscStatus);
    of->addWidget(reference);

    // MIDI devices
    auto *midi = new QGroupBox(tr("MIDI inputs"));
    auto *mf = new QVBoxLayout(midi);
    m_devices = new QListWidget;
    m_devices->setMaximumHeight(90);
    auto *refreshButton = new QPushButton(tr("Refresh"));
    mf->addWidget(m_devices);
    mf->addWidget(refreshButton, 0, Qt::AlignLeft);

    // Mappings
    auto *maps = new QGroupBox(tr("MIDI mappings (saved with the setup)"));
    auto *mapl = new QVBoxLayout(maps);
    m_mappings = new QTableWidget(0, 5);
    m_mappings->setHorizontalHeaderLabels({tr("Action"), tr("Target"), tr("MIDI control"), QString(), QString()});
    m_mappings->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    // Button columns: fixed, since the buttons are cell widgets that "resize to contents" ignores.
    for (int col : {3, 4}) {
        m_mappings->horizontalHeader()->setSectionResizeMode(col, QHeaderView::Fixed);
        m_mappings->horizontalHeader()->resizeSection(col, 90);
    }
    m_mappings->verticalHeader()->hide();
    m_mappings->setSelectionMode(QAbstractItemView::NoSelection);
    m_mappings->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_newAction = new QComboBox;
    for (int i = 0; i < kControlActionCount; ++i)
        m_newAction->addItem(controlActionName(ControlAction(i)), i);
    m_newLayer = new QSpinBox;
    m_newLayer->setPrefix(tr("Layer "));
    m_newLayer->setMinimum(1);
    m_newColumn = new QSpinBox;
    m_newColumn->setPrefix(tr("Column "));
    m_newColumn->setMinimum(1);
    auto *add = new QPushButton(tr("Add and learn"));
    auto *addRow = new QHBoxLayout;
    addRow->addWidget(m_newAction, 1);
    addRow->addWidget(m_newLayer);
    addRow->addWidget(m_newColumn);
    addRow->addWidget(add);
    mapl->addWidget(m_mappings, 1);
    mapl->addLayout(addRow);
    mapl->addWidget(new QLabel(tr("Learn: press the button, then press a key or move a knob/fader on your controller. "
                                  "Tip: right-click a clip or column number in the Clip Deck to learn it directly.")));

    // Monitor
    auto *mon = new QGroupBox(tr("Incoming messages"));
    auto *monl = new QVBoxLayout(mon);
    m_monitor = new QPlainTextEdit;
    m_monitor->setReadOnly(true);
    m_monitor->setMaximumBlockCount(300);
    QFont mono(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::Monospace);
    m_monitor->setFont(mono);
    monl->addWidget(m_monitor);

    auto *split = new QSplitter(Qt::Vertical);
    split->addWidget(maps);
    split->addWidget(mon);
    split->setSizes({300, 160});

    auto *top = new QHBoxLayout;
    top->addWidget(osc, 3);
    top->addWidget(midi, 2);
    auto *v = new QVBoxLayout(this);
    v->addLayout(top);
    v->addWidget(split, 1);

    connect(m_oscEnabled, &QCheckBox::toggled, this, [this](bool on) {
        if (m_refreshing)
            return;
        QString error;
        if (!m_router->setOsc(on, m_oscPort->value(), &error))
            m_oscStatus->setText(tr("<span style='color:#ff6060'>Cannot listen: %1</span>").arg(error.toHtmlEscaped()));
    });
    connect(m_oscPort, &QSpinBox::editingFinished, this, [this] {
        if (m_router->oscEnabled() && m_oscPort->value() != m_router->oscPort())
            m_router->setOsc(true, m_oscPort->value());
    });
    connect(m_router, &ControlRouter::oscStateChanged, this, &ControlDialog::refreshOsc);
    connect(refreshButton, &QPushButton::clicked, this, &ControlDialog::refreshDevices);
    connect(m_devices, &QListWidget::itemChanged, this, [this] {
        if (m_refreshing)
            return;
        QStringList on;
        for (int i = 0; i < m_devices->count(); ++i)
            if (m_devices->item(i)->checkState() == Qt::Checked)
                on << m_devices->item(i)->text();
        m_router->setMidiDevices(on);
    });
    connect(add, &QPushButton::clicked, this, [this] {
        addMapping(m_newAction->currentData().toInt(), m_newLayer->value() - 1, m_newColumn->value() - 1);
        m_router->startLearn(int(m_doc->project().controls.size()) - 1);
    });
    auto syncAddFields = [this] {
        const ControlAction a = ControlAction(m_newAction->currentData().toInt());
        m_newLayer->setEnabled(controlActionUsesLayer(a));
        m_newColumn->setEnabled(controlActionUsesColumn(a));
    };
    connect(m_newAction, &QComboBox::currentIndexChanged, this, syncAddFields);
    syncAddFields();
    connect(m_doc, &Document::structureChanged, this, &ControlDialog::refreshMappings);
    connect(m_router, &ControlRouter::learnStateChanged, this, &ControlDialog::refreshMappings);
    connect(m_router, &ControlRouter::activity, this, [this](const QString &line) {
        if (isVisible())
            m_monitor->appendPlainText(QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz  ")) + line);
    });

    refreshOsc();
    refreshDevices();
    refreshMappings();
}

void ControlDialog::refreshOsc()
{
    m_refreshing = true;
    m_oscEnabled->setChecked(m_router->oscEnabled());
    m_oscPort->setValue(m_router->oscPort());
    m_refreshing = false;
    if (!m_router->oscEnabled()) {
        m_oscStatus->setText(tr("OSC is off."));
        return;
    }
    QStringList addresses;
    for (const QHostAddress &a : QNetworkInterface::allAddresses())
        if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback())
            addresses << QStringLiteral("%1:%2").arg(a.toString()).arg(m_router->oscPort());
    m_oscStatus->setText(tr("Listening. Send to: %1").arg(addresses.isEmpty() ? QStringLiteral("127.0.0.1:%1").arg(m_router->oscPort())
                                                                                : addresses.join(QStringLiteral(", "))));
}

void ControlDialog::refreshDevices()
{
    m_refreshing = true;
    m_devices->clear();
    const QStringList open = m_router->midi()->openDevices();
    const QStringList all = m_router->midi()->availableDevices();
    for (const QString &name : all) {
        auto *item = new QListWidgetItem(name, m_devices);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(open.contains(name) ? Qt::Checked : Qt::Unchecked);
    }
    if (all.isEmpty()) {
        auto *item = new QListWidgetItem(tr("No MIDI devices found"), m_devices);
        item->setFlags(Qt::NoItemFlags);
    }
    m_refreshing = false;
}

void ControlDialog::addMapping(int action, int layer, int column)
{
    ControlMapping m;
    m.action = ControlAction(action);
    m.layer = qMax(0, layer);
    m.column = qMax(0, column);
    m_doc->project().controls.append(m);
    m_doc->notifyChanged(true);
}

void ControlDialog::learnNew(int action, int layer, int column)
{
    addMapping(action, layer, column);
    m_router->startLearn(int(m_doc->project().controls.size()) - 1);
    show();
    raise();
}

void ControlDialog::refreshMappings()
{
    const QVector<ControlMapping> &controls = m_doc->project().controls;
    m_mappings->setRowCount(int(controls.size()));
    for (int i = 0; i < controls.size(); ++i) {
        const ControlMapping &c = controls[i];
        m_mappings->setItem(i, 0, new QTableWidgetItem(controlActionName(c.action)));
        m_mappings->setItem(i, 1, new QTableWidgetItem(c.targetText()));
        const bool learning = m_router->learningIndex() == i;
        auto *midiItem = new QTableWidgetItem(learning ? tr("Waiting for a key, knob or fader...") : c.midiText());
        if (learning)
            midiItem->setForeground(QColor(255, 190, 60));
        m_mappings->setItem(i, 2, midiItem);

        auto *learn = new QPushButton(learning ? tr("Cancel") : tr("Learn"));
        connect(learn, &QPushButton::clicked, this, [this, i, learning] {
            if (learning)
                m_router->cancelLearn();
            else
                m_router->startLearn(i);
        });
        m_mappings->setCellWidget(i, 3, learn);
        auto *remove = new QPushButton(tr("Remove"));
        connect(remove, &QPushButton::clicked, this, [this, i] {
            QVector<ControlMapping> &list = m_doc->project().controls;
            if (i < list.size()) {
                m_router->cancelLearn();
                list.removeAt(i);
                m_doc->notifyChanged(true);
            }
        });
        m_mappings->setCellWidget(i, 4, remove);
    }
}
