#include "library/online/onlinebpmdetector.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <cmath>
#include <soundtouch/BPMDetect.h>

#include "moc_onlinebpmdetector.cpp"
#include "util/logger.h"

namespace {

const mixxx::Logger kLogger("OnlineBpmDetector");

constexpr int kMaxParallelJobs = 3;
constexpr int kSampleRate = 44100;
constexpr double kExcerptSeconds = 30.0;
constexpr double kMinBpm = 70.0;
constexpr double kMaxBpm = 180.0;
constexpr double kMaxTrackSeconds = 12 * 60;

} // anonymous namespace

OnlineBpmDetector::OnlineBpmDetector(QObject* pParent)
        : QObject(pParent),
          m_nextId(0) {
}

OnlineBpmDetector::~OnlineBpmDetector() {
    clear();
}

void OnlineBpmDetector::setTools(const QString& ytDlpPath,
        const QString& ffmpegPath,
        const QProcessEnvironment& environment) {
    m_ytDlp = ytDlpPath;
    m_ffmpeg = ffmpegPath;
    m_environment = environment;
}

double OnlineBpmDetector::bpmFromText(const QString& text) {
    static const QRegularExpression re(
            QStringLiteral("(\\d{2,3}(?:[.,]\\d+)?)\\s*-?\\s*bpm"),
            QRegularExpression::CaseInsensitiveOption);
    const auto match = re.match(text);
    if (!match.hasMatch()) {
        return 0.0;
    }
    QString number = match.captured(1);
    number.replace(',', '.');
    const double bpm = number.toDouble();
    return (bpm >= 40 && bpm <= 250) ? bpm : 0.0;
}

void OnlineBpmDetector::enqueue(const QString& url, double durationSeconds) {
    if (url.isEmpty()) {
        return;
    }
    Job job;
    job.url = url;
    job.duration = durationSeconds;
    job.id = m_nextId++;
    m_queue.append(job);
    startJobs();
}

void OnlineBpmDetector::clear() {
    m_queue.clear();
    const QList<Job*> running = m_running;
    m_running.clear();
    for (Job* pJob : running) {
        if (pJob->pProcess) {
            pJob->pProcess->disconnect(this);
            pJob->pProcess->kill();
            pJob->pProcess->waitForFinished(1000);
            pJob->pProcess->deleteLater();
        }
        delete pJob;
    }
    // Drop the excerpts of cancelled jobs.
    m_pTempDir.reset();
}

void OnlineBpmDetector::startJobs() {
    if (m_ytDlp.isEmpty() || m_ffmpeg.isEmpty()) {
        // Can't detect without the tools: report failure for everything.
        const QList<Job> queue = m_queue;
        m_queue.clear();
        for (const Job& job : queue) {
            emit bpmDetected(job.url, 0.0);
        }
        return;
    }
    if (!m_pTempDir || !m_pTempDir->isValid()) {
        m_pTempDir = std::make_unique<QTemporaryDir>(
                QDir::tempPath() + QStringLiteral("/mixxx-bpm-XXXXXX"));
    }
    while (m_running.size() < kMaxParallelJobs && !m_queue.isEmpty()) {
        Job* pJob = new Job(m_queue.takeFirst());
        m_running.append(pJob);
        startDownload(pJob);
    }
}

void OnlineBpmDetector::startDownload(Job* pJob) {
    // Downloading the (small) audio stream natively is much faster than
    // streaming a section through ffmpeg, which YouTube throttles. Long uploads
    // (DJ mixes) are skipped: one BPM wouldn't describe them anyway.
    if (pJob->duration > kMaxTrackSeconds) {
        finishJob(pJob, -1.0);
        return;
    }
    const QString outputTemplate = QDir(m_pTempDir->path())
                                           .filePath(QStringLiteral("%1.%(ext)s").arg(pJob->id));
    QStringList args;
    args << QStringLiteral("--no-playlist")
         << QStringLiteral("--no-warnings")
         << QStringLiteral("--quiet")
         << QStringLiteral("-f") << QStringLiteral("bestaudio/best")
         << QStringLiteral("--max-filesize") << QStringLiteral("30M")
         << QStringLiteral("--ffmpeg-location") << m_ffmpeg
         << QStringLiteral("-o") << outputTemplate
         << QStringLiteral("--print") << QStringLiteral("after_move:filepath")
         << pJob->url;

    pJob->pProcess = new QProcess(this);
    pJob->pProcess->setProcessEnvironment(m_environment);
    connect(pJob->pProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this, pJob](int exitCode, QProcess::ExitStatus status) {
                if (!m_running.contains(pJob)) {
                    return;
                }
                const QString output = QString::fromUtf8(
                        pJob->pProcess->readAllStandardOutput())
                                               .trimmed();
                pJob->pProcess->deleteLater();
                pJob->pProcess = nullptr;
                const QString filePath = output.section('\n', -1).trimmed();
                if (status != QProcess::NormalExit || exitCode != 0 ||
                        filePath.isEmpty() || !QFile::exists(filePath)) {
                    finishJob(pJob, 0.0);
                    return;
                }
                // Analyze an excerpt from the body of the track, where the
                // beat usually is.
                double start = 30.0;
                if (pJob->duration > 0) {
                    start = qBound(0.0,
                            pJob->duration * 0.35,
                            qMax(0.0, pJob->duration - kExcerptSeconds));
                }
                startDecode(pJob, filePath, start);
            });
    connect(pJob->pProcess, &QProcess::errorOccurred, this, [this, pJob](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart && m_running.contains(pJob)) {
            pJob->pProcess->deleteLater();
            pJob->pProcess = nullptr;
            finishJob(pJob, 0.0);
        }
    });
    pJob->pProcess->start(m_ytDlp, args);
}

void OnlineBpmDetector::startDecode(Job* pJob, const QString& filePath, double startSeconds) {
    pJob->decoding = true;
    QStringList args;
    args << QStringLiteral("-v") << QStringLiteral("error")
         << QStringLiteral("-ss") << QString::number(startSeconds)
         << QStringLiteral("-i") << filePath
         << QStringLiteral("-t") << QString::number(kExcerptSeconds)
         << QStringLiteral("-ac") << QStringLiteral("1")
         << QStringLiteral("-ar") << QString::number(kSampleRate)
         << QStringLiteral("-f") << QStringLiteral("f32le")
         << QStringLiteral("-");
    pJob->pProcess = new QProcess(this);
    connect(pJob->pProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this, pJob, filePath](int exitCode, QProcess::ExitStatus status) {
                if (!m_running.contains(pJob)) {
                    return;
                }
                const QByteArray samples = pJob->pProcess->readAllStandardOutput();
                pJob->pProcess->deleteLater();
                pJob->pProcess = nullptr;
                QFile::remove(filePath);
                if (status != QProcess::NormalExit || exitCode != 0 || samples.size() < 4) {
                    finishJob(pJob, 0.0);
                    return;
                }
                finishJob(pJob, detectBpm(samples, kSampleRate));
            });
    connect(pJob->pProcess, &QProcess::errorOccurred, this, [this, pJob](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart && m_running.contains(pJob)) {
            pJob->pProcess->deleteLater();
            pJob->pProcess = nullptr;
            finishJob(pJob, 0.0);
        }
    });
    pJob->pProcess->start(m_ffmpeg, args);
}

double OnlineBpmDetector::detectBpm(const QByteArray& monoFloatSamples, int sampleRate) {
    const int count = static_cast<int>(monoFloatSamples.size() / sizeof(float));
    if (count < sampleRate * 5) {
        return 0.0; // too short to be meaningful
    }
    soundtouch::BPMDetect detector(1, sampleRate);
    const auto* pSamples = reinterpret_cast<const float*>(monoFloatSamples.constData());
    constexpr int kChunk = 4096;
    for (int offset = 0; offset < count; offset += kChunk) {
        detector.inputSamples(pSamples + offset, std::min(kChunk, count - offset));
    }
    double bpm = detector.getBpm();
    if (bpm <= 0) {
        return 0.0;
    }
    // Fold into the usual DJ range (half / double time detections).
    while (bpm < kMinBpm) {
        bpm *= 2;
    }
    while (bpm > kMaxBpm) {
        bpm /= 2;
    }
    return std::round(bpm * 10.0) / 10.0;
}

void OnlineBpmDetector::finishJob(Job* pJob, double bpm) {
    m_running.removeOne(pJob);
    const QString url = pJob->url;
    delete pJob;
    if (bpm <= 0) {
        kLogger.debug() << "BPM detection failed for" << url;
    }
    emit bpmDetected(url, bpm);
    startJobs();
}
