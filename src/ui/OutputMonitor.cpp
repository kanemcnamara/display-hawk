#include "ui/OutputMonitor.h"

#include "model/Document.h"

#include <QPainter>
#include <QtMath>

OutputMonitor::OutputMonitor(Document *doc, RenderEngine *engine, QWidget *parent)
    : QWidget(parent)
    , m_doc(doc)
    , m_engine(engine)
{
    setMinimumSize(240, 160);
    connect(m_doc, &Document::changed, this, &OutputMonitor::updateRequest); // composition size
    connect(m_engine, &RenderEngine::previewReady, this,
            [this](int view, const QImage &image, const QRectF &, bool outputMode, int) {
                if (view != kView || outputMode)
                    return;
                m_image = image;
                update();
            });
}

QRect OutputMonitor::pictureRect() const
{
    const QRect area = rect().adjusted(4, 20, -4, -4);
    const QSize s = m_doc->project().compSize().scaled(area.size(), Qt::KeepAspectRatio);
    return QRect(area.x() + (area.width() - s.width()) / 2, area.y() + (area.height() - s.height()) / 2, s.width(),
                 s.height());
}

void OutputMonitor::updateRequest()
{
    PreviewRequest req;
    const QRect r = pictureRect();
    if (isVisible() && !r.isEmpty()) {
        const double dpr = devicePixelRatioF();
        req.enabled = true;
        req.region = QRectF(QPointF(), m_doc->project().compSize());
        req.pixelSize = QSize(qCeil(r.width() * dpr), qCeil(r.height() * dpr)).boundedTo(QSize(1920, 1080));
    }
    if (req != m_request) {
        m_request = req;
        m_engine->setPreview(kView, req);
    }
}

void OutputMonitor::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    updateRequest();
}

void OutputMonitor::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    updateRequest();
}

void OutputMonitor::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    updateRequest(); // stops rendering it
}

void OutputMonitor::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(20, 20, 23));
    QFont f = font();
    f.setPixelSize(11);
    p.setFont(f);
    p.setPen(QColor(170, 170, 175));
    const Project &proj = m_doc->project();
    p.drawText(QRect(6, 2, width() - 12, 16), Qt::AlignLeft | Qt::AlignVCenter,
               tr("Output: composition %1 x %2").arg(proj.compWidth).arg(proj.compHeight));
    if (proj.blackout) {
        p.setPen(QColor(255, 90, 90));
        p.drawText(QRect(6, 2, width() - 12, 16), Qt::AlignRight | Qt::AlignVCenter, tr("BLACKOUT"));
    }
    const QRect target = pictureRect();
    p.fillRect(target, Qt::black);
    if (!m_image.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(target, m_image);
    }
    p.setPen(QColor(70, 70, 76));
    p.drawRect(target.adjusted(0, 0, -1, -1));
}
