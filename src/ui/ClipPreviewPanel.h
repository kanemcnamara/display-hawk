#pragma once

#include <QImage>
#include <QWidget>

class Document;
class RenderEngine;

// The preview monitor: the clip selected in the deck, with its transform and effects, as it would
// look in its layer's area, before it is triggered (and without affecting the outputs).
class ClipPreviewPanel : public QWidget
{
    Q_OBJECT
public:
    ClipPreviewPanel(Document *doc, RenderEngine *engine, QWidget *parent = nullptr);
    QImage image() const { return m_image; }

protected:
    void paintEvent(QPaintEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private:
    void follow(); // previews the selected clip, if the panel is shown

    Document *m_doc;
    RenderEngine *m_engine;
    QImage m_image;
    QString m_clipId;
};
