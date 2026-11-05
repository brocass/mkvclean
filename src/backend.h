#pragma once

#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QVariantList>

#include "mkv.h"

class FilePicker;

// Drives the app's one job: inspect MKVs, extract their English subtitles,
// remux them down to the video plus the default English audio, replacing the
// originals in place. One file stops at a review step; several files or a
// folder run as a batch straight through. The heavy work is shelled out to
// MKVToolNix through QProcess; originals are only replaced after a good remux.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString sourcePath READ sourcePath NOTIFY stateChanged)
    Q_PROPERTY(QString sourceName READ sourceName NOTIFY stateChanged)
    Q_PROPERTY(QVariantList tracks READ tracks NOTIFY stateChanged)
    Q_PROPERTY(int extractCount READ extractCount NOTIFY stateChanged)
    Q_PROPERTY(int removedCount READ removedCount NOTIFY stateChanged)
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(QString outputPath READ outputPath NOTIFY stateChanged)
    Q_PROPERTY(QStringList extractedFiles READ extractedFiles NOTIFY stateChanged)
    Q_PROPERTY(QVariantList queue READ queue NOTIFY stateChanged)
    Q_PROPERTY(int queueIndex READ queueIndex NOTIFY stateChanged)
    Q_PROPERTY(int queueCount READ queueCount NOTIFY stateChanged)
    Q_PROPERTY(int queueCleanedCount READ queueCleanedCount NOTIFY stateChanged)
    Q_PROPERTY(int queueFailedCount READ queueFailedCount NOTIFY stateChanged)
    Q_PROPERTY(bool batch READ batch NOTIFY stateChanged)
    Q_PROPERTY(bool batchAborted READ batchAborted NOTIFY stateChanged)

public:
    // The picker may be null (headless CLI); tool paths are injectable for tests.
    explicit Backend(FilePicker *picker, QObject *parent = nullptr,
                     const QString &mkvmerge = {}, const QString &mkvextract = {});

    QString state() const;
    QString sourcePath() const { return m_sourcePath; }
    QString sourceName() const;
    QVariantList tracks() const;
    int extractCount() const { return m_plan.extractSubtitleIds.size(); }
    int removedCount() const { return m_plan.removedCount(); }
    int progress() const { return m_progress; }
    QString statusText() const;
    QString errorText() const { return m_errorText; }
    QString outputPath() const { return m_outputPath; }
    QStringList extractedFiles() const { return m_extractedFiles; }

    QVariantList queue() const;
    int queueIndex() const { return m_queueIndex; }
    int queueCount() const { return m_queuePaths.size(); }
    int queueCleanedCount() const { return countStates("done"); }
    int queueFailedCount() const { return countStates("error"); }
    bool batch() const { return m_queuePaths.size() > 1; }
    bool batchAborted() const { return m_batchAborted; }

    MkvPlan plan() const { return m_plan; }
    bool toolsAvailable() const;

    Q_INVOKABLE void openFiles();
    Q_INVOKABLE void openFolder();
    Q_INVOKABLE void openPath(const QString &path);
    Q_INVOKABLE void openPaths(const QStringList &paths);
    Q_INVOKABLE void clean();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void reset();
    Q_INVOKABLE void openOutputFolder();

signals:
    void stateChanged();
    void progressChanged();

private:
    enum class Stage { Idle, Inspecting, Ready, Extracting, Remuxing, Done, Error };

    bool busy() const;
    void setStage(Stage stage);
    void inspect(const QString &path);
    void advance();
    void recordQueueError(const QString &message);
    int countStates(const char *state) const;
    void startExtraction();
    void startRemux();
    void failWith(const QString &message);
    void processFinished(int code, QProcess::ExitStatus status);
    void parseRemuxOutput();

    Stage m_stage = Stage::Idle;
    QString m_sourcePath;
    MkvPlan m_plan;
    QString m_errorText;
    QString m_toolError;       // "#GUI#error …" line from mkvmerge, when there is one
    QString m_outputPath;      // final destination — the original's own path
    QString m_partialPath;     // m_outputPath + ".part" while remuxing
    MkvExtractJobs m_extractJobs;
    QStringList m_extractedFiles;
    int m_progress = 0;
    bool m_cancelling = false;

    // The batch queue; a single file is a queue of one that pauses at Ready.
    QStringList m_queuePaths;
    QStringList m_queueStates;        // "pending" | "active" | "done" | "error"
    QStringList m_queueErrors;
    QList<QStringList> m_queueExtracted;
    QList<int> m_queueKeptAudioIds;
    int m_queueIndex = -1;
    bool m_batchAborted = false;

    QString m_mkvmerge, m_mkvextract;
    QProcess m_process;
    FilePicker *m_picker;
};
