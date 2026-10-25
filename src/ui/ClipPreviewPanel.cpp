#include "ui/ClipPreviewPanel.h"

#include "model/Document.h"
#include "render/RenderEngine.h"

#include <QPainter>

namespace {
constexpr QSize kPreviewSize(640, 360);
}

ClipPreviewPanel::ClipPreviewPanel(Document *doc, RenderEngine *engine, QWidget *parent)
    : QWidget(parent)
    , m_doc(doc)
    , m_engine(engine)
{
    setMinimumSize(240, 160);
    connect(m_doc, &Document::selectionChanged, this, &ClipPreviewPanel::follow);
    connect(m_doc, &Document::structureChanged, this, &ClipPreviewPanel::follow);
    connect(m_engine, &RenderEngine::clipPreviewReady, this, [this](const QString &clipId, const QImage &img) {
        if (clipId != m_clipId)
            return;
        m_image = img;
        update();
    });
}

void ClipPreviewPanel::follow()
{
    const ::Clip *clip = isVisible() ? m_doc->selectedClip() : nullptr;
    const QString id = clip ? clip->id : QString();
    if (id != m_clipId) {
        m_clipId = id;
        m_image = QImage();
        update();
    }
    m_engine->setPreviewClip(id, kPreviewSize);
}

void ClipPreviewPanel::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    follow();
}

void ClipPreviewPanel::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    follow(); // stops previewing (and releases the clip's media if it is not playing)
}

void ClipPreviewPanel::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(20, 20, 23));
    const ::Clip *clip = m_doc->selectedClip();
    QFont f = font();
    f.setPixelSize(11);
    p.setFont(f);
    p.setPen(QColor(170, 170, 175));
    const QRect header(6, 2, width() - 12, 16);
    if (!clip) {
        p.drawText(rect(), Qt::AlignCenter, tr("Select a clip in the deck to preview it here\n(without sending it to the outputs)"));
        return;
    }
    p.drawText(header, Qt::AlignLeft | Qt::AlignVCenter, tr("Preview: %1").arg(clip->displayName()));
    const QRect area = rect().adjusted(4, 20, -4, -4);
    if (m_image.isNull()) {
        p.drawText(area, Qt::AlignCenter, tr("Loading..."));
        return;
    }
    const QSize s = m_image.size().scaled(area.size(), Qt::KeepAspectRatio);
    const QRect target(area.x() + (area.width() - s.width()) / 2, area.y() + (area.height() - s.height()) / 2, s.width(), s.height());
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(target, m_image);
    p.setPen(QColor(70, 70, 76));
    p.drawRect(target.adjusted(0, 0, -1, -1));
}
