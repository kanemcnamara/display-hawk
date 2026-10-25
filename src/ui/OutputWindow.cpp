#include "ui/OutputWindow.h"

#include <QKeyEvent>
#include <QPlatformSurfaceEvent>

OutputWindow::OutputWindow(QSurface::SurfaceType surfaceType, int screenIndex)
    : m_screenIndex(screenIndex)
{
    setSurfaceType(surfaceType);
}

void OutputWindow::exposeEvent(QExposeEvent *)
{
    m_exposed.store(isExposed());
}

void OutputWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape)
        emit stopRequested();
    else
        QWindow::keyPressEvent(e);
}

bool OutputWindow::event(QEvent *e)
{
    // Never let the platform window be destroyed under the render thread's swapchain; the owner
    // detaches the swapchain first and then deletes this window.
    if (e->type() == QEvent::Close) {
        e->ignore();
        emit stopRequested();
        return true;
    }
    if (e->type() == QEvent::PlatformSurface
        && static_cast<QPlatformSurfaceEvent *>(e)->surfaceEventType()
               == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
        m_exposed.store(false);
    return QWindow::event(e);
}
