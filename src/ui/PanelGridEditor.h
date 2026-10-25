#pragma once

#include <QPoint>
#include <QVector>
#include <QWidget>

// A wall's panels as a clickable grid: click a cabinet to mark it not fitted (or fitted again);
// dragging paints the same state over several cabinets.
class PanelGridEditor : public QWidget
{
    Q_OBJECT
public:
    explicit PanelGridEditor(QWidget *parent = nullptr);

    void setWall(int cols, int rows, const QVector<QPoint> &disabled, const QColor &color);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

signals:
    void disabledChanged(const QVector<QPoint> &disabled);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    int cellSize() const;
    QPoint cellAt(const QPoint &pos) const;
    void apply(const QPoint &cell);

    int m_cols = 0;
    int m_rows = 0;
    QVector<QPoint> m_disabled;
    QColor m_color;
    bool m_painting = false;
    bool m_paintDisabled = false;
};
