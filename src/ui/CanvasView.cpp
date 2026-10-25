#include "ui/CanvasView.h"

#include "model/CabinetMapping.h"


#include <QKeyEvent>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QWheelEvent>
#include <QtMath>

namespace {

constexpr double kHandleSize = 7.0;
constexpr double kSnapPixels = 8.0;

// Finds the smallest offset that moves any of the edges onto a candidate within the threshold.
bool bestSnap(std::initializer_list<int> edges, const QVector<int> &candidates, double threshold, int *delta)
{
    bool found = false;
    int best = 0;
    for (int e : edges) {
        for (int c : candidates) {
            const int d = c - e;
            if (qAbs(d) <= threshold && (!found || qAbs(d) < qAbs(best))) {
                best = d;
                found = true;
            }
        }
    }
    if (found)
        *delta = best;
    return found;
}

int snapValue(int v, const QVector<int> &candidates, double threshold)
{
    int d = 0;
    return bestSnap({v}, candidates, threshold, &d) ? v + d : v;
}

} // namespace

CanvasView::CanvasView(Document *doc, RenderEngine *engine, Mode mode, QWidget *parent)
    : QWidget(parent)
    , m_doc(doc)
    , m_engine(engine)
    , m_mode(mode)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(200, 150);
    setAttribute(Qt::WA_OpaquePaintEvent);
    connect(m_doc, &Document::changed, this, qOverload<>(&QWidget::update));
    connect(m_doc, &Document::selectionChanged, this, qOverload<>(&QWidget::update));
    connect(m_engine, &RenderEngine::previewReady, this, &CanvasView::onPreview);
}

void CanvasView::setScreenIndex(int index)
{
    if (m_screen == index)
        return;
    m_screen = index;
    m_preview = QImage();
    resetView();
}

void CanvasView::resetView()
{
    m_zoom = 1.0;
    m_pan = QPointF();
    update();
}

void CanvasView::updatePreviewRequest()
{
    PreviewRequest req;
    const Project &p = m_doc->project();
    const bool validScreen = m_mode == InputMap || (m_screen >= 0 && m_screen < p.screens.size());
    if (validScreen && isVisible()) {
        // Only the visible part of the canvas, at the widget's device-pixel resolution.
        const QRectF visible = QRectF(toCanvas(QPointF(0, 0)), toCanvas(QPointF(width(), height())))
                                   .intersected(QRectF(QPointF(), canvasSize()));
        const double dpr = devicePixelRatioF();
        const QSize px(qCeil(visible.width() * scale() * dpr), qCeil(visible.height() * scale() * dpr));
        if (!visible.isEmpty() && !px.isEmpty()) {
            req.enabled = true;
            req.outputMode = m_mode == OutputMap;
            req.screen = m_mode == OutputMap ? m_screen : -1;
            req.region = visible;
            req.pixelSize = px;
        }
    }
    if (req != m_request) {
        m_request = req;
        m_engine->setPreview(int(m_mode), req);
    }
}

void CanvasView::onPreview(int view, const QImage &image, const QRectF &region, bool outputMode, int screen)
{
    if (view != int(m_mode) || outputMode != (m_mode == OutputMap) || (outputMode && screen != m_screen))
        return;
    m_preview = image;
    m_previewRegion = region;
    update();
}

QSize CanvasView::canvasSize() const
{
    const Project &p = m_doc->project();
    if (m_mode == InputMap)
        return p.compSize();
    if (m_screen >= 0 && m_screen < p.screens.size())
        return QSize(p.screens[m_screen].width, p.screens[m_screen].height);
    return QSize(1920, 1080);
}

double CanvasView::scale() const
{
    const QSize c = canvasSize();
    const double base = qMin((width() - 40.0) / c.width(), (height() - 40.0) / c.height());
    return qMax(0.0001, base * m_zoom);
}

QPointF CanvasView::origin() const
{
    const QSize c = canvasSize();
    const double s = scale();
    return QPointF(width() / 2.0 - c.width() * s / 2.0, height() / 2.0 - c.height() * s / 2.0) + m_pan;
}

QPointF CanvasView::toWidget(const QPointF &c) const
{
    return origin() + c * scale();
}

QPointF CanvasView::toCanvas(const QPointF &w) const
{
    return (w - origin()) / scale();
}

QRectF CanvasView::toWidget(const QRect &r) const
{
    return QRectF(toWidget(QPointF(r.x(), r.y())), QSizeF(r.width(), r.height()) * scale());
}

QPolygonF CanvasView::toWidget(const QPolygonF &poly) const
{
    QPolygonF out;
    for (const QPointF &pt : poly)
        out << toWidget(pt);
    return out;
}

bool CanvasView::contains(const Item &item, const QPointF &c)
{
    return item.poly.isEmpty() ? QRectF(item.rect).contains(c) : item.poly.containsPoint(c, Qt::OddEvenFill);
}

QVector<CanvasView::Item> CanvasView::items() const
{
    const Project &p = m_doc->project();
    const Selection sel = m_doc->selection();
    QVector<Item> list;
    if (m_mode == InputMap) {
        for (int i = 0; i < p.walls.size(); ++i) {
            const LedWall &w = p.walls[i];
            Item item{Selection::ofWall(i), w.rect(), w.color, w.name, false, false, {}};
            if (w.rotated())
                item.poly = w.outline();
            list.append(item);
        }
        const int activeScreen = (sel.kind == Selection::Screen || sel.kind == Selection::Slice) ? sel.screen : -1;
        for (int s = 0; s < p.screens.size(); ++s) {
            const Screen &sc = p.screens[s];
            for (int i = 0; i < sc.slices.size(); ++i) {
                const Slice &sl = sc.slices[i];
                const bool dim = activeScreen >= 0 && activeScreen != s;
                Item item{Selection::ofSlice(s, i), sl.input, QColor(255, 160, 40),
                          sc.name + QStringLiteral(" / ") + sl.name, true, dim || !sl.enabled, {}};
                if (sl.angle != 0.0) { // resized numerically; dragging moves it
                    item.poly = sl.inputPolygon();
                    item.resizable = false;
                }
                list.append(item);
            }
        }
    } else if (m_screen >= 0 && m_screen < p.screens.size()) {
        const Screen &sc = p.screens[m_screen];
        for (int i = 0; i < sc.slices.size(); ++i) {
            const Slice &sl = sc.slices[i];
            // Cabinets are small: column/row and port are what matter on the output.
            const QString label = sl.isCabinet()
                                      ? QStringLiteral("C%1 R%2%3").arg(sl.cabinet.x() + 1).arg(sl.cabinet.y() + 1)
                                            .arg(sl.port > 0 ? QStringLiteral(" P%1").arg(sl.port) : QString())
                                      : sl.name;
            Item item{Selection::ofSlice(m_screen, i), sl.output,
                      sl.port > 0 ? CabinetMapping::portColor(sl.port) : QColor(255, 160, 40), label, true,
                      !sl.enabled, {}};
            if (sl.isWarped()) { // corner pin: its corners are the handles
                item.poly = sl.warp;
                item.resizable = false;
            }
            list.append(item);
        }
    }
    return list;
}

bool CanvasView::ownsSelection(const Selection &s) const
{
    if (m_mode == InputMap)
        return s.kind == Selection::Wall || s.kind == Selection::Slice;
    return s.kind == Selection::Slice && s.screen == m_screen;
}

QRect CanvasView::rectFor(const Selection &s) const
{
    const Project &p = m_doc->project();
    if (s.kind == Selection::Wall && s.wall >= 0 && s.wall < p.walls.size())
        return p.walls[s.wall].rect();
    if (s.kind == Selection::Slice && s.screen >= 0 && s.screen < p.screens.size()
        && s.slice >= 0 && s.slice < p.screens[s.screen].slices.size()) {
        const Slice &sl = p.screens[s.screen].slices[s.slice];
        return m_mode == InputMap ? sl.input : sl.output;
    }
    return {};
}

void CanvasView::setRectFor(const Selection &s, const QRect &r)
{
    Project &p = m_doc->project();
    if (s.kind == Selection::Wall) {
        LedWall w = p.walls[s.wall];
        w.x = r.x();
        w.y = r.y();
        m_doc->applyWall(s.wall, w);
    } else if (s.kind == Selection::Slice) {
        Slice &sl = p.screens[s.screen].slices[s.slice];
        if (m_mode == OutputMap && sl.isWarped()) // moving a pinned slice moves its corners
            sl.warp.translate(QPointF(r.topLeft() - sl.output.topLeft()));
        (m_mode == InputMap ? sl.input : sl.output) = r;
        m_doc->notifyChanged();
    }
}

QVector<QPointF> CanvasView::handlePositions(const QRectF &r) const
{
    const double cx = r.center().x(), cy = r.center().y();
    return {r.topLeft(), {cx, r.top()}, r.topRight(), {r.right(), cy},
            r.bottomRight(), {cx, r.bottom()}, r.bottomLeft(), {r.left(), cy}};
}

QPolygonF CanvasView::selectedWarp() const
{
    const Selection sel = m_doc->selection();
    const Project &p = m_doc->project();
    if (m_mode != OutputMap || sel.kind != Selection::Slice || sel.screen != m_screen || sel.screen >= p.screens.size()
        || sel.slice < 0 || sel.slice >= p.screens[sel.screen].slices.size())
        return {};
    return p.screens[sel.screen].slices[sel.slice].warp;
}

int CanvasView::hitHandle(const QPointF &pos) const
{
    const Selection sel = m_doc->selection();
    if (!ownsSelection(sel) || sel.kind != Selection::Slice)
        return -1;
    const QPolygonF warp = selectedWarp();
    if (warp.size() == 4) {
        for (int k = 0; k < 4; ++k) {
            const QPointF h = toWidget(warp[k]);
            if (qAbs(h.x() - pos.x()) <= kHandleSize + 2 && qAbs(h.y() - pos.y()) <= kHandleSize + 2)
                return kCornerHandle + k;
        }
        return -1;
    }
    for (const Item &it : items())
        if (it.sel == sel && !it.resizable)
            return -1;
    const QVector<QPointF> hs = handlePositions(toWidget(rectFor(sel)));
    for (int i = 0; i < hs.size(); ++i)
        if (qAbs(hs[i].x() - pos.x()) <= kHandleSize && qAbs(hs[i].y() - pos.y()) <= kHandleSize)
            return i;
    return -1;
}

int CanvasView::hitItem(const QVector<Item> &list, const QPointF &pos) const
{
    const QPointF c = toCanvas(pos);
    const Selection sel = m_doc->selection();
    // The current selection wins so it can always be dragged even under other items.
    for (int i = 0; i < list.size(); ++i)
        if (list[i].sel == sel && contains(list[i], c))
            return i;
    for (int i = list.size() - 1; i >= 0; --i)
        if (contains(list[i], c))
            return i;
    return -1;
}

void CanvasView::snapCandidates(QVector<int> &xs, QVector<int> &ys) const
{
    const QSize c = canvasSize();
    xs = {0, c.width()};
    ys = {0, c.height()};
    for (const Item &it : items()) {
        if (it.sel == m_dragSel)
            continue;
        xs << it.rect.x() << it.rect.x() + it.rect.width();
        ys << it.rect.y() << it.rect.y() + it.rect.height();
    }
    if (m_mode == InputMap && m_dragSel.kind == Selection::Slice) {
        // Panel seams are the natural place to cut slices for LED processors.
        for (const LedWall &w : m_doc->project().walls) {
            if (w.rotated())
                continue;
            for (int i = 1; i < w.cols; ++i)
                xs << w.x + i * w.panelPxW;
            for (int i = 1; i < w.rows; ++i)
                ys << w.y + i * w.panelPxH;
        }
    }
}

void CanvasView::dragTo(const QPointF &widgetPos, bool snapping)
{
    const QPointF d = toCanvas(widgetPos) - toCanvas(m_pressPos);
    const int dx = qRound(d.x()), dy = qRound(d.y());
    const double threshold = kSnapPixels / scale();
    QVector<int> xs, ys;
    if (snapping)
        snapCandidates(xs, ys);

    if (m_drag == Corner) {
        const Selection s = m_dragSel;
        Slice &sl = m_doc->project().screens[s.screen].slices[s.slice];
        QPolygonF warp = m_startWarp;
        QPointF pt = warp[m_handle - kCornerHandle] + QPointF(dx, dy);
        if (snapping) { // corners snap to the screen edges and to other slices' edges
            QVector<int> xs, ys;
            snapCandidates(xs, ys);
            pt = QPointF(snapValue(qRound(pt.x()), xs, threshold), snapValue(qRound(pt.y()), ys, threshold));
        }
        warp[m_handle - kCornerHandle] = pt;
        sl.warp = warp;
        sl.output = warp.boundingRect().toAlignedRect(); // keeps the slice's bounds for sizing and reports
        m_doc->notifyChanged();
        return;
    }
    QRect r = m_startRect;
    if (m_drag == Move) {
        r.translate(dx, dy);
        int sx = 0, sy = 0;
        if (snapping && bestSnap({r.x(), r.x() + r.width()}, xs, threshold, &sx))
            r.translate(sx, 0);
        if (snapping && bestSnap({r.y(), r.y() + r.height()}, ys, threshold, &sy))
            r.translate(0, sy);
    } else if (m_drag == Resize) {
        int l = r.x(), t = r.y(), rt = r.x() + r.width(), b = r.y() + r.height();
        const int h = m_handle;
        const bool left = h == 0 || h == 6 || h == 7;
        const bool right = h == 2 || h == 3 || h == 4;
        const bool top = h == 0 || h == 1 || h == 2;
        const bool bottom = h == 4 || h == 5 || h == 6;
        if (left) l = snapping ? snapValue(l + dx, xs, threshold) : l + dx;
        if (right) rt = snapping ? snapValue(rt + dx, xs, threshold) : rt + dx;
        if (top) t = snapping ? snapValue(t + dy, ys, threshold) : t + dy;
        if (bottom) b = snapping ? snapValue(b + dy, ys, threshold) : b + dy;
        if (left) l = qMin(l, rt - 1);
        if (right) rt = qMax(rt, l + 1);
        if (top) t = qMin(t, b - 1);
        if (bottom) b = qMax(b, t + 1);
        r = QRect(l, t, rt - l, b - t);
    }
    if (r != rectFor(m_dragSel))
        setRectFor(m_dragSel, r);
}

void CanvasView::paintEvent(QPaintEvent *)
{
    updatePreviewRequest();

    const Project &p = m_doc->project();
    const QSize cs = canvasSize();
    const QRectF canvasRect = toWidget(QRect(QPoint(), cs));
    const bool validScreen = m_mode == InputMap || (m_screen >= 0 && m_screen < p.screens.size());

    QPainter painter(this);
    painter.fillRect(rect(), QColor(28, 28, 31));
    painter.fillRect(canvasRect, Qt::black);
    if (validScreen && !m_preview.isNull()) {
        // The preview may lag the view by a frame while zooming or panning; it is drawn where its
        // region sits now. Nearest-neighbour when zoomed in so individual LED pixels stay crisp.
        painter.setRenderHint(QPainter::SmoothPixmapTransform, scale() < 1.0);
        const QRectF dst(toWidget(m_previewRegion.topLeft()), m_previewRegion.size() * scale());
        painter.drawImage(dst, m_preview);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    }

    painter.setPen(QPen(QColor(90, 90, 95), 1));
    painter.drawRect(canvasRect);

    const Selection sel = m_doc->selection();
    QFont labelFont = font();
    labelFont.setPixelSize(11);
    painter.setFont(labelFont);
    const QVector<Item> list = items();
    const Item *selected = nullptr;

    for (const Item &it : list) {
        const QRectF wr = toWidget(it.rect);
        const bool isSel = it.sel == sel;
        if (isSel)
            selected = &it;
        const bool outOfBounds = !QRect(QPoint(), cs).contains(it.rect);
        QColor c = outOfBounds && it.sel.kind == Selection::Slice ? QColor(255, 70, 70) : it.color;
        if (it.dim && !isSel)
            c.setAlpha(90);
        if (isSel) {
            QColor fill = c;
            fill.setAlpha(45);
            painter.fillRect(wr, fill);
        }
        painter.setPen(QPen(c, isSel ? 2 : 1, it.sel.kind == Selection::Wall ? Qt::DashLine : Qt::SolidLine));
        if (!it.poly.isEmpty()) {
            painter.drawPolygon(toWidget(it.poly));
            continue; // rotated: no label box
        }
        painter.drawRect(wr);
        if (wr.width() > 40 && wr.height() > 16) {
            const QRectF lr(wr.x() + 3, wr.y() + 2, wr.width() - 6, 14);
            painter.setPen(c);
            painter.drawText(lr, Qt::AlignLeft | Qt::AlignVCenter,
                             painter.fontMetrics().elidedText(it.label, Qt::ElideRight, int(lr.width())));
        }
    }

    // Processor port routes through the cabinets, in chain order, with an arrow at each step.
    if (m_mode == OutputMap && validScreen) {
        painter.setRenderHint(QPainter::Antialiasing);
        for (const CabinetMapping::PortRoute &route : CabinetMapping::portRoutes(p.screens[m_screen])) {
            const QPolygonF pts = toWidget(route.points);
            painter.setPen(QPen(route.color, 2));
            painter.drawPolyline(pts);
            for (int i = 1; i < pts.size(); ++i) {
                const QPointF a = pts[i - 1], b = pts[i];
                const QPointF mid = (a + b) / 2;
                const double len = qSqrt(QPointF::dotProduct(b - a, b - a));
                if (len < 14)
                    continue;
                const QPointF dir = (b - a) / len, n(-dir.y(), dir.x());
                painter.setBrush(route.color);
                painter.drawPolygon(QPolygonF({mid + dir * 5, mid - dir * 4 + n * 4, mid - dir * 4 - n * 4}));
                painter.setBrush(Qt::NoBrush);
            }
            if (!pts.isEmpty()) {
                const QString text = tr("Port %1").arg(route.port);
                const QRectF tr(pts.first() + QPointF(-4, -22), QSizeF(painter.fontMetrics().horizontalAdvance(text) + 8, 16));
                painter.fillRect(tr, route.color);
                painter.setPen(Qt::black);
                painter.drawText(tr, Qt::AlignCenter, text);
            }
        }
        painter.setRenderHint(QPainter::Antialiasing, false);
    }

    if (selected) {
        const QRectF wr = toWidget(selected->rect);
        const QPolygonF warp = selectedWarp();
        if (warp.size() == 4) { // corner-pin handles, numbered clockwise from the top-left
            painter.setRenderHint(QPainter::Antialiasing);
            for (int k = 0; k < 4; ++k) {
                const QPointF h = toWidget(warp[k]);
                painter.setPen(QPen(Qt::black, 1));
                painter.setBrush(QColor(255, 210, 60));
                painter.drawEllipse(h, kHandleSize, kHandleSize);
                painter.setPen(Qt::black);
                painter.drawText(QRectF(h.x() - kHandleSize, h.y() - kHandleSize, 2 * kHandleSize, 2 * kHandleSize),
                                 Qt::AlignCenter, QString::number(k + 1));
            }
            painter.setBrush(Qt::NoBrush);
            painter.setRenderHint(QPainter::Antialiasing, false);
        }
        if (selected->resizable) {
            painter.setPen(QPen(Qt::black, 1));
            painter.setBrush(Qt::white);
            for (const QPointF &h : handlePositions(wr))
                painter.drawRect(QRectF(h.x() - kHandleSize / 2, h.y() - kHandleSize / 2, kHandleSize, kHandleSize));
            painter.setBrush(Qt::NoBrush);
        }
        const QRect r = selected->rect;
        const QString info = QStringLiteral("%1 x %2  @ %3, %4").arg(r.width()).arg(r.height()).arg(r.x()).arg(r.y());
        const QRectF ir(wr.x(), wr.bottom() + 4, 220, 16);
        painter.fillRect(QRectF(ir.topLeft(), QSizeF(painter.fontMetrics().horizontalAdvance(info) + 8, 16)),
                         QColor(0, 0, 0, 180));
        painter.setPen(Qt::white);
        painter.drawText(ir.adjusted(4, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, info);
    }

    QString title;
    if (m_mode == InputMap)
        title = tr("Composition  %1 x %2").arg(cs.width()).arg(cs.height());
    else if (validScreen)
        title = tr("%1  %2 x %3").arg(p.screens[m_screen].name).arg(cs.width()).arg(cs.height());
    else
        title = tr("No screen selected");
    title += tr("   |   zoom %1%").arg(qRound(scale() * 100));
    painter.setPen(QColor(200, 200, 200));
    painter.drawText(QRectF(8, 4, width() - 16, 18), Qt::AlignLeft | Qt::AlignVCenter, title);
}

void CanvasView::mousePressEvent(QMouseEvent *e)
{
    setFocus();
    m_pressPos = m_lastPos = e->position();
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && e->modifiers() & Qt::ControlModifier)) {
        m_drag = Pan;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;

    const int handle = hitHandle(e->position());
    if (handle >= kCornerHandle) {
        m_drag = Corner;
        m_handle = handle;
        m_dragSel = m_doc->selection();
        m_startWarp = selectedWarp();
        return;
    }
    if (handle >= 0) {
        m_drag = Resize;
        m_handle = handle;
        m_dragSel = m_doc->selection();
        m_startRect = rectFor(m_dragSel);
        return;
    }

    const QVector<Item> list = items();
    int hit = hitItem(list, e->position());
    if (hit >= 0 && (e->modifiers() & Qt::AltModifier)) {
        // Alt+click cycles through items stacked under the cursor.
        const QPointF c = toCanvas(e->position());
        QVector<int> under;
        for (int i = list.size() - 1; i >= 0; --i)
            if (QRectF(list[i].rect).contains(c))
                under << i;
        const int cur = under.indexOf(hit);
        hit = under[(cur + 1) % under.size()];
    }
    if (hit >= 0) {
        m_doc->select(list[hit].sel);
        m_drag = Move;
        m_dragSel = list[hit].sel;
        m_startRect = list[hit].rect;
    } else {
        if (m_mode == InputMap)
            m_doc->select(Selection::composition());
        else if (m_screen >= 0 && m_screen < m_doc->project().screens.size())
            m_doc->select(Selection::ofScreen(m_screen));
        m_drag = Pan;
        setCursor(Qt::ClosedHandCursor);
    }
}

void CanvasView::mouseMoveEvent(QMouseEvent *e)
{
    const QPointF pos = e->position();
    if (m_drag == Pan) {
        m_pan += pos - m_lastPos;
        update();
    } else if (m_drag == Move || m_drag == Resize || m_drag == Corner) {
        dragTo(pos, !(e->modifiers() & Qt::ShiftModifier));
    } else {
        static const Qt::CursorShape shapes[] = {Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor,
                                                 Qt::SizeHorCursor,   Qt::SizeFDiagCursor, Qt::SizeVerCursor,
                                                 Qt::SizeBDiagCursor, Qt::SizeHorCursor};
        const int h = hitHandle(pos);
        setCursor(h >= kCornerHandle ? Qt::CrossCursor : (h >= 0 ? shapes[h] : Qt::ArrowCursor));
    }
    m_lastPos = pos;
    const QPointF c = toCanvas(pos);
    emit statusMessage(tr("%1 px  %2, %3").arg(m_mode == InputMap ? tr("Composition") : tr("Screen"))
                           .arg(qFloor(c.x())).arg(qFloor(c.y())));
}

void CanvasView::mouseReleaseEvent(QMouseEvent *)
{
    if (m_drag == Move || m_drag == Resize || m_drag == Corner)
        m_doc->commitUndo(); // one undo step per drag
    m_drag = NoDrag;
    m_handle = -1;
    unsetCursor();
}

void CanvasView::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (hitItem(items(), e->position()) < 0)
        resetView();
}

void CanvasView::zoomAt(const QPointF &pos, double factor)
{
    const QPointF before = toCanvas(pos);
    m_zoom = qBound(0.05, m_zoom * factor, 200.0);
    m_pan += pos - toWidget(before);
    update();
}

void CanvasView::wheelEvent(QWheelEvent *e)
{
    // Trackpads (pixel deltas) scroll to pan, like other macOS canvases; Ctrl/Cmd+scroll and mouse
    // wheels zoom around the cursor.
    if (!e->pixelDelta().isNull() && !(e->modifiers() & Qt::ControlModifier)) {
        m_pan += QPointF(e->pixelDelta());
        update();
        return;
    }
    zoomAt(e->position(), qPow(1.0015, e->angleDelta().y()));
}

bool CanvasView::event(QEvent *e)
{
    if (e->type() == QEvent::NativeGesture) {
        auto *g = static_cast<QNativeGestureEvent *>(e);
        if (g->gestureType() == Qt::ZoomNativeGesture) { // trackpad pinch
            zoomAt(g->position(), 1.0 + g->value());
            return true;
        }
    }
    return QWidget::event(e);
}

void CanvasView::keyPressEvent(QKeyEvent *e)
{
    const Selection sel = m_doc->selection();
    const int step = (e->modifiers() & Qt::ShiftModifier) ? 10 : 1;
    QPoint delta;
    switch (e->key()) {
    case Qt::Key_Left: delta = QPoint(-step, 0); break;
    case Qt::Key_Right: delta = QPoint(step, 0); break;
    case Qt::Key_Up: delta = QPoint(0, -step); break;
    case Qt::Key_Down: delta = QPoint(0, step); break;
    case Qt::Key_F: resetView(); return;
    default: QWidget::keyPressEvent(e); return;
    }
    if (ownsSelection(sel))
        setRectFor(sel, rectFor(sel).translated(delta));
}
