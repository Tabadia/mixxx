#pragma once

#include <QDateTime>
#include <QList>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QWidget>

#include "library/libraryview.h"
#include "preferences/usersettings.h"
#include "track/track_decl.h"

class Library;
class OnlineBpmDetector;
class QCheckBox;
class QSpinBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class WLibrary;

/// Library view that searches YouTube / SoundCloud with yt-dlp, downloads
/// the selected results as MP3 files and adds them to the Mixxx library.
class DlgOnlineSearch : public QWidget, public virtual LibraryView {
    Q_OBJECT
  public:
    DlgOnlineSearch(WLibrary* pParent, UserSettingsPointer pConfig, Library* pLibrary);
    ~DlgOnlineSearch() override;

    void onShow() override {
    }
    bool hasFocus() const override;
    void setFocus() override;

  public slots:
    void focusSearchBox();

  signals:
    void loadTrack(TrackPointer pTrack);
    void loadTrackToPlayer(TrackPointer pTrack, const QString& group, bool play);

  private slots:
    void slotSearch();
    void slotSearchOutput();
    void slotSearchFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void slotDownloadSelected();
    void slotDownloadToDeck1();
    void slotDownloadToDeck2();
    void slotDownloadOutput();
    void slotDownloadFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void slotChooseFolder();

  private:
    struct DownloadJob {
        QString url;
        QString title;
        QString group; // empty: only add to library
    };

    enum Column {
        kColTitle = 0,
        kColUploader,
        kColBpm,
        kColDuration,
        kColStatus,
        kColCount,
    };

    void enqueueSelected(const QString& group);
    void startNextDownload();
    void setRowStatus(const QString& url, const QString& status);
    void setStatus(const QString& text);
    void addResultFromJson(const QByteArray& line);
    void onDownloadedFile(const DownloadJob& job, const QString& filePath);
    QString downloadedFilePath() const;
    void slotBpmDetected(const QString& url, double bpm);
    void startSearchProcess(const QString& target, const QString& statusText);
    static QString searchTarget(const QString& source, const QString& query);
    void applyBpmFilter();
    bool rowMatchesBpmFilter(int row) const;

    QString ytDlpPath() const;
    QString ffmpegPath() const;
    QString downloadDir() const;
    QProcessEnvironment processEnvironment() const;

    UserSettingsPointer m_pConfig;
    Library* m_pLibrary;

    QComboBox* m_pSourceCombo;
    QLineEdit* m_pQueryEdit;
    QPushButton* m_pSearchButton;
    QTableWidget* m_pResults;
    QPushButton* m_pDownloadButton;
    QPushButton* m_pDeck1Button;
    QPushButton* m_pDeck2Button;
    QPushButton* m_pFolderButton;
    QLabel* m_pStatusLabel;
    QCheckBox* m_pDetectBpm;
    QSpinBox* m_pBpmMin;
    QSpinBox* m_pBpmMax;
    OnlineBpmDetector* m_pBpmDetector;

    QPointer<QProcess> m_pSearchProcess;
    QByteArray m_searchBuffer;
    QString m_searchError;
    QString m_lastQuery;
    QString m_searchSource; // empty for a pasted link
    bool m_fallbackTried = false;
    QList<QPair<QString, double>> m_pendingBpm; // url, duration

    QPointer<QProcess> m_pDownloadProcess;
    QByteArray m_downloadBuffer;
    QList<DownloadJob> m_downloadQueue;
    DownloadJob m_currentJob;
    QString m_currentFile;
    QString m_pathFile;
    QDateTime m_jobStarted;
    QString m_currentError;
};
