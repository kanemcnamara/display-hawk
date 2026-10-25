#pragma once

#include <QDialog>

class Document;
class QTableWidget;

// Control > LFOs: tempo-synced waves on layer opacity, the master or any effect parameter.
class LfoDialog : public QDialog
{
    Q_OBJECT
public:
    LfoDialog(Document *doc, QWidget *parent = nullptr);

private:
    void rebuild();

    Document *m_doc;
    QTableWidget *m_table;
};
