#pragma once

#include "media/HapEncoder.h"

#include <QDialog>

class Document;
class HapConverter;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

// File > Convert to HAP: a queue of movies converted one after another in the background (the
// window can be closed meanwhile). Clips that play a converted file can switch to the HAP copy.
class HapConvertDialog : public QDialog
{
    Q_OBJECT
public:
    HapConvertDialog(Document *doc, QWidget *parent = nullptr);

    void addFiles(const QStringList &paths); // queued and started
    bool isBusy() const;
    // For tests and the status bar: how the queue stands.
    int pending() const;

signals:
    void statusChanged(const QString &text); // empty when idle
    void converted(const QString &source, const QString &target);

private:
    struct Job {
        QString source;
        QString target;
        enum State { Waiting, Running, Done, Failed, Skipped } state = Waiting;
        QString message;
        double progress = 0.0;
    };
    void next();
    void refreshRow(int row);
    void updateButtons();
    void switchClips(const QString &source, const QString &target);

    Document *m_doc;
    HapConverter *m_converter;
    QVector<Job> m_jobs;
    int m_current = -1;
    QTableWidget *m_table;
    QComboBox *m_format;
    QLineEdit *m_folder;
    QCheckBox *m_audio;
    QCheckBox *m_switch;
    QPushButton *m_cancel;
    QPushButton *m_clear;
    QLabel *m_formatHelp;
};
