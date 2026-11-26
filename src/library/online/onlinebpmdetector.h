#pragma once

#include <QList>
#include <QObject>
#include <QProcessEnvironment>
#include <QString>
#include <QTemporaryDir>
#include <memory>

class QProcess;

/// Estimates the BPM of online (YouTube / SoundCloud) results before they are
/// downloaded: fetches a ~30 s excerpt with yt-dlp, decodes it with ffmpeg and
/// runs SoundTouch's beat detector on it. Runs a couple of jobs in parallel.
class OnlineBpmDetector : public QObject {
    Q_OBJECT
  public:
    explicit OnlineBpmDetector(QObject* pParent = nullptr);
    ~OnlineBpmDetector() override;

    void setTools(const QString& ytDlpPath,
            const QString& ffmpegPath,
            const QProcessEnvironment& environment);
    /// durationSeconds may be 0 if unknown.
    void enqueue(const QString& url, double durationSeconds);
    /// Cancels everything that is queued or running.
    void clear();

    /// Parses "128 BPM" / "128bpm" style hints from a title. Returns 0 if none.
    static double bpmFromText(const QString& text);

  signals:
    /// bpm == 0: detection failed; bpm < 0: skipped (e.g. a long DJ mix).
    void bpmDetected(const QString& url, double bpm);

  private:
    struct Job {
        QString url;
        double duration = 0;
        int id = 0;
        QProcess* pProcess = nullptr;
        bool decoding = false;
    };

    void startJobs();
    void startDownload(Job* pJob);
    void startDecode(Job* pJob, const QString& filePath, double startSeconds);
    void finishJob(Job* pJob, double bpm);
    static double detectBpm(const QByteArray& monoFloatSamples, int sampleRate);

    QString m_ytDlp;
    QString m_ffmpeg;
    QProcessEnvironment m_environment;
    std::unique_ptr<QTemporaryDir> m_pTempDir;
    QList<Job> m_queue;
    QList<Job*> m_running;
    int m_nextId;
};
