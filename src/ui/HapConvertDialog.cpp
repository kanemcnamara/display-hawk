#include "ui/HapConvertDialog.h"

#include "media/HapConverter.h"
#include "media/MovReader.h"
#include "model/Document.h"
#include "ui/MediaBrowser.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

QString formatHelp(Hap::Format f)
{
    switch (f) {
    case Hap::Format::Hap: return QObject::tr("Smallest files and fastest playback; fine for most content.");
    case Hap::Format::HapAlpha: return QObject::tr("Keeps transparency (logos, overlays). Twice the size of Hap.");
    case Hap::Format::HapQ: return QObject::tr("Best quality (smooth gradients, fine detail, text). Twice the size of Hap.");
    }
    return {};
}

bool samePath(const QString &a, const QString &b)
{
    return QFileInfo(a).absoluteFilePath().compare(QFileInfo(b).absoluteFilePath(),
#ifdef Q_OS_WIN
                                                   Qt::CaseInsensitive
#else
                                                   Qt::CaseSensitive
#endif
                                                   ) == 0;
}

} // namespace

HapConvertDialog::HapConvertDialog(Document *doc, QWidget *parent)
    : QDialog(parent)
    , m_doc(doc)
    , m_converter(new HapConverter(this))
{
    setWindowTitle(tr("Convert to HAP"));
    resize(820, 480);
    QSettings s;

    m_table = new QTableWidget(0, 3);
    m_table->setHorizontalHeaderLabels({tr("File"), tr("Progress"), tr("Result")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_table->horizontalHeader()->resizeSection(1, 160);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->verticalHeader()->hide();
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);

    m_format = new QComboBox;
    for (Hap::Format f : {Hap::Format::Hap, Hap::Format::HapQ, Hap::Format::HapAlpha})
        m_format->addItem(Hap::formatName(f), int(f));
    m_format->setCurrentIndex(qMax(0, m_format->findData(s.value(QStringLiteral("hap/format"), 0).toInt())));
    m_formatHelp = new QLabel;
    m_formatHelp->setEnabled(false);
    auto *formatRow = new QHBoxLayout;
    formatRow->addWidget(m_format);
    formatRow->addWidget(m_formatHelp, 1);

    m_folder = new QLineEdit(s.value(QStringLiteral("hap/folder")).toString());
    m_folder->setPlaceholderText(tr("Next to the original"));
    m_folder->setClearButtonEnabled(true);
    auto *choose = new QPushButton(tr("Choose..."));
    auto *folderRow = new QHBoxLayout;
    folderRow->addWidget(m_folder, 1);
    folderRow->addWidget(choose);

    m_audio = new QCheckBox(tr("Keep the sound (as uncompressed PCM)"));
    m_audio->setChecked(s.value(QStringLiteral("hap/audio"), true).toBool());
    m_switch = new QCheckBox(tr("Switch clips that play the original to the HAP file when it is done"));
    m_switch->setChecked(s.value(QStringLiteral("hap/switchClips"), true).toBool());

    auto *form = new QFormLayout;
    form->addRow(tr("Format"), formatRow);
    form->addRow(tr("Save to"), folderRow);
    form->addRow(m_audio);
    form->addRow(m_switch);

    auto *add = new QPushButton(tr("Add files..."));
    m_cancel = new QPushButton(tr("Stop"));
    m_cancel->setToolTip(tr("Stop the file being converted and everything waiting"));
    m_clear = new QPushButton(tr("Clear finished"));
    auto *close = new QPushButton(tr("Close"));
    close->setToolTip(tr("Conversions carry on in the background"));
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(add);
    buttons->addWidget(m_cancel);
    buttons->addWidget(m_clear);
    buttons->addStretch();
    buttons->addWidget(close);

    auto *note = new QLabel(tr("HAP movies decode on the GPU: many layers of HD or 4K play smoothly, and scrubbing, "
                               "reverse and bounce are instant. Files are several times larger than H.264. Converting "
                               "uses most of the CPU at low priority, so the show keeps running."));
    note->setWordWrap(true);
    note->setEnabled(false);

    auto *v = new QVBoxLayout(this);
    v->addLayout(form);
    v->addWidget(m_table, 1);
    v->addWidget(note);
    v->addLayout(buttons);

    auto syncHelp = [this] { m_formatHelp->setText(formatHelp(Hap::Format(m_format->currentData().toInt()))); };
    syncHelp();
    connect(m_format, &QComboBox::currentIndexChanged, this, [syncHelp, this] {
        syncHelp();
        QSettings().setValue(QStringLiteral("hap/format"), m_format->currentData().toInt());
    });
    connect(m_folder, &QLineEdit::textChanged, this, [](const QString &t) { QSettings().setValue(QStringLiteral("hap/folder"), t); });
    connect(m_audio, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue(QStringLiteral("hap/audio"), on); });
    connect(m_switch, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue(QStringLiteral("hap/switchClips"), on); });
    connect(choose, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Save HAP files to"), m_folder->text());
        if (!dir.isEmpty())
            m_folder->setText(dir);
    });
    connect(add, &QPushButton::clicked, this, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(this, tr("Movies to convert to HAP"), QString(),
                                                                tr("Movies (*.mp4 *.mov *.m4v *.avi *.mkv *.webm *.wmv *.mpg *.mpeg *.mxf);;All files (*)"));
        addFiles(files);
    });
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        for (Job &j : m_jobs)
            if (j.state == Job::Waiting) {
                j.state = Job::Skipped;
                j.message = tr("Stopped");
            }
        m_converter->cancel();
        for (int i = 0; i < m_jobs.size(); ++i)
            refreshRow(i);
        updateButtons();
    });
    connect(m_clear, &QPushButton::clicked, this, [this] {
        for (int i = int(m_jobs.size()) - 1; i >= 0; --i) {
            if (m_jobs[i].state == Job::Waiting || m_jobs[i].state == Job::Running)
                continue;
            m_jobs.removeAt(i);
            m_table->removeRow(i);
            if (m_current > i)
                --m_current;
        }
        updateButtons();
    });
    connect(close, &QPushButton::clicked, this, &QDialog::hide);

    connect(m_converter, &HapConverter::progress, this, [this](double f) {
        if (m_current < 0 || m_current >= m_jobs.size())
            return;
        m_jobs[m_current].progress = f;
        refreshRow(m_current);
        emit statusChanged(tr("HAP %1/%2: %3%").arg(m_current + 1).arg(m_jobs.size()).arg(qRound(f * 100)));
    });
    connect(m_converter, &HapConverter::finished, this, [this](bool ok, const QString &message) {
        if (m_current < 0 || m_current >= m_jobs.size())
            return;
        Job &j = m_jobs[m_current];
        if (j.state == Job::Running) {
            j.state = ok ? Job::Done : Job::Failed;
            j.progress = ok ? 1.0 : j.progress;
            j.message = ok ? tr("%1 -> %2").arg(message, QFileInfo(j.target).fileName()) : message;
        }
        refreshRow(m_current);
        if (ok) {
            if (m_switch->isChecked())
                switchClips(j.source, j.target);
            emit converted(j.source, j.target);
        }
        m_current = -1;
        next();
    });
    updateButtons();
}

void HapConvertDialog::addFiles(const QStringList &paths)
{
    for (const QString &path : paths) {
        if (path.isEmpty())
            continue;
        bool queued = false;
        for (const Job &j : std::as_const(m_jobs))
            queued = queued || ((j.state == Job::Waiting || j.state == Job::Running) && samePath(j.source, path));
        if (queued)
            continue;
        Job j;
        j.source = QFileInfo(path).absoluteFilePath();
        if (MovReader::isHap(path)) {
            j.state = Job::Skipped;
            j.message = tr("Already HAP");
        }
        m_jobs.append(j);
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        m_table->setItem(row, 0, new QTableWidgetItem(QFileInfo(path).fileName()));
        m_table->item(row, 0)->setToolTip(j.source);
        auto *bar = new QProgressBar;
        bar->setRange(0, 1000);
        bar->setTextVisible(true);
        m_table->setCellWidget(row, 1, bar);
        m_table->setItem(row, 2, new QTableWidgetItem);
        refreshRow(row);
    }
    next();
}

bool HapConvertDialog::isBusy() const
{
    return m_converter->isRunning();
}

int HapConvertDialog::pending() const
{
    int n = 0;
    for (const Job &j : m_jobs)
        n += j.state == Job::Waiting || j.state == Job::Running;
    return n;
}

void HapConvertDialog::next()
{
    if (m_converter->isRunning())
        return;
    for (int i = 0; i < m_jobs.size(); ++i) {
        Job &j = m_jobs[i];
        if (j.state != Job::Waiting)
            continue;
        const Hap::Format format = Hap::Format(m_format->currentData().toInt());
        j.target = HapConverter::defaultTarget(j.source, format, m_folder->text().trimmed());
        HapConverter::Options o;
        o.format = format;
        o.audio = m_audio->isChecked();
        j.state = Job::Running;
        j.message = tr("Converting to %1...").arg(Hap::formatName(format));
        m_current = i;
        refreshRow(i);
        updateButtons();
        m_converter->start(j.source, j.target, o);
        return;
    }
    updateButtons();
    emit statusChanged(QString());
}

void HapConvertDialog::refreshRow(int row)
{
    if (row < 0 || row >= m_jobs.size() || row >= m_table->rowCount())
        return;
    const Job &j = m_jobs[row];
    if (auto *bar = qobject_cast<QProgressBar *>(m_table->cellWidget(row, 1)))
        bar->setValue(qRound(j.progress * 1000));
    QString text = j.message;
    if (j.state == Job::Waiting)
        text = tr("Waiting");
    else if (j.state == Job::Failed)
        text = tr("Failed: %1").arg(j.message);
    QTableWidgetItem *item = m_table->item(row, 2);
    item->setText(text);
    item->setToolTip(j.target);
    item->setForeground(j.state == Job::Failed ? QColor(255, 110, 110)
                        : j.state == Job::Done ? QColor(110, 220, 120)
                                               : palette().color(QPalette::Text));
}

void HapConvertDialog::updateButtons()
{
    m_cancel->setEnabled(pending() > 0);
}

void HapConvertDialog::switchClips(const QString &source, const QString &target)
{
    Project &p = m_doc->project();
    int switched = 0;
    for (Layer &l : p.layers)
        for (Clip &c : l.clips)
            if (!c.isEmpty() && c.source == ContentSource::Media && samePath(c.mediaPath, source)) {
                c.mediaPath = target;
                ++switched;
            }
    if (switched)
        m_doc->notifyChanged(true);
}
