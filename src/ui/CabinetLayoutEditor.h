#pragma once

#include <QWidget>

#include <functional>

class Document;
class QComboBox;
class QVBoxLayout;
struct CabinetLayout;

// Edits the cabinet layouts of one screen: which walls are mapped cabinet by cabinet, how the
// cabinets are arranged on the output and how they are chained onto processor ports.
class CabinetLayoutEditor : public QWidget
{
    Q_OBJECT
public:
    explicit CabinetLayoutEditor(Document *doc, QWidget *parent = nullptr);

    void setScreen(int screen);
    void refresh();

private:
    void rebuild();
    void edit(int index, const std::function<void(CabinetLayout &)> &fn);

    Document *m_doc;
    int m_screen = -1;
    QVBoxLayout *m_list;
    QComboBox *m_addWall;
    QVector<int> m_builtFor; // walls of the layouts the widgets were built for
    int m_builtScreen = -2;
    bool m_refreshing = false;
    QVector<std::function<void()>> m_updaters; // push the model's values into each layout's widgets
};
