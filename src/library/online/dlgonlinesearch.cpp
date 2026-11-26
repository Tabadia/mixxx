#include "library/online/dlgonlinesearch.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "library/library.h"
#include "library/trackcollectionmanager.h"
#include "moc_dlgonlinesearch.cpp"
#include "track/track.h"
#include "track/trackref.h"
#include "util/logger.h"
#include "widget/wlibrary.h"

namespace {

const mixxx::Logger kLogger("DlgOnlineSearch");

const QString kConfigGroup = QStringLiteral("[OnlineSearch]");
const ConfigKey kYtDlpPathKey(kConfigGroup, QStringLiteral("YtDlpPath"));
const ConfigKey kFfmpegPathKey(kConfigGroup, QStringLiteral("FfmpegPath"));
const ConfigKey kDownloadDirKey(kConfigGroup, QStringLiteral("DownloadDir"));
const ConfigKey kSourceKey(kConfigGroup, QStringLiteral("Source"));

constexpr int kMaxResults = 25;
constexpr int kUrlRole = Qt::UserRole;

const QString kFileMarker = QStringLiteral("MIXXXFILE=");
const QString kProgressMarker = QStringLiteral("MIXXXPROGRESS=");

QString formatDuration(double seconds) {
    if (seconds <= 0) {
        return QString();
    }
    const int total = static_cast<int>(seconds + 0.5);
    const int h = total / 3600;
    const int m = (total % 3600) / 60;
    const int s = total % 60;
    if (h > 0) {
        return QStringLiteral("%1:%2:%3")
                .arg(h)
                .arg(m, 2, 10, QChar('0'))
                .arg(s, 2, 10, QChar('0'));
    }
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

/// Look for an executable in PATH and in a few well known install locations
/// (winget, scoop, next to mixxx.exe).
QString findTool(const QString& name) {
    QString found = QStandardPaths::findExecutable(name);
    if (!found.isEmpty()) {
        return found;
    }
    QStringList extraDirs;
    extraDirs << QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    if (!localAppData.isEmpty()) {
        extraDirs << localAppData + QStringLiteral("/Microsoft/WinGet/Links");
    }
    extraDirs << QDir::homePath() + QStringLiteral("/scoop/shims");
#else
    extraDirs << QStringLiteral("/usr/local/bin")
              << QStringLiteral("/opt/homebrew/bin")
              << QDir::homePath() + QStringLiteral("/.local/bin");
#endif
    found = QStandardPaths::findExecutable(name, extraDirs);
    return found;
}

} // anonymous namespace

DlgOnlineSearch::DlgOnlineSearch(
        WLibrary* pParent, UserSettingsPointer pConfig, Library* pLibrary)
        : QWidget(pParent),
          m_pConfig(pConfig),
          m_pLibrary(pLibrary),
          m_currentJob{} {
    setObjectName(QStringLiteral("DlgOnlineSearch"));

    // --- search bar ---
    m_pSourceCombo = new QComboBox(this);
    m_pSourceCombo->addItem(tr("YouTube"), QStringLiteral("ytsearch"));
    m_pSourceCombo->addItem(tr("SoundCloud"), QStringLiteral("scsearch"));
    m_pSourceCombo->setCurrentIndex(
            m_pConfig->getValue(kSourceKey, 0) == 1 ? 1 : 0);
    connect(m_pSourceCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [this](int index) {
                m_pConfig->setValue(kSourceKey, index);
            });

    m_pQueryEdit = new QLineEdit(this);
    m_pQueryEdit->setPlaceholderText(
            tr("Search YouTube / SoundCloud, or paste a link, then press Enter"));
    m_pQueryEdit->setClearButtonEnabled(true);
    connect(m_pQueryEdit, &QLineEdit::returnPressed, this, &DlgOnlineSearch::slotSearch);

    m_pSearchButton = new QPushButton(tr("Search"), this);
    connect(m_pSearchButton, &QPushButton::clicked, this, &DlgOnlineSearch::slotSearch);

    auto* pSearchRow = new QHBoxLayout();
    pSearchRow->addWidget(m_pSourceCombo);
    pSearchRow->addWidget(m_pQueryEdit, 1);
    pSearchRow->addWidget(m_pSearchButton);

    // --- results ---
    m_pResults = new QTableWidget(0, kColCount, this);
    m_pResults->setHorizontalHeaderLabels(
            {tr("Title"), tr("Uploader"), tr("Duration"), tr("Status")});
    m_pResults->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pResults->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_pResults->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pResults->verticalHeader()->setVisible(false);
    m_pResults->setAlternatingRowColors(true);
    m_pResults->setWordWrap(false);
    QHeaderView* pHeader = m_pResults->horizontalHeader();
    pHeader->setSectionResizeMode(kColTitle, QHeaderView::Stretch);
    pHeader->setSectionResizeMode(kColUploader, QHeaderView::Interactive);
    pHeader->setSectionResizeMode(kColDuration, QHeaderView::ResizeToContents);
    pHeader->setSectionResizeMode(kColStatus, QHeaderView::Interactive);
    m_pResults->setColumnWidth(kColUploader, 200);
    m_pResults->setColumnWidth(kColStatus, 160);
    connect(m_pResults,
            &QTableWidget::cellDoubleClicked,
            this,
            &DlgOnlineSearch::slotDownloadSelected);

    // --- action bar ---
    m_pDownloadButton = new QPushButton(tr("Download to Library"), this);
    m_pDeck1Button = new QPushButton(tr("Download -> Deck 1"), this);
    m_pDeck2Button = new QPushButton(tr("Download -> Deck 2"), this);
    m_pFolderButton = new QPushButton(tr("Download Folder..."), this);
    connect(m_pDownloadButton,
            &QPushButton::clicked,
            this,
            &DlgOnlineSearch::slotDownloadSelected);
    connect(m_pDeck1Button,
            &QPushButton::clicked,
            this,
            &DlgOnlineSearch::slotDownloadToDeck1);
    connect(m_pDeck2Button,
            &QPushButton::clicked,
            this,
            &DlgOnlineSearch::slotDownloadToDeck2);
    connect(m_pFolderButton,
            &QPushButton::clicked,
            this,
            &DlgOnlineSearch::slotChooseFolder);

    m_pStatusLabel = new QLabel(this);
    m_pStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* pActionRow = new QHBoxLayout();
    pActionRow->addWidget(m_pDownloadButton);
    pActionRow->addWidget(m_pDeck1Button);
    pActionRow->addWidget(m_pDeck2Button);
    pActionRow->addStretch(1);
    pActionRow->addWidget(m_pFolderButton);

    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(4, 4, 4, 4);
    pLayout->addLayout(pSearchRow);
    pLayout->addWidget(m_pResults, 1);
    pLayout->addLayout(pActionRow);
    pLayout->addWidget(m_pStatusLabel);

    if (ytDlpPath().isEmpty()) {
        setStatus(tr("yt-dlp was not found. Install it (e.g. 'winget install "
                     "yt-dlp.yt-dlp') or set [OnlineSearch] YtDlpPath in "
                     "mixxx.cfg."));
    } else {
        setStatus(tr("Downloads are saved to %1").arg(QDir::toNativeSeparators(downloadDir())));
    }
}

DlgOnlineSearch::~DlgOnlineSearch() {
    m_downloadQueue.clear();
    for (QProcess* pProcess : {m_pSearchProcess.data(), m_pDownloadProcess.data()}) {
        if (pProcess) {
            pProcess->disconnect(this);
            pProcess->kill();
            pProcess->waitForFinished(2000);
        }
    }
}

bool DlgOnlineSearch::hasFocus() const {
    return m_pQueryEdit->hasFocus() || m_pResults->hasFocus();
}

void DlgOnlineSearch::setFocus() {
    m_pResults->setFocus();
}

void DlgOnlineSearch::focusSearchBox() {
    m_pQueryEdit->setFocus();
    m_pQueryEdit->selectAll();
}

// ---------------------------------------------------------------------------
// Tool / path configuration
// ---------------------------------------------------------------------------

QString DlgOnlineSearch::ytDlpPath() const {
    const QString configured = m_pConfig->getValueString(kYtDlpPathKey);
    if (!configured.isEmpty() && QFileInfo::exists(configured)) {
        return configured;
    }
    return findTool(QStringLiteral("yt-dlp"));
}

QString DlgOnlineSearch::ffmpegPath() const {
    const QString configured = m_pConfig->getValueString(kFfmpegPathKey);
    if (!configured.isEmpty() && QFileInfo::exists(configured)) {
        return configured;
    }
    return findTool(QStringLiteral("ffmpeg"));
}

QString DlgOnlineSearch::downloadDir() const {
    QString dir = m_pConfig->getValueString(kDownloadDirKey);
    if (dir.isEmpty()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::MusicLocation) +
                QStringLiteral("/Mixxx Downloads");
    }
    return dir;
}

QProcessEnvironment DlgOnlineSearch::processEnvironment() const {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // yt-dlp is a Python program; force UTF-8 on its pipes so that
    // non-ASCII titles and file names survive on Windows.
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    return env;
}

void DlgOnlineSearch::slotChooseFolder() {
    const QString dir = QFileDialog::getExistingDirectory(
            this, tr("Choose download folder"), downloadDir());
    if (dir.isEmpty()) {
        return;
    }
    m_pConfig->set(kDownloadDirKey, ConfigValue(dir));
    setStatus(tr("Downloads are saved to %1").arg(QDir::toNativeSeparators(dir)));
}

void DlgOnlineSearch::setStatus(const QString& text) {
    m_pStatusLabel->setText(text);
}

// ---------------------------------------------------------------------------
// Searching
// ---------------------------------------------------------------------------

void DlgOnlineSearch::slotSearch() {
    const QString query = m_pQueryEdit->text().trimmed();
    if (query.isEmpty()) {
        return;
    }
    const QString ytDlp = ytDlpPath();
    if (ytDlp.isEmpty()) {
        setStatus(tr("yt-dlp was not found, cannot search."));
        return;
    }
    if (m_pSearchProcess) {
        m_pSearchProcess->disconnect(this);
        m_pSearchProcess->kill();
        m_pSearchProcess->deleteLater();
    }

    m_pResults->setRowCount(0);
    m_searchBuffer.clear();
    m_searchError.clear();

    // A pasted link is resolved directly; otherwise use the search prefix.
    QString target;
    const bool isUrl = query.startsWith(QStringLiteral("http://")) ||
            query.startsWith(QStringLiteral("https://"));
    if (isUrl) {
        target = query;
    } else {
        target = QStringLiteral("%1%2:%3")
                         .arg(m_pSourceCombo->currentData().toString())
                         .arg(kMaxResults)
                         .arg(query);
    }

    QStringList args;
    args << QStringLiteral("--flat-playlist")
         << QStringLiteral("--dump-json")
         << QStringLiteral("--no-warnings")
         << QStringLiteral("--ignore-errors")
         << target;

    m_pSearchProcess = new QProcess(this);
    m_pSearchProcess->setProcessEnvironment(processEnvironment());
    connect(m_pSearchProcess,
            &QProcess::readyReadStandardOutput,
            this,
            &DlgOnlineSearch::slotSearchOutput);
    connect(m_pSearchProcess,
            &QProcess::readyReadStandardError,
            this,
            [this]() {
                const QString err = QString::fromUtf8(
                        m_pSearchProcess->readAllStandardError());
                for (const QString& line : err.split('\n', Qt::SkipEmptyParts)) {
                    if (line.startsWith(QStringLiteral("ERROR"))) {
                        m_searchError = line.trimmed();
                    }
                }
            });
    connect(m_pSearchProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            &DlgOnlineSearch::slotSearchFinished);
    connect(m_pSearchProcess,
            &QProcess::errorOccurred,
            this,
            [this](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) {
                    setStatus(tr("Could not start yt-dlp."));
                    m_pSearchButton->setEnabled(true);
                }
            });

    setStatus(isUrl ? tr("Resolving link...")
                    : tr("Searching %1 for \"%2\"...")
                              .arg(m_pSourceCombo->currentText(), query));
    m_pSearchButton->setEnabled(false);
    kLogger.debug() << "Running" << ytDlp << args;
    m_pSearchProcess->start(ytDlp, args);
}

void DlgOnlineSearch::slotSearchOutput() {
    if (!m_pSearchProcess) {
        return;
    }
    m_searchBuffer += m_pSearchProcess->readAllStandardOutput();
    int newline;
    while ((newline = m_searchBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_searchBuffer.left(newline).trimmed();
        m_searchBuffer.remove(0, newline + 1);
        if (!line.isEmpty()) {
            addResultFromJson(line);
        }
    }
}

void DlgOnlineSearch::addResultFromJson(const QByteArray& line) {
    const QJsonDocument doc = QJsonDocument::fromJson(line);
    if (!doc.isObject()) {
        return;
    }
    const QJsonObject obj = doc.object();

    QString url = obj.value(QStringLiteral("webpage_url")).toString();
    if (url.isEmpty()) {
        url = obj.value(QStringLiteral("url")).toString();
    }
    if (url.isEmpty()) {
        return;
    }
    QString title = obj.value(QStringLiteral("title")).toString();
    if (title.isEmpty()) {
        // SoundCloud flat results sometimes omit the title: derive it from
        // the URL slug (".../artist/track-name").
        title = QUrl(url).path().section('/', -1).replace('-', ' ');
    }
    QString uploader = obj.value(QStringLiteral("uploader")).toString();
    if (uploader.isEmpty()) {
        uploader = obj.value(QStringLiteral("channel")).toString();
    }
    if (uploader.isEmpty()) {
        uploader = QUrl(url).path().section('/', 1, 1);
    }
    const QString duration = formatDuration(
            obj.value(QStringLiteral("duration")).toDouble());

    const int row = m_pResults->rowCount();
    m_pResults->insertRow(row);
    auto* pTitle = new QTableWidgetItem(title);
    pTitle->setData(kUrlRole, url);
    pTitle->setToolTip(url);
    m_pResults->setItem(row, kColTitle, pTitle);
    m_pResults->setItem(row, kColUploader, new QTableWidgetItem(uploader));
    m_pResults->setItem(row, kColDuration, new QTableWidgetItem(duration));
    m_pResults->setItem(row, kColStatus, new QTableWidgetItem());
}

void DlgOnlineSearch::slotSearchFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    Q_UNUSED(exitCode);
    slotSearchOutput(); // flush remaining output
    m_pSearchButton->setEnabled(true);
    const int count = m_pResults->rowCount();
    if (exitStatus != QProcess::NormalExit) {
        setStatus(tr("Search aborted."));
    } else if (count == 0) {
        setStatus(m_searchError.isEmpty() ? tr("No results.") : m_searchError);
    } else {
        setStatus(tr("%1 results. Double-click or select and press a download "
                     "button.")
                          .arg(count));
        m_pResults->selectRow(0);
    }
    if (m_pSearchProcess) {
        m_pSearchProcess->deleteLater();
    }
}

// ---------------------------------------------------------------------------
// Downloading
// ---------------------------------------------------------------------------

void DlgOnlineSearch::slotDownloadSelected() {
    enqueueSelected(QString());
}

void DlgOnlineSearch::slotDownloadToDeck1() {
    enqueueSelected(QStringLiteral("[Channel1]"));
}

void DlgOnlineSearch::slotDownloadToDeck2() {
    enqueueSelected(QStringLiteral("[Channel2]"));
}

void DlgOnlineSearch::enqueueSelected(const QString& group) {
    const QModelIndexList rows = m_pResults->selectionModel()->selectedRows(kColTitle);
    if (rows.isEmpty()) {
        setStatus(tr("Select a result first."));
        return;
    }
    if (ytDlpPath().isEmpty()) {
        setStatus(tr("yt-dlp was not found, cannot download."));
        return;
    }
    for (const QModelIndex& index : rows) {
        const QString url = index.data(kUrlRole).toString();
        if (url.isEmpty() || url == m_currentJob.url) {
            continue;
        }
        bool alreadyQueued = false;
        for (const DownloadJob& job : std::as_const(m_downloadQueue)) {
            if (job.url == url) {
                alreadyQueued = true;
                break;
            }
        }
        if (alreadyQueued) {
            continue;
        }
        m_downloadQueue.append(DownloadJob{
                url, index.data(Qt::DisplayRole).toString(), group});
        setRowStatus(url, tr("Queued"));
    }
    if (!m_pDownloadProcess) {
        startNextDownload();
    }
}

void DlgOnlineSearch::setRowStatus(const QString& url, const QString& status) {
    // Rows may have been replaced by a newer search, so locate them by URL.
    if (url.isEmpty()) {
        return;
    }
    for (int row = 0; row < m_pResults->rowCount(); ++row) {
        QTableWidgetItem* pTitle = m_pResults->item(row, kColTitle);
        if (pTitle && pTitle->data(kUrlRole).toString() == url) {
            QTableWidgetItem* pStatus = m_pResults->item(row, kColStatus);
            if (pStatus) {
                pStatus->setText(status);
            }
        }
    }
}

void DlgOnlineSearch::startNextDownload() {
    if (m_downloadQueue.isEmpty()) {
        m_currentJob = DownloadJob{};
        return;
    }
    m_currentJob = m_downloadQueue.takeFirst();
    m_currentFile.clear();
    m_currentError.clear();
    m_downloadBuffer.clear();

    const QString dir = downloadDir();
    QDir().mkpath(dir);

    QStringList args;
    args << QStringLiteral("--no-playlist")
         << QStringLiteral("--newline")
         << QStringLiteral("--no-warnings")
         << QStringLiteral("-f") << QStringLiteral("bestaudio/best")
         << QStringLiteral("--extract-audio")
         << QStringLiteral("--audio-format") << QStringLiteral("mp3")
         << QStringLiteral("--audio-quality") << QStringLiteral("0")
         << QStringLiteral("--embed-metadata")
         << QStringLiteral("--embed-thumbnail")
         << QStringLiteral("--convert-thumbnails") << QStringLiteral("jpg")
         << QStringLiteral("--paths") << dir
         << QStringLiteral("-o") << QStringLiteral("%(uploader)s - %(title)s.%(ext)s")
         << QStringLiteral("--progress")
         << QStringLiteral("--progress-template")
         << QStringLiteral("download:") + kProgressMarker +
                    QStringLiteral("%(progress._percent_str)s")
         << QStringLiteral("--print")
         << QStringLiteral("after_move:") + kFileMarker + QStringLiteral("%(filepath)s");
    const QString ffmpeg = ffmpegPath();
    if (!ffmpeg.isEmpty()) {
        args << QStringLiteral("--ffmpeg-location") << ffmpeg;
    }
    args << m_currentJob.url;

    m_pDownloadProcess = new QProcess(this);
    m_pDownloadProcess->setProcessEnvironment(processEnvironment());
    m_pDownloadProcess->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_pDownloadProcess,
            &QProcess::readyReadStandardOutput,
            this,
            &DlgOnlineSearch::slotDownloadOutput);
    connect(m_pDownloadProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            &DlgOnlineSearch::slotDownloadFinished);
    connect(m_pDownloadProcess,
            &QProcess::errorOccurred,
            this,
            [this](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) {
                    m_currentError = tr("Could not start yt-dlp.");
                    slotDownloadFinished(-1, QProcess::CrashExit);
                }
            });

    setStatus(tr("Downloading \"%1\"... (%2 more queued)")
                      .arg(m_currentJob.title)
                      .arg(m_downloadQueue.size()));
    setRowStatus(m_currentJob.url, tr("Downloading..."));
    kLogger.debug() << "Running" << ytDlpPath() << args;
    m_pDownloadProcess->start(ytDlpPath(), args);
}

void DlgOnlineSearch::slotDownloadOutput() {
    if (!m_pDownloadProcess) {
        return;
    }
    m_downloadBuffer += m_pDownloadProcess->readAllStandardOutput();
    int newline;
    while ((newline = m_downloadBuffer.indexOf('\n')) >= 0) {
        const QString line = QString::fromUtf8(m_downloadBuffer.left(newline)).trimmed();
        m_downloadBuffer.remove(0, newline + 1);
        if (line.startsWith(kFileMarker)) {
            m_currentFile = line.mid(kFileMarker.size());
        } else if (line.startsWith(kProgressMarker)) {
            const QString percent = line.mid(kProgressMarker.size()).trimmed();
            setRowStatus(m_currentJob.url, tr("Downloading %1").arg(percent));
        } else if (line.startsWith(QStringLiteral("[ExtractAudio]"))) {
            setRowStatus(m_currentJob.url, tr("Converting to MP3..."));
        } else if (line.startsWith(QStringLiteral("ERROR"))) {
            m_currentError = line;
        }
    }
}

void DlgOnlineSearch::slotDownloadFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    if (!m_pDownloadProcess) {
        return;
    }
    slotDownloadOutput();
    m_pDownloadProcess->disconnect(this);
    m_pDownloadProcess->deleteLater();
    m_pDownloadProcess = nullptr;

    const DownloadJob job = m_currentJob;
    if (exitStatus == QProcess::NormalExit && exitCode == 0 &&
            !m_currentFile.isEmpty() && QFileInfo::exists(m_currentFile)) {
        onDownloadedFile(job, m_currentFile);
    } else {
        const QString reason = m_currentError.isEmpty()
                ? tr("yt-dlp exited with code %1").arg(exitCode)
                : m_currentError;
        kLogger.warning() << "Download failed:" << job.url << reason;
        setRowStatus(job.url, tr("Failed"));
        setStatus(tr("Download of \"%1\" failed: %2").arg(job.title, reason));
    }
    startNextDownload();
}

void DlgOnlineSearch::onDownloadedFile(const DownloadJob& job, const QString& filePath) {
    TrackPointer pTrack = m_pLibrary->trackCollectionManager()->getOrAddTrack(
            TrackRef::fromFilePath(filePath));
    if (!pTrack) {
        setRowStatus(job.url, tr("Saved (not added)"));
        setStatus(tr("Saved %1, but Mixxx could not add it to the library.")
                          .arg(QDir::toNativeSeparators(filePath)));
        return;
    }
    setRowStatus(job.url, tr("In library"));
    setStatus(tr("Added \"%1\" to the library.").arg(QFileInfo(filePath).completeBaseName()));
    if (!job.group.isEmpty()) {
        emit loadTrackToPlayer(pTrack, job.group, false);
    }
}
