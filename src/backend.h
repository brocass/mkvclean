#pragma once

#include <QDateTime>
#include <QObject>
#include <QProcess>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include "mkv.h"

class AppSettings;
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
    Q_PROPERTY(bool scanning READ scanning NOTIFY stateChanged)
    Q_PROPERTY(bool scanFoundNothing READ scanFoundNothing NOTIFY stateChanged)
    Q_PROPERTY(int scanIndex READ scanIndex NOTIFY progressChanged)
    Q_PROPERTY(int scanTotal READ scanTotal NOTIFY stateChanged)
    Q_PROPERTY(bool ffmpegAvailable READ ffmpegAvailable CONSTANT)
    Q_PROPERTY(bool removeBars READ removeBars NOTIFY stateChanged)
    Q_PROPERTY(int barsDetectedCount READ barsDetectedCount NOTIFY stateChanged)
    Q_PROPERTY(int barsSelectedCount READ barsSelectedCount NOTIFY stateChanged)
    Q_PROPERTY(bool watchServiceActive READ watchServiceActive NOTIFY serviceStatusChanged)

public:
    // The picker may be null (headless CLI); tool paths and the inspect
    // timeout are injectable for tests.
    explicit Backend(FilePicker *picker, QObject *parent = nullptr,
                     const QString &mkvmerge = {}, const QString &mkvextract = {},
                     int inspectTimeoutMs = 30000, const QString &ffmpeg = {});

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
    bool scanning() const { return m_scanning; }
    bool scanFoundNothing() const { return m_scanFoundNothing; }
    int scanIndex() const { return m_scanDone; }
    int scanTotal() const { return m_scanList.size(); }
    bool ffmpegAvailable() const;
    bool removeBars() const;
    int barsDetectedCount() const;
    int barsSelectedCount() const;
    bool cropped() const;
    bool watchServiceActive() const { return m_serviceActive; }

    MkvPlan plan() const { return m_plan; }
    bool toolsAvailable() const;
    void setSettings(AppSettings *settings) { m_settings = settings; }
    // GUI mode: a completed scan pauses at a review list before processing.
    Q_INVOKABLE void setReviewAfterScan(bool review) { m_reviewAfterScan = review; }

    Q_INVOKABLE void openFiles();
    Q_INVOKABLE void openFolder();
    Q_INVOKABLE void openPath(const QString &path);
    Q_INVOKABLE void openPaths(const QStringList &paths);
    Q_INVOKABLE void scanFolders(const QStringList &folders);
    Q_INVOKABLE void scanBlackBarFolders();
    Q_INVOKABLE void startBatch();
    Q_INVOKABLE void removeFromQueue(int index);
    Q_INVOKABLE void pickWatchFolder();
    Q_INVOKABLE void pickExcludeFolder();
    Q_INVOKABLE void pickBlackBarWatchFolder();
    Q_INVOKABLE void pickBlackBarExcludeFolder();
    Q_INVOKABLE void setWatchServiceEnabled(bool enabled);
    Q_INVOKABLE void refreshServiceStatus();
    Q_INVOKABLE void setRemoveBars(bool enabled);
    Q_INVOKABLE void setQueueRemoveBars(int index, bool enabled);
    Q_INVOKABLE void setAllRemoveBars(bool enabled);
    // Headless callers decide once, instead of persisting a preference.
    void setCropOverride(bool enabled) { m_cropOverride = enabled ? 1 : 0; }
    Q_INVOKABLE void clean();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void reset();
    Q_INVOKABLE void openOutputFolder();

signals:
    void stateChanged();
    void progressChanged();
    void serviceStatusChanged();

private:
    enum class Stage { Idle, Inspecting, Ready, Review, Extracting, Cropping, Encoding, Remuxing, Done, Error };

    bool busy() const;
    void setStage(Stage stage);
    void initQueue(const QStringList &files, const QHash<QString, QVariantList> &tracksByPath = {},
                   const QHash<QString, MkvCrop> &cropsByPath = {},
                   const QHash<QString, bool> &needsWorkByPath = {});
    void inspect(const QString &path);
    void advance();
    void removeQueueRow(int index);
    void recordQueueError(const QString &message);
    int countStates(const char *state) const;
    bool cropSelected(int index) const;
    bool removeBarsDefault() const;
    void startExtraction();
    void startBars();
    void startRemux();
    void startVideoEncode();
    void prepareTempOutput();
    void finishOutput(bool cropped);
    bool convertSdhSubtitle(MkvExtractJob &job);
    void pumpScanPool();
    void probeFinished(QProcess *process);
    void retireScanProbe(QProcess *process);
    void finishScan();
    void scanFolderList(const QStringList &folders, const QStringList &excluded);
    QString subtitleLanguage() const;
    QString audioLanguage() const;
    void failWith(const QString &message);
    void processFinished(int code, QProcess::ExitStatus status);
    void parseToolOutput();
    void readStderr();
    void clearCropState();

    Stage m_stage = Stage::Idle;
    QString m_sourcePath;
    MkvPlan m_plan;
    QString m_errorText;
    QString m_toolError;       // "#GUI#error …" line from mkvmerge, when there is one
    QString m_outputPath;      // final destination — the original's own path
    QString m_partialPath;     // m_outputPath + ".remux" (only once complete)
    QString m_remuxTempPath;   // private in-progress file; never "*.remux"
    MkvExtractJobs m_extractJobs;
    QStringList m_extractedFiles;
    int m_progress = 0;
    bool m_cancelling = false;

    // Black-bar removal for the current file. m_cropKnown distinguishes "the
    // probe says there are none" from "not looked yet".
    MkvCrop m_crop;
    bool m_cropKnown = false;
    qint64 m_durationMs = 0;
    int m_sourceWidth = 0;
    int m_sourceHeight = 0;
    QByteArray m_stderrTail;   // last of ffmpeg's/mkvmerge's stderr, for errors
    QByteArray m_stdoutBuffer; // partial line held between progress reads

    // The batch queue; a single file is a queue of one that pauses at Ready.
    QStringList m_queuePaths;
    QStringList m_queueStates;        // "pending" | "active" | "done" | "error"
    QStringList m_queueErrors;
    QList<QStringList> m_queueExtracted;
    QList<int> m_queueKeptAudioIds;
    QList<QVariantList> m_queueTracks;  // per-file track plan, when known (scans)
    QList<MkvCrop> m_queueCrops;
    QList<bool> m_queueCropKnown;
    QList<bool> m_queueRemoveBars;  // the per-row tick in the review list
    QList<bool> m_queueNeedsWork;   // stripped tracks or extractable subtitles
    QList<bool> m_queueCropped;
    int m_queueIndex = -1;
    bool m_batchAborted = false;
    int m_cropOverride = -1;  // CLI: -1 follows the setting, 0 off, 1 on

    // Library scan ("pick up what was missed"): probe every MKV first, then
    // batch only the ones that still need cleaning.
    bool m_scanning = false;
    bool m_scanFoundNothing = false;
    bool m_autoCleanOnce = false;
    QStringList m_scanList;
    QStringList m_scanAccepted;
    int m_scanIndex = -1;
    int m_scanDone = 0;

    // Scan probes run a few at a time: the library is big and the probes are
    // I/O-bound. A hung one (corrupt file, stalling disk) is killed after
    // m_inspectTimeoutMs and skipped.
    QList<QProcess *> m_scanPool;
    QHash<QProcess *, QString> m_scanProbeFile;
    QHash<QProcess *, QDateTime> m_scanProbeStarted;
    QSet<QProcess *> m_scanCropping;  // the probe is done; this one is cropdetect
    QHash<QString, QVariantList> m_scanTrackLists;
    QHash<QString, MkvCrop> m_scanCrops;
    QHash<QString, bool> m_scanNeedsWork;
    QHash<QString, int> m_scanSourceWidth;
    QHash<QString, int> m_scanSourceHeight;
    QTimer m_scanTick;
    QTimer m_inspectTimeout;
    int m_inspectTimeoutMs;
    int m_cropTimeoutMs = 600000;  // keyframe crop probes are much slower

    // What the open folder picker's result is for: a cleaning job, a new
    // watched folder, or a new excluded folder.
    enum class PickPurpose { Clean, WatchFolder, ExcludeFolder, BlackBarWatchFolder, BlackBarExcludeFolder };
    PickPurpose m_pickPurpose = PickPurpose::Clean;

    QProcess m_serviceProcess;
    bool m_serviceActive = false;
    bool m_reviewAfterScan = false;

    AppSettings *m_settings = nullptr;
    QString m_mkvmerge, m_mkvextract, m_ffmpeg;
    QProcess m_process;
    FilePicker *m_picker;
};
