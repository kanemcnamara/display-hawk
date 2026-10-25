#include "ui/ControlDialog.h"

#include "control/ArtNet.h"
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
#include <QTabWidget>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

ControlDialog::ControlDialog(Document *doc, ControlRouter *router, QWidget *parent)
    : QDialog(parent)
    , m_doc(doc)
    , m_router(router)
{
    setWindowTitle(tr("OSC, MIDI & Art-Net"));
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

    // Art-Net / DMX
    auto *artnet = new QWidget;
    auto *al = new QVBoxLayout(artnet);
    auto *patchBox = new QGroupBox(tr("DMX patch (saved with the setup)"));
    auto *pf = new QFormLayout(patchBox);
    m_dmxEnabled = new QCheckBox(tr("Receive Art-Net (UDP 6454)"));
    m_dmxUniverse = new QSpinBox;
    m_dmxUniverse->setRange(0, 32767);
    m_dmxUniverseText = new QLabel;
    m_dmxAddress = new QSpinBox;
    m_dmxAddress->setRange(1, 512);
    m_dmxLayers = new QSpinBox;
    m_dmxLayers->setRange(1, 32);
    m_dmxStatus = new QLabel;
    m_dmxStatus->setWordWrap(true);
    auto *uniRow = new QHBoxLayout;
    uniRow->addWidget(m_dmxUniverse);
    uniRow->addWidget(m_dmxUniverseText, 1);
    pf->addRow(m_dmxEnabled);
    pf->addRow(tr("Universe (Port-Address)"), uniRow);
    pf->addRow(tr("Start address"), m_dmxAddress);
    pf->addRow(tr("Layers controlled"), m_dmxLayers);
    pf->addRow(m_dmxStatus);
    m_dmxChannels = new QTableWidget(0, 3);
    m_dmxChannels->setHorizontalHeaderLabels({tr("DMX channel"), tr("Function"), tr("Value")});
    m_dmxChannels->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_dmxChannels->verticalHeader()->hide();
    m_dmxChannels->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_dmxChannels->setSelectionMode(QAbstractItemView::NoSelection);
    auto *help = new QLabel(tr("Clip channels: 0 stops the layer, 1-255 plays that column. Column channel: 1-255 "
                               "triggers that column. Bypass and blackout switch on at 128. Values act when they "
                               "change, so the desk and this app can both be used; a desk's first packet sets "
                               "faders only."));
    help->setWordWrap(true);
    al->addWidget(patchBox);
    al->addWidget(m_dmxChannels, 1);
    al->addWidget(help);

    auto *oscPage = new QWidget;
    auto *ol = new QVBoxLayout(oscPage);
    ol->addWidget(osc);
    ol->addStretch();
    auto *midiPage = new QWidget;
    auto *ml = new QVBoxLayout(midiPage);
    ml->addWidget(midi);
    ml->addWidget(maps, 1);
    m_tabs = new QTabWidget;
    m_tabs->addTab(oscPage, tr("OSC"));
    m_tabs->addTab(midiPage, tr("MIDI"));
    m_tabs->addTab(artnet, tr("Art-Net / DMX"));

    auto *split = new QSplitter(Qt::Vertical);
    split->addWidget(m_tabs);
    split->addWidget(mon);
    split->setSizes({520, 160});
    auto *v = new QVBoxLayout(this);
    v->addWidget(split, 1);

    auto patchEdit = [this] {
        if (m_refreshing)
            return;
        DmxPatch &p = m_doc->project().dmx;
        p.enabled = m_dmxEnabled->isChecked();
        p.universe = m_dmxUniverse->value();
        p.address = m_dmxAddress->value();
        p.layers = m_dmxLayers->value();
        m_doc->notifyChanged();
        refreshDmx();
    };
    connect(m_dmxEnabled, &QCheckBox::toggled, this, patchEdit);
    connect(m_dmxUniverse, &QSpinBox::valueChanged, this, patchEdit);
    connect(m_dmxAddress, &QSpinBox::valueChanged, this, patchEdit);
    connect(m_dmxLayers, &QSpinBox::valueChanged, this, patchEdit);
    // Live values while the dialog is open.
    auto *dmxTimer = new QTimer(this);
    dmxTimer->setInterval(200);
    connect(dmxTimer, &QTimer::timeout, this, [this] {
        if (isVisible() && m_tabs->currentIndex() == 2)
            refreshDmxValues();
    });
    dmxTimer->start();
    connect(m_doc, &Document::changed, this, [this] {
        if (isVisible())
            refreshDmx();
    });
    refreshDmx();

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

void ControlDialog::refreshDmx()
{
    const DmxPatch &p = m_doc->project().dmx;
    m_refreshing = true;
    m_dmxEnabled->setChecked(p.enabled);
    if (m_dmxUniverse->value() != p.universe)
        m_dmxUniverse->setValue(p.universe);
    if (m_dmxAddress->value() != p.address)
        m_dmxAddress->setValue(p.address);
    if (m_dmxLayers->value() != p.layers)
        m_dmxLayers->setValue(p.layers);
    m_refreshing = false;
    m_dmxUniverseText->setText(tr("net.subnet.universe %1").arg(ArtNet::universeText(p.universe)));

    const QStringList names = ControlRouter::dmxChannelNames(p);
    if (m_dmxChannels->rowCount() != names.size()) {
        m_dmxChannels->setRowCount(int(names.size()));
        for (int i = 0; i < names.size(); ++i)
            for (int c = 0; c < 3; ++c)
                m_dmxChannels->setItem(i, c, new QTableWidgetItem);
    }
    for (int i = 0; i < names.size(); ++i) {
        const int channel = p.address + i;
        m_dmxChannels->item(i, 0)->setText(channel <= 512 ? QString::number(channel) : tr("%1 (past 512!)").arg(channel));
        m_dmxChannels->item(i, 1)->setText(names[i]);
    }
    refreshDmxValues();
}

void ControlDialog::refreshDmxValues()
{
    const DmxPatch &p = m_doc->project().dmx;
    const QByteArray dmx = m_router->lastDmx();
    for (int i = 0; i < m_dmxChannels->rowCount(); ++i) {
        const int index = p.address - 1 + i;
        m_dmxChannels->item(i, 2)->setText(index < dmx.size() ? QString::number(quint8(dmx[index])) : QStringLiteral("-"));
    }
    m_dmxStatus->setText(m_router->artNetStatus());
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
