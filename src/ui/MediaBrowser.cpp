#include "ui/MediaBrowser.h"

#include "model/Document.h"

#include <QAudioOutput>
#include <QFileSystemModel>
#include <QHeaderView>
#include <QIdentityProxyModel>
#include <QImageReader>
#include <QListView>
#include <QMenu>
#include <QMediaPlayer>
#include <QPainter>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <QVideoSink>

namespace {

const QStringList kImageSuffixes = {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
                                    QStringLiteral("bmp"), QStringLiteral("gif"), QStringLiteral("tif"),
                                    QStringLiteral("tiff")};

QImage fitted(const QImage &img)
{
    QImage out(Thumbnailer::size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(QColor(20, 20, 22));
    if (!img.isNull()) {
        const QImage s = img.scaled(out.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPainter p(&out);
        p.drawImage((out.width() - s.width()) / 2, (out.height() - s.height()) / 2, s);
    }
    return out;
}

} // namespace

// File system model whose icons are thumbnails (a placeholder until they arrive).
class ThumbnailModel : public QIdentityProxyModel
{
public:
    ThumbnailModel(Thumbnailer *thumbs, QObject *parent)
        : QIdentityProxyModel(parent)
        , m_thumbs(thumbs)
    {
        QObject::connect(thumbs, &Thumbnailer::ready, this, [this](const QString &path, const QImage &img) {
            m_cache.insert(path, QPixmap::fromImage(img));
            auto *fs = static_cast<QFileSystemModel *>(sourceModel());
            const QModelIndex i = mapFromSource(fs->index(path));
            if (i.isValid())
                emit dataChanged(i, i, {Qt::DecorationRole});
        });
        m_placeholder = QPixmap(Thumbnailer::size());
        m_placeholder.fill(QColor(32, 32, 36));
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (role == Qt::DecorationRole && index.column() == 0) {
            const QString path = static_cast<QFileSystemModel *>(sourceModel())->filePath(mapToSource(index));
            if (auto it = m_cache.constFind(path); it != m_cache.cend())
                return *it;
            m_thumbs->request(path);
            return m_placeholder;
        }
        return QIdentityProxyModel::data(index, role);
    }

private:
    Thumbnailer *m_thumbs;
    QHash<QString, QPixmap> m_cache;
    QPixmap m_placeholder;
};

Thumbnailer::Thumbnailer(QObject *parent)
    : QObject(parent)
{
}

void Thumbnailer::request(const QString &path)
{
    if (m_requested.contains(path))
        return;
    m_requested.insert(path, true);
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (kImageSuffixes.contains(suffix)) {
        QThreadPool::globalInstance()->start([this, path] {
            QImageReader reader(path);
            reader.setAutoTransform(true);
            const QSize full = reader.size();
            if (full.isValid())
                reader.setScaledSize(full.scaled(size() * 2, Qt::KeepAspectRatio));
            const QImage img = fitted(reader.read());
            QMetaObject::invokeMethod(this, [this, path, img] { emit ready(path, img); }, Qt::QueuedConnection);
        });
        return;
    }
    m_videoQueue << path;
    if (m_videoPath.isEmpty())
        nextVideo();
}

void Thumbnailer::nextVideo()
{
    if (m_videoQueue.isEmpty()) {
        m_videoPath.clear();
        return;
    }
    m_videoPath = m_videoQueue.takeFirst();
    if (!m_player) {
        m_player = new QMediaPlayer(this);
        m_sink = new QVideoSink(this);
        auto *audio = new QAudioOutput(this);
        audio->setMuted(true);
        m_player->setAudioOutput(audio);
        m_player->setVideoSink(m_sink);
        m_timeout = new QTimer(this);
        m_timeout->setSingleShot(true);
        connect(m_timeout, &QTimer::timeout, this, [this] { finishVideo(QImage()); });
        connect(m_sink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &f) {
            if (!m_videoPath.isEmpty() && f.isValid())
                finishVideo(f.toImage());
        });
        connect(m_player, &QMediaPlayer::errorOccurred, this, [this] { finishVideo(QImage()); });
    }
    m_player->setSource(QUrl::fromLocalFile(m_videoPath));
    m_player->setPosition(0);
    m_player->play();
    m_timeout->start(4000);
}

void Thumbnailer::finishVideo(const QImage &image)
{
    if (m_videoPath.isEmpty())
        return;
    const QString path = m_videoPath;
    m_videoPath.clear();
    m_timeout->stop();
    m_player->stop();
    QImage thumb = fitted(image);
    if (image.isNull()) { // no picture: a film-strip mark with the file type
        QPainter p(&thumb);
        p.setPen(QColor(150, 150, 160));
        p.drawText(thumb.rect(), Qt::AlignCenter, QFileInfo(path).suffix().toUpper());
    }
    emit ready(path, thumb);
    QTimer::singleShot(0, this, &Thumbnailer::nextVideo);
}

bool MediaBrowser::isMovie(const QString &path)
{
    static const QStringList movies{QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("mkv"),
                                    QStringLiteral("avi"), QStringLiteral("webm"), QStringLiteral("wmv"),
                                    QStringLiteral("m4v"), QStringLiteral("mpg"), QStringLiteral("mpeg"),
                                    QStringLiteral("mxf")};
    return movies.contains(QFileInfo(path).suffix().toLower());
}

QStringList MediaBrowser::mediaFilters()
{
    QStringList filters;
    for (const char *s : {"mp4", "mov", "mkv", "avi", "webm", "wmv", "m4v", "png", "jpg", "jpeg", "bmp", "gif", "tif", "tiff"})
        filters << QStringLiteral("*.%1").arg(QLatin1String(s));
    return filters;
}

MediaBrowser::MediaBrowser(Document *doc, QWidget *parent)
    : QWidget(parent)
    , m_doc(doc)
{
    m_dirs = new QFileSystemModel(this);
    m_dirs->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot | QDir::Drives);
    m_dirs->setRootPath(QString());
    m_tree = new QTreeView;
    m_tree->setModel(m_dirs);
    for (int c = 1; c < m_dirs->columnCount(); ++c)
        m_tree->hideColumn(c);
    m_tree->header()->hide();

    m_files = new QFileSystemModel(this);
    m_files->setFilter(QDir::Files);
    m_files->setNameFilters(mediaFilters());
    m_files->setNameFilterDisables(false);
    auto *thumbs = new Thumbnailer(this);
    m_thumbs = new ThumbnailModel(thumbs, this);
    m_thumbs->setSourceModel(m_files);
    m_list = new QListView;
    m_list->setModel(m_thumbs);
    m_list->setViewMode(QListView::IconMode);
    m_list->setIconSize(Thumbnailer::size());
    m_list->setGridSize(Thumbnailer::size() + QSize(16, 34));
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    m_list->setWordWrap(true);
    m_list->setUniformItemSizes(true);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setDragEnabled(true); // file URLs: the clip deck takes them
    m_list->setDragDropMode(QAbstractItemView::DragOnly);
    m_list->setToolTip(tr("Drag onto the clip deck, or double-click to add to the selected layer"));

    auto *split = new QSplitter;
    split->addWidget(m_tree);
    split->addWidget(m_list);
    split->setStretchFactor(1, 3);
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(split);

    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &i) {
        setFolder(m_dirs->filePath(i));
    });
    connect(m_list, &QListView::doubleClicked, this, [this](const QModelIndex &i) {
        addToDeck(m_files->filePath(m_thumbs->mapToSource(i)));
    });
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListView::customContextMenuRequested, this, [this](const QPoint &pos) {
        QStringList selected, movies;
        for (const QModelIndex &i : m_list->selectionModel()->selectedIndexes()) {
            const QString path = m_files->filePath(m_thumbs->mapToSource(i));
            selected << path;
            if (isMovie(path))
                movies << path;
        }
        if (selected.isEmpty())
            return;
        QMenu menu(this);
        menu.addAction(tr("Add to the selected layer"), this, [this, selected] {
            for (const QString &path : selected)
                addToDeck(path);
        });
        QAction *convert = menu.addAction(movies.size() > 1 ? tr("Convert %1 movies to HAP...").arg(movies.size())
                                                            : tr("Convert to HAP..."),
                                          this, [this, movies] { emit convertToHapRequested(movies); });
        convert->setEnabled(!movies.isEmpty());
        menu.exec(m_list->viewport()->mapToGlobal(pos));
    });

    const QString start = QSettings().value(QStringLiteral("browser/folder"),
                                            QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).toString();
    setFolder(start);
}

QString MediaBrowser::folder() const
{
    return m_files->rootPath();
}

void MediaBrowser::setFolder(const QString &path)
{
    if (path.isEmpty())
        return;
    m_list->setRootIndex(m_thumbs->mapFromSource(m_files->setRootPath(path)));
    const QModelIndex dirIndex = m_dirs->index(path);
    m_tree->setCurrentIndex(dirIndex);
    m_tree->scrollTo(dirIndex);
    QSettings().setValue(QStringLiteral("browser/folder"), path);
}

void MediaBrowser::addToDeck(const QString &path)
{
    // The next empty slot of the selected layer (layer 1 if none is selected).
    const Selection s = m_doc->selection();
    const int layer = (s.kind == Selection::Layer || s.kind == Selection::Clip) ? s.layer : 0;
    if (layer < 0 || layer >= m_doc->project().layers.size())
        return;
    const QVector<::Clip> &clips = m_doc->project().layers[layer].clips;
    int column = -1;
    for (int c = (s.kind == Selection::Clip ? qMax(0, s.column) : 0); c < m_doc->project().columns; ++c)
        if (c >= clips.size() || clips[c].isEmpty()) {
            column = c;
            break;
        }
    if (column < 0) {
        m_doc->addColumn();
        column = m_doc->project().columns - 1;
    }
    m_doc->setClip(layer, column, ::Clip::fromMedia(path));
    m_doc->select(Selection::ofClip(layer, column));
}
