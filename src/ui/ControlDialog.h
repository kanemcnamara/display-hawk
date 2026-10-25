#pragma once

#include <QDialog>

class ControlRouter;
class Document;
class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QSpinBox;
class QTabWidget;
class QTableWidget;

// OSC and MIDI setup: OSC port, MIDI devices, MIDI mappings with learn, and a live monitor.
class ControlDialog : public QDialog
{
    Q_OBJECT
public:
    ControlDialog(Document *doc, ControlRouter *router, QWidget *parent = nullptr);

    // Adds a mapping for `action` (layer/column 0-based) and starts learning it.
    void learnNew(int action, int layer, int column, int param = 0);

private:
    void refreshOsc();
    void refreshDevices();
    void refreshMappings();
    void refreshDmx();
    void refreshDmxValues();
    void addMapping(int action, int layer, int column, int param = 0);

    Document *m_doc;
    ControlRouter *m_router;
    QCheckBox *m_oscEnabled;
    QSpinBox *m_oscPort;
    QLabel *m_oscStatus;
    QListWidget *m_devices;
    QTableWidget *m_mappings;
    QComboBox *m_newAction;
    QSpinBox *m_newLayer;
    QSpinBox *m_newColumn;
    QSpinBox *m_newParam;
    QPlainTextEdit *m_monitor;
    QTabWidget *m_tabs;
    QCheckBox *m_dmxEnabled;
    QComboBox *m_dmxProtocol;
    QSpinBox *m_dmxUniverse, *m_dmxAddress, *m_dmxLayers;
    QLabel *m_dmxUniverseText;
    QLabel *m_dmxStatus;
    QTableWidget *m_dmxChannels;
    bool m_refreshing = false;
};
