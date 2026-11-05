#include "watcher.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

#include "settings.h"

Watcher::Watcher(const QStringList &folders, int settleMs, QObject *parent,
                 const QString &mkvmerge, const QString &mkvextract, AppSettings *settings)
    : QObject(parent),
      m_roots(folders),
      m_excluded(settings ? settings->excludedFolders() : QStringList()),
      m_settleMs(settleMs),
      m_backend(nullptr, this, mkvmerge, mkvextract) {
    m_backend.setSettings(settings);
    m_stabilityTimer.setInterval(1000);
    connect(&m_stabilityTimer, &QTimer::timeout, this, &Watcher::checkCandidates);
    // Events can be missed on fused/network mounts; a slow poll makes the
    // watcher robust regardless.
    m_pollTimer.setInterval(60000);

    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this,
            [this](const QString &path) { scanDirectory(path); });
    connect(&m_pollTimer, &QTimer::timeout, this, [this] {
        completeSwaps();
        for (const auto &root : m_roots)
            watchRoot(root, false);
    });

    connect(&m_backend, &Backend::stateChanged, this, [this] {
        const QString state = m_backend.state();
        if (state == "ready") {
            m_backend.clean();
            return;
        }
        if (state == "done") {
            emit logLine("mkvclean: cleaned " + m_backend.sourcePath());
            finishOne();
            return;
        }
        if (state == "error") {
            // A failed file (no English audio, unreadable…) is logged and
            // skipped; it is not retried until the watcher restarts.
            emit logLine("mkvclean: failed " + m_backend.sourcePath() + ": " + m_backend.errorText());
            finishOne();
        }
    });
}

void Watcher::start() {
    for (const auto &root : m_roots) {
        if (!QFileInfo(root).isDir()) {
            emit logLine("mkvclean: not a folder, skipping: " + root);
            continue;
        }
        watchRoot(root, true);
        emit logLine("mkvclean: watching " + root);
    }
    for (const auto &folder : m_excluded)
        emit logLine("mkvclean: excluded " + folder);
    completeSwaps();
    m_stabilityTimer.start();
    m_pollTimer.start();
    emit logLine("mkvclean: ready");
}

bool Watcher::excluded(const QString &path) const {
    return !Mkv::withoutExcluded({path}, m_excluded).contains(path);
}

void Watcher::addWatchDir(const QString &path) {
    if (m_watchedDirs.contains(path) || excluded(path))
        return;
    if (m_watcher.addPath(path))
        m_watchedDirs.insert(path);
}

void Watcher::watchRoot(const QString &folder, bool snapshotOnly) {
    addWatchDir(folder);
    QSet<QString> present;
    QDirIterator it(folder, {"*.mkv", "*.MKV"},
                    QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QFileInfo info = it.fileInfo();
        if (info.isDir()) {
            addWatchDir(path);
            continue;
        }
        present.insert(path);
        if (snapshotOnly)
            m_seen.insert(path);
        else
            armCandidate(path);
    }

    // A path whose file was deleted becomes "new" again if it reappears —
    // that is how quality upgrades re-use the same file name.
    const QString prefix = folder.endsWith('/') ? folder : folder + '/';
    for (auto it = m_seen.begin(); it != m_seen.end();) {
        if (it->startsWith(prefix) && !present.contains(*it))
            it = m_seen.erase(it);
        else
            ++it;
    }
}

void Watcher::scanDirectory(const QString &path) {
    const QDir dir(path);
    for (const auto &entry : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        addWatchDir(dir.absoluteFilePath(entry));

    QSet<QString> present;
    QDirIterator it(path, {"*.mkv", "*.MKV"}, QDir::Files);
    while (it.hasNext()) {
        const QString file = it.next();
        present.insert(file);
        armCandidate(file);
    }
    for (auto it = m_seen.begin(); it != m_seen.end();) {
        if (QFileInfo(*it).absolutePath() == path && !present.contains(*it))
            it = m_seen.erase(it);
        else
            ++it;
    }
}

void Watcher::armCandidate(const QString &path) {
    if (m_seen.contains(path) || m_candidates.contains(path) || excluded(path))
        return;
    const QFileInfo info(path);
    m_seen.insert(path);
    m_candidates.insert(path, {info.size(), info.lastModified().toMSecsSinceEpoch(),
                               QDateTime::currentDateTime()});
    emit logLine("mkvclean: found " + path + ", waiting for it to settle");
}

void Watcher::checkCandidates() {
    const auto now = QDateTime::currentDateTime();
    QStringList ready;
    for (auto it = m_candidates.begin(); it != m_candidates.end();) {
        const QFileInfo info(it.key());
        if (!info.exists()) {
            // Went away mid-copy; forget it entirely so a retry can re-arm.
            m_seen.remove(it.key());
            it = m_candidates.erase(it);
            continue;
        }
        const qint64 size = info.size();
        const qint64 mtime = info.lastModified().toMSecsSinceEpoch();
        if (size != it.value().size || mtime != it.value().mtimeMs) {
            it.value().size = size;
            it.value().mtimeMs = mtime;
            it.value().lastChanged = now;
        } else if (it.value().lastChanged.msecsTo(now) >= m_settleMs) {
            ready.append(it.key());
            it = m_candidates.erase(it);
            continue;
        }
        ++it;
    }
    if (!ready.isEmpty()) {
        m_pending.append(ready);
        pump();
    }
}

void Watcher::completeSwaps() {
    for (const auto &root : m_roots) {
        QDirIterator it(root, {"*.remux"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString remux = it.next();
            if (excluded(remux))
                continue;
            const QString original = remux.left(remux.size() - QStringLiteral(".remux").size());
            if (!original.endsWith(".mkv", Qt::CaseInsensitive))
                continue;
            QString error;
            if (Mkv::replaceOriginal(original, remux, &error)) {
                // Do not treat the swapped-in file as a new arrival.
                m_seen.insert(original);
                m_swapWarned.remove(original);
                emit logLine("mkvclean: completed replacement of " + original);
            } else if (!m_swapWarned.contains(original)) {
                m_swapWarned.insert(original);
                emit logLine("mkvclean: " + original + " is still in use; will retry the replacement");
            }
        }
    }
}

void Watcher::pump() {
    if (m_pending.isEmpty() || m_processing)
        return;
    m_processing = true;
    const QString path = m_pending.takeFirst();
    emit logLine("mkvclean: cleaning " + path);
    m_backend.openPath(path);
}

void Watcher::finishOne() {
    m_processing = false;
    m_backend.reset();
    pump();
}


