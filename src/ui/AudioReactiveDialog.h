#pragma once

#include "audio/AudioAnalyzer.h"

#include <QDialog>

class AudioReactive;
class Document;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QTableWidget;

// Live meters for the analysed input: level, bass, mid, treble and a beat lamp.
class AudioMeters : public QWidget
{
    Q_OBJECT
public:
    explicit AudioMeters(QWidget *parent = nullptr);
    void setLevels(const AudioLevels &levels);
    QSize sizeHint() const override { return QSize(360, 110); }

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    AudioLevels m_levels;
    double m_beatGlow = 0.0;
};

// Audio-reactive setup: input, sensitivity, meters and the links that drive the show.
class AudioReactiveDialog : public QDialog
{
    Q_OBJECT
public:
    AudioReactiveDialog(Document *doc, AudioReactive *reactive, QWidget *parent = nullptr);

private:
    void refresh();
    void refreshLinks();

    Document *m_doc;
    AudioReactive *m_reactive;
    QCheckBox *m_enabled;
    QComboBox *m_device;
    QDoubleSpinBox *m_sensitivity;
    QLabel *m_status;
    AudioMeters *m_meters;
    QTableWidget *m_links;
    bool m_refreshing = false;
};
