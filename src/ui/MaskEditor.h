#pragma once

#include <QWidget>

#include <functional>

class Document;
class QVBoxLayout;
struct ProjectionMask;

// Lists one screen's projection masks: add, enable, invert, feather, delete, and choose which one
// the output map edits (its points become handles there).
class MaskEditor : public QWidget
{
    Q_OBJECT
public:
    explicit MaskEditor(Document *doc, QWidget *parent = nullptr);

    void setScreen(int screen);
    void refresh();

private:
    void rebuild();
    void edit(int index, const std::function<void(ProjectionMask &)> &fn);

    Document *m_doc;
    int m_screen = -1;
    QVBoxLayout *m_list;
    int m_builtCount = -1;
    int m_builtScreen = -2;
    bool m_refreshing = false;
    QVector<std::function<void()>> m_updaters;
};
