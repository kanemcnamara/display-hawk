#pragma once

#include "model/Document.h"
#include "render/RenderEngine.h"

#include <QImage>
#include <QWidget>

// Zoomable editor for either the composition (slice inputs, walls) or one screen (slice outputs).
class CanvasView : public QWidget
{
    Q_OBJECT
public:
    enum Mode { InputMap, OutputMap };

    CanvasView(Document *doc, RenderEngine *engine, Mode mode, QWidget *parent = nullptr);

    void setScreenIndex(int index);
    int screenIndex() const { return m_screen; }
    void resetView();

signals:
    void statusMessage(const QString &text);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    bool event(QEvent *e) override;

private:
    struct Item {
        Selection sel;
        QRect rect;
        QColor color;
        QString label;
        bool resizable = true;
        bool dim = false;
    };

    QVector<Item> items() const;
    QSize canvasSize() const;
    double scale() const;
    QPointF origin() const;
    QPointF toWidget(const QPointF &c) const;
    QPointF toCanvas(const QPointF &w) const;
    QRectF toWidget(const QRect &r) const;
    QVector<QPointF> handlePositions(const QRectF &wr) const;
    int hitHandle(const QPointF &pos) const;
    int hitItem(const QVector<Item> &list, const QPointF &pos) const;
    bool ownsSelection(const Selection &s) const;
    QRect rectFor(const Selection &s) const;
    void setRectFor(const Selection &s, const QRect &r);
    void dragTo(const QPointF &widgetPos, bool snapping);
    void snapCandidates(QVector<int> &xs, QVector<int> &ys) const;
    void updatePreviewRequest();
    void zoomAt(const QPointF &pos, double factor);
    void onPreview(int view, const QImage &image, const QRectF &region, bool outputMode, int screen);

    Document *m_doc;
    RenderEngine *m_engine;
    Mode m_mode;
    int m_screen = 0;
    double m_zoom = 1.0;
    QPointF m_pan;

    enum DragKind { NoDrag, Pan, Move, Resize };
    DragKind m_drag = NoDrag;
    int m_handle = -1;
    Selection m_dragSel;
    QRect m_startRect;
    QPointF m_pressPos;
    QPointF m_lastPos;

    // Latest image from the render thread and the canvas region it shows.
    QImage m_preview;
    QRectF m_previewRegion;
    PreviewRequest m_request;
};
