#pragma once

#include <QHash>
#include <QPixmap>
#include <QStringList>
#include <QWidget>

class Document;
class QFileSystemModel;
class QListView;
class QMediaPlayer;
class QTimer;
class QTreeView;
class QVideoSink;
class ThumbnailModel;

// Finds and grabs thumbnails: images scaled on a worker thread, videos by playing them muted
// until the first frame arrives (one at a time). Results arrive on the GUI thread.
class Thumbnailer : public QObject
{
    Q_OBJECT
public:
    explicit Thumbnailer(QObject *parent = nullptr, const QSize &size = defaultSize());
    void request(const QString &path);
    static QSize defaultSize() { return QSize(128, 72); }
    QSize size() const { return m_size; }

signals:
    void ready(const QString &path, const QImage &image);

private:
    void nextVideo();
    void finishVideo(const QImage &image);

    QStringList m_videoQueue;
    QString m_videoPath;
    QMediaPlayer *m_player = nullptr;
    QVideoSink *m_sink = nullptr;
    QTimer *m_timeout = nullptr;
    QHash<QString, bool> m_requested;
    QSize m_size;
};

// Media browser: folders on the left, the media in the chosen folder as thumbnails on the right.
// Drag files onto the clip deck, or double-click to fill the next empty slot of the selected layer.
class MediaBrowser : public QWidget
{
    Q_OBJECT
public:
    MediaBrowser(Document *doc, QWidget *parent = nullptr);
    void setFolder(const QString &path);
    QString folder() const;
    static QStringList mediaFilters();
    static bool isMovie(const QString &path); // a video file (not a still image)

signals:
    void convertToHapRequested(const QStringList &paths);

private:
    void addToDeck(const QString &path);

    Document *m_doc;
    QFileSystemModel *m_dirs;
    QFileSystemModel *m_files;
    ThumbnailModel *m_thumbs;
    QTreeView *m_tree;
    QListView *m_list;
};
