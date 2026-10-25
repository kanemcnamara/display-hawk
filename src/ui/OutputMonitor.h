#pragma once

#include "render/RenderEngine.h"

#include <QImage>
#include <QWidget>

class Document;

// The output monitor: the whole composition as it goes to the outputs (after effects, master and
// blackout), for watching the show from the main window.
class OutputMonitor : public QWidget
{
    Q_OBJECT
public:
    OutputMonitor(Document *doc, RenderEngine *engine, QWidget *parent = nullptr);
    QImage image() const { return m_image; }

    static constexpr int kView = 2; // the render engine's preview slot

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private:
    QRect pictureRect() const;
    void updateRequest();

    Document *m_doc;
    RenderEngine *m_engine;
    QImage m_image;
    PreviewRequest m_request;
};
