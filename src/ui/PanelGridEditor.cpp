#include "ui/PanelGridEditor.h"

#include <QMouseEvent>
#include <QPainter>

PanelGridEditor::PanelGridEditor(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
}

void PanelGridEditor::setWall(int cols, int rows, const QVector<QPoint> &disabled, const QColor &color)
{
    const bool resized = cols != m_cols || rows != m_rows;
    m_cols = cols;
    m_rows = rows;
    m_disabled = disabled;
    m_color = color;
    if (resized)
        updateGeometry();
    update();
}

int PanelGridEditor::cellSize() const
{
    if (m_cols <= 0 || m_rows <= 0)
        return 16;
    return qBound(6, qMin(320 / m_cols, 200 / m_rows), 28);
}

QSize PanelGridEditor::sizeHint() const
{
    const int s = cellSize();
    return QSize(m_cols * s + 1, m_rows * s + 1);
}

QPoint PanelGridEditor::cellAt(const QPoint &pos) const
{
    const int s = cellSize();
    const QPoint cell(pos.x() / s, pos.y() / s);
    if (pos.x() < 0 || pos.y() < 0 || cell.x() >= m_cols || cell.y() >= m_rows)
        return QPoint(-1, -1);
    return cell;
}

void PanelGridEditor::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const int s = cellSize();
    for (int r = 0; r < m_rows; ++r) {
        for (int c = 0; c < m_cols; ++c) {
            const QRect cell(c * s, r * s, s, s);
            const bool off = m_disabled.contains(QPoint(c, r));
            p.fillRect(cell, off ? QColor(24, 24, 26) : ((r + c) % 2 ? m_color.darker(220) : m_color.darker(150)));
            p.setPen(QColor(off ? 70 : 200, off ? 70 : 200, off ? 75 : 200));
            p.drawRect(cell.adjusted(0, 0, -1, -1));
            if (off) {
                p.drawLine(cell.topLeft() + QPoint(2, 2), cell.bottomRight() - QPoint(3, 3));
                p.drawLine(cell.topRight() + QPoint(-3, 2), cell.bottomLeft() + QPoint(2, -3));
            }
        }
    }
}

void PanelGridEditor::apply(const QPoint &cell)
{
    if (cell.x() < 0)
        return;
    const bool isOff = m_disabled.contains(cell);
    if (isOff == m_paintDisabled)
        return;
    if (m_paintDisabled)
        m_disabled.append(cell);
    else
        m_disabled.removeAll(cell);
    update();
    emit disabledChanged(m_disabled);
}

void PanelGridEditor::mousePressEvent(QMouseEvent *e)
{
    const QPoint cell = cellAt(e->pos());
    if (e->button() != Qt::LeftButton || cell.x() < 0)
        return;
    // The first cabinet decides whether this drag switches cabinets off or on.
    m_paintDisabled = !m_disabled.contains(cell);
    m_painting = true;
    apply(cell);
}

void PanelGridEditor::mouseMoveEvent(QMouseEvent *e)
{
    const QPoint cell = cellAt(e->pos());
    if (m_painting)
        apply(cell);
    setToolTip(cell.x() >= 0 ? tr("Column %1, row %2").arg(cell.x() + 1).arg(cell.y() + 1) : QString());
}

void PanelGridEditor::mouseReleaseEvent(QMouseEvent *)
{
    m_painting = false;
}
