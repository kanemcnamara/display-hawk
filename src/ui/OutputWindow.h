#pragma once

#include <QWindow>

#include <atomic>

// Borderless window for one Screen. The render thread owns its swapchain and draws into it.
class OutputWindow : public QWindow
{
    Q_OBJECT
public:
    OutputWindow(QSurface::SurfaceType surfaceType, int screenIndex);

    // Read from the render thread.
    int screenIndex() const { return m_screenIndex.load(); }
    bool isRenderable() const { return m_exposed.load(); }

    void setScreenIndex(int index) { m_screenIndex.store(index); }

signals:
    // Esc pressed or the window was asked to close; the owner turns outputs off.
    void stopRequested();

protected:
    void exposeEvent(QExposeEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    bool event(QEvent *e) override;

private:
    std::atomic<int> m_screenIndex;
    std::atomic<bool> m_exposed{false};
};
