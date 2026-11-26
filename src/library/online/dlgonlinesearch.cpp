#include "library/online/dlgonlinesearch.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QSpinBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "library/library.h"
#include "library/online/onlinebpmdetector.h"
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
const ConfigKey kDetectBpmKey(kConfigGroup, QStringLiteral("DetectBpm"));

const QString kSourceYouTube = QStringLiteral("ytsearch");
const QString kSourceYouTubeMusic = QStringLiteral("ytmusic");
const QString kSourceSoundCloud = QStringLiteral("scsearch");

constexpr int kMaxResults = 25;
constexpr int kUrlRole = Qt::UserRole;
// Numeric value used for sorting (BPM, duration) and for the BPM filter.
constexpr int kSortRole = Qt::UserRole + 1;
// BPM states stored in kSortRole of the BPM column.
constexpr double kBpmPending = -2.0;
constexpr double kBpmUnknown = -1.0;

/// Table item that sorts by its numeric kSortRole value.
class NumericItem : public QTableWidgetItem {
  public:
    NumericItem(const QString& text, double value)
            : QTableWidgetItem(text) {
        setData(kSortRole, value);
        setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
    bool operator<(const QTableWidgetItem& other) const override {
        return data(kSortRole).toDouble() < other.data(kSortRole).toDouble();
    }
};

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
    // winget installs usually extend the *saved* PATH, which a process that
    // was started earlier (e.g. from an old shell) doesn't see yet.
    for (const QString& key : {QStringLiteral("HKEY_CURRENT_USER\\Environment"),
                 QStringLiteral("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\"
                                "Control\\Session Manager\\Environment")}) {
        const QSettings env(key, QSettings::NativeFormat);
        const QString path = env.value(QStringLiteral("Path")).toString();
        for (QString dir : path.split(';', Qt::SkipEmptyParts)) {
            dir.replace(QStringLiteral("%LOCALAPPDATA%"), localAppData, Qt::CaseInsensitive);
            dir.replace(QStringLiteral("%USERPROFILE%"), QDir::homePath(), Qt::CaseInsensitive);
            extraDirs << dir;
        }
    }
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
    m_pSourceCombo->addItem(tr("YouTube"), kSourceYouTube);
    m_pSourceCombo->addItem(tr("YouTube Music"), kSourceYouTubeMusic);
    m_pSourceCombo->addItem(tr("SoundCloud"), kSourceSoundCloud);
    {
        // Stored by name; older versions stored the index (0 YouTube, 1 SoundCloud).
        QString saved = m_pConfig->getValueString(kSourceKey);
        if (saved == QStringLiteral("1")) {
            saved = kSourceSoundCloud;
        }
        const int index = m_pSourceCombo->findData(saved);
        m_pSourceCombo->setCurrentIndex(index >= 0 ? index : 0);
    }
    connect(m_pSourceCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [this](int index) {
                m_pConfig->setValue(kSourceKey, m_pSourceCombo->itemData(index).toString());
            });

    m_pQueryEdit = new QLineEdit(this);
    m_pQueryEdit->setPlaceholderText(
            tr("Search YouTube / SoundCloud, or paste a link, then press Enter"));
    m_pQueryEdit->setClearButtonEnabled(true);
    connect(m_pQueryEdit, &QLineEdit::returnPressed, this, &DlgOnlineSearch::slotSearch);

    m_pSearchButton = new QPushButton(tr("Search"), this);
    // Skins can style the main actions via QPushButton[primary="true"].
    m_pSearchButton->setProperty("primary", true);
    connect(m_pSearchButton, &QPushButton::clicked, this, &DlgOnlineSearch::slotSearch);

    auto* pSearchRow = new QHBoxLayout();
    pSearchRow->addWidget(m_pSourceCombo);
    pSearchRow->addWidget(m_pQueryEdit, 1);
    pSearchRow->addWidget(m_pSearchButton);

    // --- BPM ---
    m_pBpmDetector = new OnlineBpmDetector(this);
    connect(m_pBpmDetector,
            &OnlineBpmDetector::bpmDetected,
            this,
            &DlgOnlineSearch::slotBpmDetected);
    m_pDetectBpm = new QCheckBox(tr("Detect BPM"), this);
    m_pDetectBpm->setToolTip(
            tr("Estimate the BPM of each result from a short excerpt.\n"
               "Takes a few seconds per result; Mixxx analyzes the exact BPM\n"
               "once a track is downloaded."));
    m_pDetectBpm->setChecked(m_pConfig->getValue(kDetectBpmKey, true));
    connect(m_pDetectBpm, &QCheckBox::toggled, this, [this](bool checked) {
        m_pConfig->setValue(kDetectBpmKey, checked);
    });
    auto makeBpmSpin = [this]() {
        auto* pSpin = new QSpinBox(this);
        pSpin->setRange(0, 250);
        pSpin->setSpecialValueText(tr("any"));
        pSpin->setKeyboardTracking(false);
        connect(pSpin,
                QOverload<int>::of(&QSpinBox::valueChanged),
                this,
                &DlgOnlineSearch::applyBpmFilter);
        return pSpin;
    };
    m_pBpmMin = makeBpmSpin();
    m_pBpmMax = makeBpmSpin();
    auto* pBpmRow = new QHBoxLayout();
    pBpmRow->addWidget(m_pDetectBpm);
    pBpmRow->addSpacing(16);
    pBpmRow->addWidget(new QLabel(tr("BPM from"), this));
    pBpmRow->addWidget(m_pBpmMin);
    pBpmRow->addWidget(new QLabel(tr("to"), this));
    pBpmRow->addWidget(m_pBpmMax);
    pBpmRow->addStretch(1);

    // --- results ---
    m_pResults = new QTableWidget(0, kColCount, this);
    m_pResults->setHorizontalHeaderLabels(
            {tr("Title"), tr("Uploader"), tr("BPM"), tr("Duration"), tr("Status")});
    m_pResults->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pResults->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_pResults->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pResults->verticalHeader()->setVisible(false);
    m_pResults->setAlternatingRowColors(true);
    m_pResults->setWordWrap(false);
    m_pResults->setShowGrid(false);
    m_pResults->verticalHeader()->setDefaultSectionSize(28);
    m_pResults->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_pResults->horizontalHeader()->setHighlightSections(false);
    QHeaderView* pHeader = m_pResults->horizontalHeader();
    pHeader->setSectionResizeMode(kColTitle, QHeaderView::Stretch);
    pHeader->setSectionResizeMode(kColUploader, QHeaderView::Interactive);
    pHeader->setSectionResizeMode(kColBpm, QHeaderView::ResizeToContents);
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
    m_pDownloadButton->setProperty("primary", true);
    m_pDeck1Button = new QPushButton(tr("Download to Deck 1"), this);
    m_pDeck2Button = new QPushButton(tr("Download to Deck 2"), this);
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
    m_pStatusLabel->setObjectName(QStringLiteral("OnlineSearchStatus"));
    m_pStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* pActionRow = new QHBoxLayout();
    pActionRow->addWidget(m_pDownloadButton);
    pActionRow->addWidget(m_pDeck1Button);
    pActionRow->addWidget(m_pDeck2Button);
    pActionRow->addStretch(1);
    pActionRow->addWidget(m_pFolderButton);

    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(6, 6, 6, 8);
    pLayout->addLayout(pSearchRow);
    pLayout->addLayout(pBpmRow);
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

    m_pBpmDetector->clear();
    m_pBpmDetector->setTools(ytDlp, ffmpegPath(), processEnvironment());
    m_pendingBpm.clear();
    m_pResults->setSortingEnabled(false);
    m_pResults->setRowCount(0);
    m_lastQuery = query;
    m_fallbackTried = false;

    // A pasted link is resolved directly; otherwise use the search prefix.
    const bool isUrl = query.startsWith(QStringLiteral("http://")) ||
            query.startsWith(QStringLiteral("https://"));
    const QString source = m_pSourceCombo->currentData().toString();
    m_searchSource = isUrl ? QString() : source;
    if (isUrl) {
        startSearchProcess(query, tr("Resolving link..."));
    } else {
        startSearchProcess(searchTarget(source, query),
                tr("Searching %1 for \"%2\"...").arg(m_pSourceCombo->currentText(), query));
    }
}

QString DlgOnlineSearch::searchTarget(const QString& source, const QString& query) {
    if (source == kSourceYouTubeMusic) {
        // The "songs" tab gives official audio, without music-video intros.
        return QStringLiteral("https://music.youtube.com/search?q=") +
                QString::fromLatin1(QUrl::toPercentEncoding(query)) +
                QStringLiteral("#songs");
    }
    return QStringLiteral("%1%2:%3").arg(source).arg(kMaxResults).arg(query);
}

void DlgOnlineSearch::startSearchProcess(const QString& target, const QString& statusText) {
    const QString ytDlp = ytDlpPath();
    if (m_pSearchProcess) {
        m_pSearchProcess->disconnect(this);
        m_pSearchProcess->kill();
        m_pSearchProcess->deleteLater();
    }
    m_searchBuffer.clear();
    m_searchError.clear();

    QStringList args;
    args << QStringLiteral("--flat-playlist")
         << QStringLiteral("--dump-json")
         << QStringLiteral("--no-warnings")
         << QStringLiteral("--ignore-errors")
         << QStringLiteral("--playlist-end") << QString::number(kMaxResults)
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

    setStatus(statusText);
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
    const QString host = QUrl(url).host();
    if (uploader.isEmpty() && host.contains(QStringLiteral("soundcloud"))) {
        // SoundCloud URLs are ".../<artist>/<track>".
        uploader = QUrl(url).path().section('/', 1, 1);
    } else if (uploader.isEmpty() && host.startsWith(QStringLiteral("music."))) {
        uploader = tr("YouTube Music");
    }
    const double durationSeconds = obj.value(QStringLiteral("duration")).toDouble();
    const QString duration = formatDuration(durationSeconds);

    const int row = m_pResults->rowCount();
    m_pResults->insertRow(row);
    auto* pTitle = new QTableWidgetItem(title);
    pTitle->setData(kUrlRole, url);
    pTitle->setToolTip(url);
    m_pResults->setItem(row, kColTitle, pTitle);
    m_pResults->setItem(row, kColUploader, new QTableWidgetItem(uploader));
    m_pResults->setItem(row, kColDuration, new NumericItem(duration, durationSeconds));
    m_pResults->setItem(row, kColStatus, new QTableWidgetItem());

    // BPM: from the title ("... 128 BPM") if present, otherwise estimated
    // from a short excerpt in the background.
    const double titleBpm = OnlineBpmDetector::bpmFromText(title);
    if (titleBpm > 0) {
        m_pResults->setItem(row, kColBpm, new NumericItem(QString::number(titleBpm), titleBpm));
    } else if (m_pDetectBpm->isChecked()) {
        m_pResults->setItem(row, kColBpm, new NumericItem(QStringLiteral("..."), kBpmPending));
        // Started once the search is done, so it doesn't compete with it.
        m_pendingBpm.append(qMakePair(url, durationSeconds));
    } else {
        m_pResults->setItem(row, kColBpm, new NumericItem(QString(), kBpmUnknown));
    }
    m_pResults->setRowHidden(row, !rowMatchesBpmFilter(row));
}

void DlgOnlineSearch::slotBpmDetected(const QString& url, double bpm) {
    for (int row = 0; row < m_pResults->rowCount(); ++row) {
        QTableWidgetItem* pTitle = m_pResults->item(row, kColTitle);
        if (!pTitle || pTitle->data(kUrlRole).toString() != url) {
            continue;
        }
        QTableWidgetItem* pBpm = m_pResults->item(row, kColBpm);
        if (!pBpm) {
            continue;
        }
        if (bpm > 0) {
            pBpm->setText(QString::number(bpm, 'f', 1));
            pBpm->setData(kSortRole, bpm);
            pBpm->setToolTip(tr("Estimated from a 30 second excerpt. Mixxx "
                                "analyzes the exact BPM after download."));
        } else {
            pBpm->setText(bpm < 0 ? tr("mix") : QStringLiteral("?"));
            pBpm->setData(kSortRole, kBpmUnknown);
            pBpm->setToolTip(bpm < 0 ? tr("Too long to estimate (probably a DJ mix).")
                                     : tr("Could not estimate the BPM."));
        }
        m_pResults->setRowHidden(row, !rowMatchesBpmFilter(row));
    }
}

bool DlgOnlineSearch::rowMatchesBpmFilter(int row) const {
    const int minBpm = m_pBpmMin->value();
    const int maxBpm = m_pBpmMax->value();
    if (minBpm <= 0 && maxBpm <= 0) {
        return true;
    }
    const QTableWidgetItem* pBpm = m_pResults->item(row, kColBpm);
    const double bpm = pBpm ? pBpm->data(kSortRole).toDouble() : kBpmUnknown;
    if (bpm == kBpmPending) {
        return true; // still estimating; decide when it's known
    }
    if (bpm <= 0) {
        return false;
    }
    // Accept half / double time too (e.g. 64 or 256 for a 128 filter).
    for (double candidate : {bpm, bpm * 2.0, bpm / 2.0}) {
        if ((minBpm <= 0 || candidate >= minBpm - 0.5) &&
                (maxBpm <= 0 || candidate <= maxBpm + 0.5)) {
            return true;
        }
    }
    return false;
}

void DlgOnlineSearch::applyBpmFilter() {
    int visible = 0;
    for (int row = 0; row < m_pResults->rowCount(); ++row) {
        const bool show = rowMatchesBpmFilter(row);
        m_pResults->setRowHidden(row, !show);
        visible += show ? 1 : 0;
    }
    if (m_pBpmMin->value() > 0 || m_pBpmMax->value() > 0) {
        setStatus(tr("%1 of %2 results match the BPM range.")
                          .arg(visible)
                          .arg(m_pResults->rowCount()));
    }
}

void DlgOnlineSearch::slotSearchFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    Q_UNUSED(exitCode);
    slotSearchOutput(); // flush remaining output
    const int count = m_pResults->rowCount();

    // YouTube's regular search sometimes returns results in a format yt-dlp
    // can't read and reports 0 hits without an error. YouTube Music usually
    // finds the track, so retry there automatically.
    if (exitStatus == QProcess::NormalExit && count == 0 && !m_fallbackTried &&
            m_searchSource == kSourceYouTube) {
        m_fallbackTried = true;
        if (m_pSearchProcess) {
            m_pSearchProcess->disconnect(this);
            m_pSearchProcess->deleteLater();
            m_pSearchProcess = nullptr;
        }
        startSearchProcess(searchTarget(kSourceYouTubeMusic, m_lastQuery),
                tr("No YouTube results for \"%1\", trying YouTube Music...")
                        .arg(m_lastQuery));
        return;
    }

    m_pSearchButton->setEnabled(true);
    m_pResults->setSortingEnabled(true);
    for (const auto& pending : std::as_const(m_pendingBpm)) {
        m_pBpmDetector->enqueue(pending.first, pending.second);
    }
    m_pendingBpm.clear();
    if (exitStatus != QProcess::NormalExit) {
        setStatus(tr("Search aborted."));
    } else if (count == 0) {
        setStatus(m_searchError.isEmpty()
                        ? tr("No results. Try different words, or another source.")
                        : m_searchError);
    } else {
        setStatus((m_fallbackTried ? tr("%1 results from YouTube Music. ")
                                   : tr("%1 results. "))
                          .arg(count) +
                tr("Double-click or select and press a download button."));
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
    m_pathFile = QDir::temp().filePath(QStringLiteral("mixxx-online-%1.txt")
                                               .arg(QCoreApplication::applicationPid()));
    QFile::remove(m_pathFile);
    m_jobStarted = QDateTime::currentDateTime();

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
         // The final path goes to a file, not stdout: on Windows yt-dlp's
         // console output drops characters like the Unicode slash it substitutes for
         // "/" in titles, so a printed path may not match the real file.
         << QStringLiteral("--print-to-file")
         << QStringLiteral("after_move:%(filepath)s") << m_pathFile;
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
    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
        m_currentFile = downloadedFilePath();
    }
    if (exitStatus == QProcess::NormalExit && exitCode == 0 &&
            !m_currentFile.isEmpty() && QFileInfo::exists(m_currentFile)) {
        onDownloadedFile(job, m_currentFile);
    } else {
        QString reason = m_currentError.isEmpty()
                ? tr("yt-dlp exited with code %1").arg(exitCode)
                : m_currentError;
        if (reason.contains(QStringLiteral("DRM"), Qt::CaseInsensitive)) {
            reason = tr("this upload is DRM-protected (common for major-label "
                        "releases on SoundCloud) and can't be downloaded. Try "
                        "another result or the YouTube version.");
        }
        kLogger.warning() << "Download failed:" << job.url << reason;
        setRowStatus(job.url, tr("Failed"));
        setStatus(tr("Download of \"%1\" failed: %2").arg(job.title, reason));
    }
    startNextDownload();
}

QString DlgOnlineSearch::downloadedFilePath() const {
    // 1st choice: the exact path yt-dlp wrote with --print-to-file (UTF-8).
    QFile file(m_pathFile);
    if (file.open(QIODevice::ReadOnly)) {
        const QStringList lines = QString::fromUtf8(file.readAll())
                                          .split('\n', Qt::SkipEmptyParts);
        file.close();
        QFile::remove(m_pathFile);
        if (!lines.isEmpty()) {
            const QString path = lines.last().trimmed();
            if (QFileInfo::exists(path)) {
                return path;
            }
        }
    }
    // Fallback: the newest MP3 in the download folder written by this job.
    const QFileInfoList candidates = QDir(downloadDir()).entryInfoList(
            {QStringLiteral("*.mp3")}, QDir::Files, QDir::Time);
    if (!candidates.isEmpty() &&
            candidates.first().lastModified() >= m_jobStarted.addSecs(-2)) {
        return candidates.first().absoluteFilePath();
    }
    return QString();
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
