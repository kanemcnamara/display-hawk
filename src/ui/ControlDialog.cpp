#include "ui/ControlDialog.h"

#include "control/ArtNet.h"
#include "control/ControlRouter.h"
#include "control/ControlFeedback.h"
#include "control/WebRemote.h"
#include "control/Sacn.h"
#include "model/Document.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkInterface>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

ControlDialog::ControlDialog(Document *doc, ControlRouter *router, ControlFeedback *feedback, WebRemote *web,
                             QWidget *parent)
    : QDialog(parent)
    , m_doc(doc)
    , m_router(router)
    , m_feedback(feedback)
{
    setWindowTitle(tr("OSC, MIDI & DMX"));
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

    // OSC feedback: state back to the surfaces (TouchOSC, Open Stage Control, show controllers).
    auto *oscFb = new QGroupBox(tr("OSC feedback"));
    auto *ofl = new QVBoxLayout(oscFb);
    auto *oscFbOn = new QCheckBox(tr("Send the show's state to port"));
    auto *oscFbPort = new QSpinBox;
    oscFbPort->setRange(1, 65535);
    auto *oscFbHost = new QLineEdit;
    oscFbHost->setPlaceholderText(tr("every device that sends us OSC"));
    auto *oscFbRow = new QHBoxLayout;
    oscFbRow->addWidget(oscFbOn);
    oscFbRow->addWidget(oscFbPort);
    oscFbRow->addWidget(new QLabel(tr("of")));
    oscFbRow->addWidget(oscFbHost, 1);
    ofl->addLayout(oscFbRow);
    auto *oscFbHelp = new QLabel(tr("Sent on the same addresses as above when they change (master, blackout, layer "
                                    "opacity/bypass/solo, clip and column triggers as 1 = playing), plus "
                                    "/dh/layer/<l>/name, /dh/layer/<l>/clip/<c>/name and /dh/tempo/beat on each beat."));
    oscFbHelp->setWordWrap(true);
    ofl->addWidget(oscFbHelp);
    oscFbOn->setChecked(m_feedback->oscEnabled());
    oscFbPort->setValue(m_feedback->oscPort());
    oscFbHost->setText(m_feedback->oscHost());
    auto applyOscFb = [this, oscFbOn, oscFbPort, oscFbHost] {
        m_feedback->setOsc(oscFbOn->isChecked(), oscFbPort->value(), oscFbHost->text());
    };
    connect(oscFbOn, &QCheckBox::toggled, this, applyOscFb);
    connect(oscFbPort, &QSpinBox::editingFinished, this, applyOscFb);
    connect(oscFbHost, &QLineEdit::editingFinished, this, applyOscFb);

    // MIDI devices
    auto *midi = new QGroupBox(tr("MIDI inputs"));
    auto *mf = new QVBoxLayout(midi);
    m_devices = new QListWidget;
    m_devices->setMaximumHeight(70);
    auto *refreshButton = new QPushButton(tr("Refresh"));
    mf->addWidget(m_devices);
    mf->addWidget(refreshButton, 0, Qt::AlignLeft);

    // MIDI feedback: LEDs and motor faders on the controller follow the show.
    auto *outputs = new QGroupBox(tr("MIDI outputs (feedback)"));
    auto *outl = new QVBoxLayout(outputs);
    auto *fb = new QGroupBox(tr("MIDI feedback (button lights, motor faders)"));
    auto *fbl = new QVBoxLayout(fb);
    m_outputs = new QListWidget;
    m_outputs->setMaximumHeight(70);
    outl->addWidget(m_outputs);
    outl->addStretch();
    m_fbEnabled = new QCheckBox(tr("Send to the controller. Note velocity / CC value for"));
    auto spin = [] {
        auto *s = new QSpinBox;
        s->setRange(0, 127);
        return s;
    };
    m_fbOff = spin();
    m_fbLoaded = spin();
    m_fbActive = spin();
    auto *resend = new QPushButton(tr("Resend"));
    resend->setToolTip(tr("Send every light and fader position again (after reconnecting a controller)"));
    auto *fbRow = new QHBoxLayout;
    fbRow->addWidget(m_fbEnabled);
    fbRow->addWidget(new QLabel(tr("off")));
    fbRow->addWidget(m_fbOff);
    fbRow->addWidget(new QLabel(tr("loaded")));
    fbRow->addWidget(m_fbLoaded);
    fbRow->addWidget(new QLabel(tr("playing / on")));
    fbRow->addWidget(m_fbActive);
    fbRow->addStretch();
    fbRow->addWidget(resend);
    fbl->addLayout(fbRow);
    auto *fbHelp = new QLabel(tr("Pad controllers choose colours by velocity (see the controller's manual). Faders and "
                                 "knobs get their position back. Outputs named like the chosen inputs are used until "
                                 "you tick outputs here."));
    fbHelp->setWordWrap(true);
    fbl->addWidget(fbHelp);
    auto fbEdit = [this] {
        if (m_refreshing)
            return;
        MidiFeedback &f = m_doc->project().midiFeedback;
        f.enabled = m_fbEnabled->isChecked();
        f.offValue = m_fbOff->value();
        f.loadedValue = m_fbLoaded->value();
        f.activeValue = m_fbActive->value();
        m_doc->notifyChanged();
        m_feedback->resendAll();
    };
    connect(m_fbEnabled, &QCheckBox::toggled, this, fbEdit);
    for (QSpinBox *s : {m_fbOff, m_fbLoaded, m_fbActive})
        connect(s, &QSpinBox::valueChanged, this, fbEdit);
    connect(resend, &QPushButton::clicked, m_feedback, &ControlFeedback::resendAll);
    connect(m_outputs, &QListWidget::itemChanged, this, [this] {
        if (m_refreshing)
            return;
        QStringList on;
        for (int i = 0; i < m_outputs->count(); ++i)
            if (m_outputs->item(i)->checkState() == Qt::Checked)
                on << m_outputs->item(i)->text();
        m_feedback->setMidiOutputs(on);
    });

    // Mappings
    auto *maps = new QGroupBox(tr("MIDI mappings (saved with the setup)"));
    auto *mapl = new QVBoxLayout(maps);
    m_mappings = new QTableWidget(0, 5);
    m_mappings->setHorizontalHeaderLabels({tr("Action"), tr("Target"), tr("MIDI control / key"), QString(), QString()});
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
    m_newParam = new QSpinBox;
    m_newParam->setPrefix(tr("Parameter "));
    m_newParam->setRange(1, 8);
    auto *add = new QPushButton(tr("Add and learn"));
    auto *addRow = new QHBoxLayout;
    addRow->addWidget(m_newAction, 1);
    addRow->addWidget(m_newLayer);
    addRow->addWidget(m_newColumn);
    addRow->addWidget(m_newParam);
    addRow->addWidget(add);
    mapl->addWidget(m_mappings, 1);
    mapl->addLayout(addRow);
    mapl->addWidget(new QLabel(tr("Learn: press the button, then press a key or move a knob/fader on your controller, "
                                  "or press a key on the computer keyboard (not while typing in a field). "
                                  "Tip: right-click a clip or column number in the Clip Deck to learn it directly.")));

    // Monitor
    auto *mon = new QGroupBox(tr("Incoming messages"));
    auto *monl = new QVBoxLayout(mon);
    m_monitor = new QPlainTextEdit;
    m_monitor->setReadOnly(true);
    m_monitor->setMaximumBlockCount(300);
    m_monitor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    monl->addWidget(m_monitor);

    // Art-Net / DMX
    auto *artnet = new QWidget;
    auto *al = new QVBoxLayout(artnet);
    auto *patchBox = new QGroupBox(tr("DMX patch (saved with the setup)"));
    auto *pf = new QFormLayout(patchBox);
    m_dmxEnabled = new QCheckBox(tr("Receive DMX from a lighting desk"));
    m_dmxProtocol = new QComboBox;
    m_dmxProtocol->addItem(tr("Art-Net (UDP 6454)"), int(DmxProtocol::ArtNet));
    m_dmxProtocol->addItem(tr("sACN / E1.31 (UDP 5568, multicast)"), int(DmxProtocol::Sacn));
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
    pf->addRow(tr("Protocol"), m_dmxProtocol);
    pf->addRow(tr("Universe"), uniRow);
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
    ol->addWidget(oscFb);

    // Web remote: HTTP on the network, for phones, tablets and show-control systems.
    auto *webBox = new QGroupBox(tr("Web remote (HTTP)"));
    auto *wl = new QVBoxLayout(webBox);
    auto *webEnabled = new QCheckBox(tr("Serve a web remote and REST API on port"));
    auto *webPort = new QSpinBox;
    webPort->setRange(1024, 65535);
    QSettings settings;
    webPort->setValue(settings.value(QStringLiteral("web/port"), 8080).toInt());
    webEnabled->setChecked(web->isEnabled());
    auto *webRow = new QHBoxLayout;
    webRow->addWidget(webEnabled);
    webRow->addWidget(webPort);
    webRow->addStretch();
    auto *webInfo = new QLabel;
    webInfo->setWordWrap(true);
    webInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    wl->addLayout(webRow);
    wl->addWidget(webInfo);
    auto showWeb = [web, webInfo] {
        webInfo->setText(web->isEnabled()
                             ? tr("Open on a phone or tablet on the same network: %1\nREST: POST /api/<OSC path without /dh>, "
                                  "e.g. /api/column/2/trigger or /api/master?value=0.5; GET /api/state.")
                                   .arg(web->urls().join(QStringLiteral("  ")))
                             : tr("Off. When on, anyone on the network can control the show: use it on show networks only."));
    };
    auto applyWeb = [web, webEnabled, webPort, webInfo, showWeb] {
        QString error;
        const bool ok = web->setEnabled(webEnabled->isChecked(), webPort->value(), false, &error);
        QSettings s;
        s.setValue(QStringLiteral("web/enabled"), webEnabled->isChecked() && ok);
        s.setValue(QStringLiteral("web/port"), webPort->value());
        if (!ok) {
            const QSignalBlocker b(webEnabled);
            webEnabled->setChecked(false);
            webInfo->setText(tr("Cannot listen: %1").arg(error));
            return;
        }
        showWeb();
    };
    connect(webEnabled, &QCheckBox::toggled, this, applyWeb);
    connect(webPort, &QSpinBox::editingFinished, this, [webEnabled, applyWeb] {
        if (webEnabled->isChecked())
            applyWeb();
    });
    showWeb();
    ol->addWidget(webBox);
    ol->addStretch();
    auto *midiPage = new QWidget;
    auto *ml = new QVBoxLayout(midiPage);
    auto *devicesRow = new QHBoxLayout;
    devicesRow->addWidget(midi);
    devicesRow->addWidget(outputs);
    ml->addLayout(devicesRow);
    ml->addWidget(fb);
    ml->addWidget(maps, 1);
    m_tabs = new QTabWidget;
    m_tabs->addTab(oscPage, tr("OSC"));
    m_tabs->addTab(midiPage, tr("MIDI"));
    m_tabs->addTab(artnet, tr("DMX (Art-Net / sACN)"));

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
        const DmxProtocol protocol = DmxProtocol(m_dmxProtocol->currentData().toInt());
        if (protocol != p.protocol) {
            p.protocol = protocol;
            // sACN universes start at 1, Art-Net Port-Addresses at 0.
            p.universe = protocol == DmxProtocol::Sacn ? qMax(1, m_dmxUniverse->value()) : m_dmxUniverse->value();
        } else {
            p.universe = m_dmxUniverse->value();
        }
        p.address = m_dmxAddress->value();
        p.layers = m_dmxLayers->value();
        m_doc->notifyChanged();
        refreshDmx();
    };
    connect(m_dmxEnabled, &QCheckBox::toggled, this, patchEdit);
    connect(m_dmxProtocol, &QComboBox::activated, this, patchEdit);
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
        if (isVisible()) {
            refreshDmx();
            refreshFeedback();
        }
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
        refreshOutputs(); // default outputs follow the inputs
    });
    connect(add, &QPushButton::clicked, this, [this] {
        addMapping(m_newAction->currentData().toInt(), m_newLayer->value() - 1, m_newColumn->value() - 1,
                   m_newParam->value() - 1);
        m_router->startLearn(int(m_doc->project().controls.size()) - 1);
    });
    auto syncAddFields = [this] {
        const ControlAction a = ControlAction(m_newAction->currentData().toInt());
        // Effect parameters: layer 0 is the composition, the column field picks the effect.
        const bool effect = a == ControlAction::EffectParam;
        m_newLayer->setMinimum(effect ? 0 : 1);
        m_newLayer->setSpecialValueText(effect ? tr("Composition") : QString());
        m_newLayer->setEnabled(controlActionUsesLayer(a));
        m_newColumn->setPrefix(effect ? tr("Effect ") : tr("Column "));
        m_newColumn->setEnabled(controlActionUsesColumn(a) || effect);
        m_newParam->setVisible(effect);
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
    refreshFeedback();
}

void ControlDialog::refreshFeedback()
{
    const MidiFeedback &f = m_doc->project().midiFeedback;
    m_refreshing = true;
    m_fbEnabled->setChecked(f.enabled);
    m_fbOff->setValue(f.offValue);
    m_fbLoaded->setValue(f.loadedValue);
    m_fbActive->setValue(f.activeValue);
    m_refreshing = false;
}

void ControlDialog::refreshOutputs()
{
    m_refreshing = true;
    m_outputs->clear();
    const QStringList open = m_feedback->midiOutput()->openDevices();
    const QStringList all = m_feedback->midiOutput()->availableDevices();
    for (const QString &name : all) {
        auto *item = new QListWidgetItem(name, m_outputs);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(open.contains(name) ? Qt::Checked : Qt::Unchecked);
    }
    if (all.isEmpty()) {
        auto *item = new QListWidgetItem(tr("No MIDI outputs found"), m_outputs);
        item->setFlags(Qt::NoItemFlags);
    }
    m_refreshing = false;
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
    m_dmxProtocol->setCurrentIndex(m_dmxProtocol->findData(int(p.protocol)));
    const bool sacn = p.protocol == DmxProtocol::Sacn;
    m_dmxUniverse->setRange(sacn ? 1 : 0, sacn ? 63999 : 32767);
    if (m_dmxUniverse->value() != p.universe)
        m_dmxUniverse->setValue(p.universe);
    if (m_dmxAddress->value() != p.address)
        m_dmxAddress->setValue(p.address);
    if (m_dmxLayers->value() != p.layers)
        m_dmxLayers->setValue(p.layers);
    m_refreshing = false;
    m_dmxUniverseText->setText(sacn ? tr("multicast group %1").arg(Sacn::multicastGroup(p.universe).toString())
                                    : tr("net.subnet.universe %1").arg(ArtNet::universeText(p.universe)));

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
    refreshOutputs();
}

void ControlDialog::addMapping(int action, int layer, int column, int param)
{
    ControlMapping m;
    m.action = ControlAction(action);
    m.layer = m.action == ControlAction::EffectParam ? qMax(-1, layer) : qMax(0, layer);
    m.column = qMax(0, column);
    m.param = qMax(0, param);
    m_doc->project().controls.append(m);
    m_doc->notifyChanged(true);
}

void ControlDialog::learnNew(int action, int layer, int column, int param)
{
    addMapping(action, layer, column, param);
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
        auto *midiItem = new QTableWidgetItem(learning ? tr("Waiting for a MIDI key, knob or fader, or a computer key...")
                                                       : c.midiText());
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
