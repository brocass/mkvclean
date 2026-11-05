#include "backend.h"

#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

#include "filepicker.h"
#include "settings.h"

Backend::Backend(FilePicker *picker, QObject *parent, const QString &mkvmerge, const QString &mkvextract,
                 int inspectTimeoutMs)
    : QObject(parent),
      m_mkvmerge(mkvmerge.isEmpty() ? QStandardPaths::findExecutable("mkvmerge") : mkvmerge),
      m_mkvextract(mkvextract.isEmpty() ? QStandardPaths::findExecutable("mkvextract") : mkvextract),
      m_picker(picker),
      m_inspectTimeoutMs(inspectTimeoutMs) {
    m_inspectTimeout.setSingleShot(true);
    m_inspectTimeout.setInterval(inspectTimeoutMs);
    connect(&m_inspectTimeout, &QTimer::timeout, this, [this] {
        if (m_stage == Stage::Inspecting)
            m_process.kill();
    });
    // The scan pool's watchdog: kill any probe that has hung too long.
    m_scanTick.setInterval(1000);
    connect(&m_scanTick, &QTimer::timeout, this, [this] {
        const auto now = QDateTime::currentDateTime();
        for (auto it = m_scanProbeStarted.begin(); it != m_scanProbeStarted.end(); ++it)
            if (it.value().msecsTo(now) >= m_inspectTimeoutMs)
                it.key()->kill();
    });

    if (m_picker) {
        connect(m_picker, &FilePicker::filesSelected, this, [this](const QList<QUrl> &urls) {
            QStringList paths;
            for (const auto &url : urls)
                paths.append(url.toLocalFile());
            if (m_pickPurpose != PickPurpose::Clean) {
                const auto purpose = m_pickPurpose;
                m_pickPurpose = PickPurpose::Clean;
                if (m_settings)
                    for (const auto &path : paths) {
                        if (purpose == PickPurpose::WatchFolder)
                            m_settings->addWatchFolder(path);
                        else
                            m_settings->addExcludedFolder(path);
                    }
                return;
            }
            openPaths(paths);
        });
        connect(m_picker, &FilePicker::failed, this,
                [this](const QString &message) { failWith(message); });
    }

    // The watch-service toggle in the settings panel reports through systemctl.
    connect(&m_serviceProcess, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        const bool active = QString::fromLocal8Bit(m_serviceProcess.readAllStandardOutput()).trimmed() == "active";
        if (active != m_serviceActive) {
            m_serviceActive = active;
            emit serviceStatusChanged();
        }
    });

    // mkvmerge reports "#GUI#progress 42%" and "#GUI#error …" on stdout.
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &Backend::parseRemuxOutput);
    connect(&m_process, &QProcess::finished, this, &Backend::processFinished);

    if (!toolsAvailable()) {
        m_errorText = tr("MKVToolNix (mkvmerge, mkvextract) is not installed.");
        m_stage = Stage::Error;
    }
}

bool Backend::toolsAvailable() const {
    return QFileInfo(m_mkvmerge).isExecutable() && QFileInfo(m_mkvextract).isExecutable();
}

bool Backend::busy() const {
    return m_stage == Stage::Inspecting || m_stage == Stage::Extracting || m_stage == Stage::Remuxing;
}

QString Backend::state() const {
    switch (m_stage) {
    case Stage::Idle: return "idle";
    case Stage::Inspecting: return "inspecting";
    case Stage::Ready: return "ready";
    case Stage::Review: return "review";
    case Stage::Extracting: return "extracting";
    case Stage::Remuxing: return "remuxing";
    case Stage::Done: return "done";
    case Stage::Error: return "error";
    }
    return {};
}

QString Backend::sourceName() const {
    return QFileInfo(m_sourcePath).fileName();
}

QString Backend::statusText() const {
    if (m_scanning)
        return tr("Scanning the library…");
    switch (m_stage) {
    case Stage::Inspecting: return tr("Inspecting…");
    case Stage::Extracting: return tr("Extracting subtitles…");
    case Stage::Remuxing: return tr("Writing cleaned file…");
    default: return {};
    }
}

QString Backend::subtitleLanguage() const {
    return m_settings ? m_settings->subtitleLanguage() : QStringLiteral("eng");
}

QString Backend::audioLanguage() const {
    return m_settings ? m_settings->audioLanguage() : QStringLiteral("eng");
}

namespace {
// The track list the UI shows for a file: type, codec, language, flags, and
// what the plan will do with it.
QVariantList planTracks(const MkvPlan &plan) {
    QVariantList list;
    for (const auto &track : plan.tracks) {
        QString action;
        if (track.type == "video")
            action = "keep";
        else if (track.id == plan.keepAudioId)
            action = "keep";
        else if (plan.extractSubtitleIds.contains(track.id))
            action = "extract";
        else
            action = "remove";
        list.append(QVariantMap{
            {"id", track.id},
            {"type", track.type},
            {"codec", track.codec},
            {"language", track.language.toUpper()},
            {"name", track.name},
            {"isDefault", track.isDefault},
            {"isForced", track.isForced},
            {"isSdh", Mkv::subtitleClass(track) == MkvSubtitleClass::Sdh},
            {"action", action},
        });
    }
    return list;
}
}

QVariantList Backend::tracks() const {
    return planTracks(m_plan);
}

QVariantList Backend::queue() const {
    QVariantList list;
    for (int i = 0; i < m_queuePaths.size(); ++i)
        list.append(QVariantMap{
            {"name", QFileInfo(m_queuePaths.at(i)).fileName()},
            {"path", m_queuePaths.at(i)},
            {"state", m_queueStates.value(i)},
            {"error", m_queueErrors.value(i)},
            {"subtitles", m_queueExtracted.value(i)},
            {"keptAudioId", m_queueKeptAudioIds.value(i, -1)},
            {"tracks", m_queueTracks.value(i)},
        });
    return list;
}

int Backend::countStates(const char *state) const {
    return m_queueStates.count(QLatin1String(state));
}

void Backend::setStage(Stage stage) {
    if (m_stage == stage)
        return;
    m_stage = stage;
    emit stateChanged();
}

void Backend::failWith(const QString &message) {
    m_errorText = message;
    setStage(Stage::Error);
}

void Backend::openFiles() {
    if (busy() || !toolsAvailable() || !m_picker)
        return;
    m_picker->openFiles();
}

void Backend::openFolder() {
    if (busy() || !toolsAvailable() || !m_picker)
        return;
    m_picker->openFolder();
}

void Backend::pickWatchFolder() {
    if (busy() || !toolsAvailable() || !m_picker || !m_settings)
        return;
    m_pickPurpose = PickPurpose::WatchFolder;
    m_picker->openFolder();
}

void Backend::pickExcludeFolder() {
    if (busy() || !toolsAvailable() || !m_picker || !m_settings)
        return;
    m_pickPurpose = PickPurpose::ExcludeFolder;
    m_picker->openFolder();
}

void Backend::setWatchServiceEnabled(bool enabled) {
    QProcess::execute("systemctl", {"--user", enabled ? "enable" : "disable", "--now", "mkvclean"});
    refreshServiceStatus();
}

void Backend::refreshServiceStatus() {
    if (m_serviceProcess.state() != QProcess::NotRunning)
        return;
    m_serviceProcess.start("systemctl", {"--user", "is-active", "mkvclean"});
}

void Backend::openPath(const QString &path) {
    openPaths({path});
}

void Backend::openPaths(const QStringList &paths) {
    if (busy() || !toolsAvailable() || paths.isEmpty())
        return;

    // Drops arrive as file:// URLs; the CLI and dialogs hand over plain paths.
    // Folders expand to every MKV inside them, recursively.
    QStringList locals;
    bool sawDirectory = false;
    for (const auto &raw : paths) {
        const QString local = raw.startsWith("file://") ? QUrl(raw).toLocalFile() : raw;
        sawDirectory |= QFileInfo(local).isDir();
        locals.append(local);
    }
    const QStringList files = Mkv::collectMkvFiles(locals);
    if (files.isEmpty()) {
        failWith(sawDirectory ? tr("No MKV files found in that folder.")
                              : tr("This is not an MKV file."));
        return;
    }

    initQueue(files);
    advance();
}

void Backend::initQueue(const QStringList &files, const QHash<QString, QVariantList> &tracksByPath) {
    m_queuePaths = files;
    m_queueStates.clear();
    m_queueErrors.clear();
    m_queueExtracted.clear();
    m_queueKeptAudioIds.clear();
    m_queueTracks.clear();
    for (int i = 0; i < files.size(); ++i) {
        m_queueStates.append("pending");
        m_queueErrors.append(QString());
        m_queueExtracted.append(QStringList());
        m_queueKeptAudioIds.append(-1);
        m_queueTracks.append(tracksByPath.value(files.at(i)));
    }
    m_queueIndex = -1;
    m_batchAborted = false;
}

void Backend::advance() {
    ++m_queueIndex;
    if (m_queueIndex >= m_queuePaths.size()) {
        setStage(Stage::Done);
        return;
    }
    m_queueStates[m_queueIndex] = "active";
    inspect(m_queuePaths.at(m_queueIndex));
}

void Backend::inspect(const QString &path) {
    m_sourcePath = path;
    m_plan = {};
    m_errorText.clear();
    m_toolError.clear();
    m_extractJobs.clear();
    m_extractedFiles.clear();
    m_outputPath.clear();
    m_partialPath.clear();
    m_progress = 0;
    emit progressChanged();
    setStage(Stage::Inspecting);
    m_inspectTimeout.start();
    m_process.start(m_mkvmerge, {"-J", m_sourcePath});
}

void Backend::scanFolders(const QStringList &folders) {
    if (busy() || !toolsAvailable())
        return;
    QStringList files = Mkv::collectMkvFiles(folders);
    if (m_settings)
        files = Mkv::withoutExcluded(files, m_settings->excludedFolders());
    if (files.isEmpty()) {
        m_scanFoundNothing = true;
        setStage(Stage::Done);
        return;
    }
    reset();  // start from a clean queue
    m_scanning = true;
    m_scanFoundNothing = false;
    m_scanList = files;
    m_scanAccepted.clear();
    m_scanIndex = -1;
    m_scanDone = 0;

    // Probes are I/O-bound, so a handful run at once: on a big library a
    // purely sequential scan would take hours.
    for (int i = 0; i < 4; ++i) {
        auto *process = new QProcess(this);
        connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this, process](int, QProcess::ExitStatus) { probeFinished(process); });
        connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError) {
            probeFinished(process);
        });
        m_scanPool.append(process);
    }
    m_scanTick.start();
    emit stateChanged();
    pumpScanPool();
}

void Backend::pumpScanPool() {
    for (auto *process : m_scanPool) {
        if (m_scanProbeFile.contains(process))
            continue;  // busy
        if (m_scanIndex + 1 >= m_scanList.size())
            break;
        ++m_scanIndex;
        const QString file = m_scanList.at(m_scanIndex);
        m_scanProbeFile.insert(process, file);
        m_scanProbeStarted.insert(process, QDateTime::currentDateTime());
        m_sourcePath = file;
        // The stage stays Inspecting through the whole scan, so setStage
        // alone never refreshes the file name in the UI.
        emit stateChanged();
        process->start(m_mkvmerge, {"-J", file});
    }
    if (m_scanProbeFile.isEmpty() && m_scanIndex + 1 >= m_scanList.size())
        finishScan();
}

void Backend::probeFinished(QProcess *process) {
    if (!m_scanProbeFile.contains(process))
        return;
    const QString file = m_scanProbeFile.take(process);
    m_scanProbeStarted.remove(process);
    if (!m_scanning)  // cancelled while this probe was in flight
        return;

    ++m_scanDone;
    m_progress = m_scanDone * 100 / m_scanList.size();
    emit progressChanged();

    // A probe that read clean JSON keeps the file only if it would change.
    if (process->exitCode() == 0 && process->exitStatus() == QProcess::NormalExit) {
        const auto plan = Mkv::makePlan(Mkv::parseTracks(QJsonDocument::fromJson(process->readAllStandardOutput())),
                                        subtitleLanguage(), audioLanguage());
        if (plan.removedCount() > 0 || !plan.extractSubtitleIds.isEmpty()) {
            m_scanAccepted.append(file);
            m_scanTrackLists.insert(file, planTracks(plan));
        }
    }
    pumpScanPool();
}

void Backend::finishScan() {
    m_scanTick.stop();
    // deleteLater: finishScan runs from a pool process's own finished signal.
    for (auto *process : m_scanPool)
        process->deleteLater();
    m_scanPool.clear();
    m_scanProbeFile.clear();
    m_scanProbeStarted.clear();
    m_scanning = false;
    if (m_scanAccepted.isEmpty()) {
        m_scanFoundNothing = true;
        setStage(Stage::Done);
        return;
    }
    setStage(Stage::Idle);
    // A single dirty file cleans straight away too; pausing for a review
    // after an explicit scan would be odd.
    if (m_scanAccepted.size() == 1)
        m_autoCleanOnce = true;
    // Accepted files are in probe-completion order; present them sorted.
    Mkv::sortNaturally(m_scanAccepted);
    initQueue(m_scanAccepted, m_scanTrackLists);
    m_scanTrackLists.clear();
    if (m_reviewAfterScan) {
        // Let the user trim the list — dual-audio anomalies and all — before
        // anything is processed.
        setStage(Stage::Review);
        return;
    }
    advance();
}

void Backend::startBatch() {
    if (m_stage != Stage::Review || m_queuePaths.isEmpty())
        return;
    // Trimmed down to one file? Clean it straight away, no second review.
    if (m_queuePaths.size() == 1)
        m_autoCleanOnce = true;
    advance();
}

void Backend::removeFromQueue(int index) {
    if (index < 0 || index >= m_queuePaths.size())
        return;
    if (m_stage == Stage::Review) {
        removeQueueRow(index);
        if (m_queuePaths.isEmpty())
            setStage(Stage::Idle);
        else
            emit stateChanged();
        return;
    }
    // During the batch only future, still-pending rows can be dropped.
    if (busy() && index > m_queueIndex && m_queueStates.at(index) == "pending") {
        removeQueueRow(index);
        emit stateChanged();
    }
}

void Backend::removeQueueRow(int index) {
    m_queuePaths.removeAt(index);
    m_queueStates.removeAt(index);
    m_queueErrors.removeAt(index);
    m_queueExtracted.removeAt(index);
    m_queueKeptAudioIds.removeAt(index);
    m_queueTracks.removeAt(index);
    if (index < m_queueIndex)
        --m_queueIndex;
}

void Backend::clean() {
    if (m_stage != Stage::Ready)
        return;
    m_progress = 0;
    emit progressChanged();
    m_toolError.clear();
    startExtraction();
}

void Backend::startExtraction() {
    if (m_plan.extractSubtitleIds.isEmpty()) {
        startRemux();
        return;
    }
    // Plain, SDH, and forced tracks each number within their own class, so
    // the common triple lands as ".en.srt", ".sdh.en.srt", ".forced.en.srt".
    int groupIndices[] = {0, 0, 0};
    for (const int id : m_plan.extractSubtitleIds) {
        const MkvTrack *track = m_plan.track(id);
        const int groupIndex = groupIndices[static_cast<int>(Mkv::subtitleClass(*track))]++;
        const QString target = Mkv::availablePath(
            Mkv::subtitleFileName(m_sourcePath, *track, groupIndex, subtitleLanguage()));
        m_extractJobs.append({id, target});
    }
    setStage(Stage::Extracting);
    m_process.start(m_mkvextract, Mkv::extractArguments(m_sourcePath, m_extractJobs));
}

void Backend::startRemux() {
    // The cleaned file replaces the original in place. mkvmerge writes
    // straight to its destination, so the remux goes to a ".remux" name
    // first: a failed or cancelled run then leaves the original untouched,
    // and a blocked swap keeps the verified remux for a later retry.
    m_outputPath = m_sourcePath;
    m_partialPath = m_sourcePath + ".remux";
    QFile::remove(m_partialPath);  // stale leftover from an earlier crash
    setStage(Stage::Remuxing);
    m_process.start(m_mkvmerge, Mkv::remuxArguments(m_sourcePath, m_partialPath, m_plan));
}

void Backend::cancel() {
    if (m_scanning) {
        // A cancelled scan simply stops; nothing was modified.
        m_scanning = false;
        m_scanTick.stop();
        for (auto *process : m_scanPool) {
            if (m_scanProbeFile.contains(process))
                process->kill();
            process->deleteLater();
        }
        m_scanPool.clear();
        m_scanProbeFile.clear();
        m_scanProbeStarted.clear();
        m_scanList.clear();
        m_scanAccepted.clear();
        m_progress = 0;
        emit progressChanged();
        setStage(Stage::Idle);
        return;
    }
    if (m_stage != Stage::Extracting && m_stage != Stage::Remuxing)
        return;
    m_cancelling = true;
    m_process.kill();
}

void Backend::reset() {
    if (busy())
        return;
    m_plan = {};
    m_sourcePath.clear();
    m_errorText.clear();
    m_toolError.clear();
    m_extractJobs.clear();
    m_extractedFiles.clear();
    m_outputPath.clear();
    m_partialPath.clear();
    m_queuePaths.clear();
    m_queueStates.clear();
    m_queueErrors.clear();
    m_queueExtracted.clear();
    m_queueKeptAudioIds.clear();
    m_queueIndex = -1;
    m_batchAborted = false;
    m_scanning = false;
    m_scanFoundNothing = false;
    m_autoCleanOnce = false;
    m_scanList.clear();
    m_scanAccepted.clear();
    m_scanIndex = -1;
    m_progress = 0;
    emit progressChanged();
    setStage(Stage::Idle);
}

void Backend::openOutputFolder() {
    if (m_outputPath.isEmpty())
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_outputPath).absolutePath()));
}

void Backend::recordQueueError(const QString &message) {
    // A bad file must not stop the batch: mark it, then move on. Only a lone
    // file gets the fullscreen error.
    if (batch()) {
        m_queueStates[m_queueIndex] = "error";
        m_queueErrors[m_queueIndex] = message;
        advance();
    } else {
        failWith(message);
    }
}

void Backend::processFinished(int code, QProcess::ExitStatus status) {
    m_inspectTimeout.stop();

    if (m_cancelling) {
        m_cancelling = false;
        QFile::remove(m_partialPath);
        m_extractJobs.clear();
        if (batch()) {
            // The current file's original is untouched; the batch stops here.
            m_queueStates[m_queueIndex] = "pending";
            m_batchAborted = true;
            setStage(Stage::Done);
        } else {
            setStage(Stage::Ready);
        }
        return;
    }

    const bool failed = code != 0 || status != QProcess::NormalExit;
    switch (m_stage) {
    case Stage::Inspecting: {
        const QByteArray output = m_process.readAllStandardOutput();
        if (failed) {
            recordQueueError(tr("This file could not be read."));
            return;
        }
        m_plan = Mkv::makePlan(Mkv::parseTracks(QJsonDocument::fromJson(output)),
                               subtitleLanguage(), audioLanguage());
        if (m_plan.tracks.isEmpty()) {
            recordQueueError(tr("This file could not be read."));
            return;
        }
        if (batch() || m_autoCleanOnce) {
            // No review step in a batch: straight to the work.
            m_autoCleanOnce = false;
            m_progress = 0;
            emit progressChanged();
            m_toolError.clear();
            startExtraction();
        } else {
            setStage(Stage::Ready);
        }
        return;
    }
    case Stage::Extracting:
        if (failed) {
            m_extractJobs.clear();
            recordQueueError(tr("Extracting the subtitles failed."));
            return;
        }
        for (const auto &job : m_extractJobs)
            m_extractedFiles.append(job.second);
        if (batch())
            m_queueExtracted[m_queueIndex] = m_extractedFiles;
        m_extractJobs.clear();
        startRemux();
        return;
    case Stage::Remuxing: {
        if (failed) {
            QFile::remove(m_partialPath);
            recordQueueError(m_toolError.isEmpty() ? tr("Remuxing failed.") : m_toolError);
            return;
        }
        // The remux verified good, so the original goes now. When something
        // has the original busy (an Emby scan, a player, permissions), the
        // ".remux" file stays behind; the watch service completes the swap on
        // a later pass.
        QString swapError;
        if (!Mkv::replaceOriginal(m_sourcePath, m_partialPath, &swapError)) {
            recordQueueError(tr("The original file is in use; the cleaned file was left as %1.").arg(m_partialPath));
            return;
        }
        m_queueStates[m_queueIndex] = "done";
        m_queueKeptAudioIds[m_queueIndex] = m_plan.keepAudioId;
        m_progress = 100;
        emit progressChanged();
        if (batch()) {
            advance();
            return;
        }
        setStage(Stage::Done);
        return;
    }
    default:
        return;
    }
}

void Backend::parseRemuxOutput() {
    if (m_stage != Stage::Remuxing)
        return;
    static const QRegularExpression progressExpression("#GUI#progress\\s+(\\d+)%");
    static const QRegularExpression errorExpression("#GUI#error\\s+(.*)");
    const QString output = QString::fromLocal8Bit(m_process.readAllStandardOutput());
    for (const auto &line : output.split('\n')) {
        const auto progressMatch = progressExpression.match(line);
        if (progressMatch.hasMatch()) {
            const int value = progressMatch.captured(1).toInt();
            if (value != m_progress) {
                m_progress = value;
                emit progressChanged();
            }
        }
        const auto errorMatch = errorExpression.match(line);
        if (errorMatch.hasMatch())
            m_toolError = errorMatch.captured(1).trimmed();
    }
}
