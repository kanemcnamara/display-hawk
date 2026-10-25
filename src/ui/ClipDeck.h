#pragma once

#include <QHash>
#include <QPixmap>
#include <QWidget>

class ClipGrid;
class Document;
class RenderEngine;
class TempoClock;
class QComboBox;
class QTabBar;
class QDoubleSpinBox;
class QPushButton;
class QSlider;
class Thumbnailer;
class QCheckBox;
class QScrollArea;
class QVBoxLayout;
struct Clip;
struct Project;

// Resolume-style clip launcher: one row per layer (top layer first), one column per clip slot.
// Layer controls sit on the left, master fader and blackout on top.
class ClipDeck : public QWidget
{
    Q_OBJECT
public:
    ClipDeck(Document *doc, RenderEngine *engine, TempoClock *tempo, QWidget *parent = nullptr);

signals:
    // Right-click "MIDI learn" on a clip or column: ControlAction, layer and column (0-based).
    void midiLearnRequested(int action, int layer, int column);
    void convertToHapRequested(const QStringList &paths);

public:
    static constexpr int kHeaderH = 24;
    static constexpr int kMinCellW = 100; // clip cells: width set by the size slider or "fit"
    static constexpr int kMaxCellW = 360;
    static int rowHeightFor(int cellWidth) { return qMax(86, cellWidth * 9 / 16 + 22); } // 16:9 + the name

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    void rebuildRows();
    void refresh();
    void applyCellSize(); // from the slider, or what fits the space when "Fit" is on

    Document *m_doc;
    QSlider *m_master;
    QPushButton *m_blackout;
    QSlider *m_audioMaster;
    QPushButton *m_audioMute;
    QDoubleSpinBox *m_bpm;
    QSlider *m_crossfader;
    QTabBar *m_deckTabs;
    void rebuildDeckTabs();
    bool m_rebuildingTabs = false;
    QComboBox *m_tempoSource, *m_quantize;
    QWidget *m_beatLamps;
    int m_beatInBar = -1;
    QVBoxLayout *m_rowLayout;
    QSlider *m_cellSize;
    QCheckBox *m_fit;
    QScrollArea *m_scroll;
    QVector<QWidget *> m_rows;
    ClipGrid *m_grid;
    bool m_refreshing = false;
};

// The grid of clip cells and column headers; painted, not built from widgets.
class ClipGrid : public QWidget
{
    Q_OBJECT
public:
    ClipGrid(Document *doc, RenderEngine *engine, QWidget *parent = nullptr);
    void updateSize();
    // The columns shown: a deck's range (count -1 = every column).
    void setColumnRange(int first, int count);
    int columnsShown() const;
    void setCellWidth(int width);
    int cellWidth() const { return m_cellW; }
    int rowHeight() const { return m_rowH; }

signals:
    void midiLearnRequested(int action, int layer, int column);
    void convertToHapRequested(const QStringList &paths);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dragLeaveEvent(QDragLeaveEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    // Cell under a point: layer index (not row) and column, or -1/-1. Column headers give layer -2.
    QPair<int, int> cellAt(const QPoint &pos) const;
    QRect cellRect(int layer, int column) const;
    QPixmap thumbnail(const Clip &clip, const Project &project);
    void browseInto(int layer, int column);

    Document *m_doc;
    RenderEngine *m_engine;
    int m_first = 0;
    int m_count = -1;
    int lastColumn() const; // one past the last column shown
    QHash<QByteArray, QPixmap> m_thumbs;
    int m_cellW = 128;
    int m_rowH = 86;
    Thumbnailer *m_frames;                // first frames of movies, for their thumbnails
    QHash<QString, QImage> m_movieFrames;      // by media path
    QPair<int, int> m_pressCell{-1, -1};
    QPoint m_pressPos;
    bool m_moving = false;
    QPair<int, int> m_dropCell{-1, -1};
};
