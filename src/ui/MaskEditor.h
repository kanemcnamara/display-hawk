#pragma once

#include <QWidget>

#include <functional>

class Document;
class QPushButton;
class QVBoxLayout;
struct ProjectionMask;

// Lists the masks of one target, a screen or the composition (kCompositionMasks): draw a new one
// point by point, add a rectangle or ellipse, enable, invert, smooth, feather, delete, and choose
// which one the map edits (its points become handles there).
class MaskEditor : public QWidget
{
    Q_OBJECT
public:
    explicit MaskEditor(Document *doc, QWidget *parent = nullptr);

    void setTarget(int target);
    void refresh();

private:
    void rebuild();
    void edit(int index, const std::function<void(ProjectionMask &)> &fn);
    QSizeF canvasSize() const;

    Document *m_doc;
    int m_target = -1;
    QVBoxLayout *m_list;
    QPushButton *m_draw;
    int m_builtCount = -1;
    int m_builtTarget = -3;
    bool m_refreshing = false;
    QVector<std::function<void()>> m_updaters;
};
