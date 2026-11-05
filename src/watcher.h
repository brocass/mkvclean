#pragma once

#include <QDateTime>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include "backend.h"

class AppSettings;

// Watches folders for new MKV files and cleans each one once it has finished
// copying (size and mtime settled). Files already in the folders at startup
// are only snapshotted, never processed: what arrives while the watcher runs
// is what gets cleaned.
class Watcher : public QObject {
    Q_OBJECT

public:
    explicit Watcher(const QStringList &folders, int settleMs, QObject *parent = nullptr,
                     const QString &mkvmerge = {}, const QString &mkvextract = {},
                     AppSettings *settings = nullptr);

    void start();
    bool toolsAvailable() const { return m_backend.toolsAvailable(); }

signals:
    void logLine(const QString &line);

private:
    struct Candidate {
        qint64 size = -1;
        qint64 mtimeMs = -1;
        QDateTime lastChanged;
    };

    void watchRoot(const QString &folder, bool snapshotOnly);
    void scanDirectory(const QString &path);
    void armCandidate(const QString &path);
    void addWatchDir(const QString &path);
    bool excluded(const QString &path) const;
    void checkCandidates();
    // Retries swaps blocked earlier (original in use): every "*.remux" file
    // under the roots is swapped into place when nothing blocks it anymore.
    void completeSwaps();
    void pump();
    void finishOne();

    QStringList m_roots;
    QStringList m_excluded;
    int m_settleMs;
    QFileSystemWatcher m_watcher;
    QSet<QString> m_watchedDirs;
    QSet<QString> m_seen;  // startup snapshot plus everything already armed
    QHash<QString, Candidate> m_candidates;
    QStringList m_pending;
    QSet<QString> m_swapWarned;
    bool m_processing = false;
    QTimer m_stabilityTimer;
    QTimer m_pollTimer;
    Backend m_backend;
};
