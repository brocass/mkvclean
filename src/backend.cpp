#include "backend.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

#include "filepicker.h"
#include "sdh.h"
#include "settings.h"

Backend::Backend(FilePicker *picker, QObject *parent, const QString &mkvmerge, const QString &mkvextract,
                 int inspectTimeoutMs, const QString &ffmpeg)
    : QObject(parent),
      m_inspectTimeoutMs(inspectTimeoutMs),
      m_mkvmerge(mkvmerge.isEmpty() ? QStandardPaths::findExecutable("mkvmerge") : mkvmerge),
      m_mkvextract(mkvextract.isEmpty() ? QStandardPaths::findExecutable("mkvextract") : mkvextract),
      m_ffmpeg(ffmpeg.isEmpty() ? QStandardPaths::findExecutable("ffmpeg") : ffmpeg),
      m_picker(picker) {
    // Crop detection decodes keyframes across the whole file, so it needs
    // longer than an identification probe; a 4K film on a slow disk is not
    // hung, just slow.
    m_cropTimeoutMs = qMax(inspectTimeoutMs, 600000);
    m_inspectTimeout.setSingleShot(true);
    m_inspectTimeout.setInterval(inspectTimeoutMs);
    connect(&m_inspectTimeout, &QTimer::timeout, this, [this] {
        if (m_stage == Stage::Inspecting || m_stage == Stage::Cropping)
            m_process.kill();
    });
    // The scan pool's watchdog: kill any probe that has hung too long.
    m_scanTick.setInterval(1000);
    connect(&m_scanTick, &QTimer::timeout, this, [this] {
        const auto now = QDateTime::currentDateTime();
        for (auto it = m_scanProbeStarted.begin(); it != m_scanProbeStarted.end(); ++it) {
            const int limit = m_scanCropping.contains(it.key()) ? m_cropTimeoutMs : m_inspectTimeoutMs;
            if (it.value().msecsTo(now) >= limit)
                it.key()->kill();
        }
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
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &Backend::parseToolOutput);
    // ffmpeg's cropdetect and errors arrive on stderr; drain it so the pipe
    // cannot fill, and keep the tail for a useful failure message.
    connect(&m_process, &QProcess::readyReadStandardError, this, &Backend::readStderr);
    connect(&m_process, &QProcess::finished, this, &Backend::processFinished);

    if (!toolsAvailable()) {
        m_errorText = tr("MKVToolNix (mkvmerge, mkvextract) is not installed.");
        m_stage = Stage::Error;
    }
}

bool Backend::toolsAvailable() const {
    return QFileInfo(m_mkvmerge).isExecutable() && QFileInfo(m_mkvextract).isExecutable();
}

bool Backend::ffmpegAvailable() const {
    return QFileInfo(m_ffmpeg).isExecutable();
}

bool Backend::removeBarsDefault() const {
    if (m_cropOverride >= 0)
        return m_cropOverride == 1;
    return m_settings && m_settings->removeBars();
}

bool Backend::cropSelected(int index) const {
    if (index < 0 || index >= m_queueRemoveBars.size())
        return removeBarsDefault();
    return m_queueRemoveBars.at(index);
}

bool Backend::removeBars() const {
    return cropSelected(m_queueIndex);
}

int Backend::barsDetectedCount() const {
    int count = 0;
    for (const auto &crop : m_queueCrops)
        if (crop.hasBars())
            ++count;
    return count;
}

int Backend::barsSelectedCount() const {
    int count = 0;
    for (int i = 0; i < m_queueCrops.size(); ++i)
        if (m_queueCrops.at(i).hasBars() && m_queueRemoveBars.value(i))
            ++count;
    return count;
}

bool Backend::cropped() const {
    return m_queueCropped.value(m_queueIndex, false);
}

void Backend::setRemoveBars(bool enabled) {
    if (m_queueIndex >= 0 && m_queueIndex < m_queueRemoveBars.size()) {
        if (m_queueRemoveBars.at(m_queueIndex) == enabled)
            return;
        m_queueRemoveBars[m_queueIndex] = enabled;
        emit stateChanged();
        return;
    }
    if (m_settings)
        m_settings->setRemoveBars(enabled);
}

void Backend::setQueueRemoveBars(int index, bool enabled) {
    if (index < 0 || index >= m_queueRemoveBars.size())
        return;
    if (m_queueRemoveBars.at(index) == enabled)
        return;
    m_queueRemoveBars[index] = enabled;
    emit stateChanged();
}

void Backend::setAllRemoveBars(bool enabled) {
    bool changed = false;
    for (int i = 0; i < m_queueCrops.size(); ++i) {
        if (!m_queueCrops.at(i).hasBars() || m_queueRemoveBars.value(i) == enabled)
            continue;
        m_queueRemoveBars[i] = enabled;
        changed = true;
    }
    if (changed)
        emit stateChanged();
}

bool Backend::busy() const {
    // A scan leaves the stage at Idle but is very much work in progress:
    // treat it as busy so it cannot be reset, restarted, or opened over.
    return m_scanning || m_stage == Stage::Inspecting || m_stage == Stage::Extracting
           || m_stage == Stage::Cropping || m_stage == Stage::Encoding
           || m_stage == Stage::Remuxing;
}

QString Backend::state() const {
    switch (m_stage) {
    case Stage::Idle: return "idle";
    case Stage::Inspecting: return "inspecting";
    case Stage::Ready: return "ready";
    case Stage::Review: return "review";
    case Stage::Extracting: return "extracting";
    case Stage::Cropping: return "cropping";
    case Stage::Encoding: return "encoding";
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
    case Stage::Cropping: return tr("Looking for black bars…");
    case Stage::Encoding: return tr("Removing black bars…");
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
            {"bars", m_queueCrops.value(i).hasBars()},
            {"cropWidth", m_queueCrops.value(i).width},
            {"cropHeight", m_queueCrops.value(i).height},
            {"removeBars", m_queueRemoveBars.value(i)},
            {"cropped", m_queueCropped.value(i)},
            {"needsWork", m_queueNeedsWork.value(i)},
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

void Backend::initQueue(const QStringList &files, const QHash<QString, QVariantList> &tracksByPath,
                        const QHash<QString, MkvCrop> &cropsByPath,
                        const QHash<QString, bool> &needsWorkByPath) {
    m_queuePaths = files;
    m_queueStates.clear();
    m_queueErrors.clear();
    m_queueExtracted.clear();
    m_queueKeptAudioIds.clear();
    m_queueTracks.clear();
    m_queueCrops.clear();
    m_queueCropKnown.clear();
    m_queueRemoveBars.clear();
    m_queueNeedsWork.clear();
    m_queueCropped.clear();
    for (int i = 0; i < files.size(); ++i) {
        const QString &file = files.at(i);
        const MkvCrop crop = cropsByPath.value(file);
        const bool cropKnown = cropsByPath.contains(file);
        m_queueStates.append("pending");
        m_queueErrors.append(QString());
        m_queueExtracted.append(QStringList());
        m_queueKeptAudioIds.append(-1);
        m_queueTracks.append(tracksByPath.value(file));
        m_queueCrops.append(crop);
        m_queueCropKnown.append(cropKnown);
        // A scan already knows whether there are bars; the tick defaults to
        // the setting, but a bar-less file never starts ticked. Files opened
        // directly have not been probed yet, so they follow the setting.
        m_queueRemoveBars.append(removeBarsDefault() && (!cropKnown || crop.hasBars()));
        m_queueNeedsWork.append(needsWorkByPath.value(file, true));
        m_queueCropped.append(false);
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

void Backend::clearCropState() {
    m_crop = {};
    m_cropKnown = false;
    m_durationMs = 0;
    m_sourceWidth = 0;
    m_sourceHeight = 0;
    m_stderrTail.clear();
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
    m_remuxTempPath.clear();
    m_progress = 0;
    m_stdoutBuffer.clear();
    clearCropState();
    emit progressChanged();
    setStage(Stage::Inspecting);
    m_inspectTimeout.setInterval(m_inspectTimeoutMs);
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
    const QString file = m_scanProbeFile.value(process);
    if (!m_scanning) {  // cancelled while this probe was in flight
        m_scanProbeFile.remove(process);
        m_scanProbeStarted.remove(process);
        m_scanCropping.remove(process);
        return;
    }

    // Phase two: ffmpeg has finished the cropdetect probe for this file.
    if (m_scanCropping.contains(process)) {
        m_scanCropping.remove(process);
        const bool ok = process->exitCode() == 0 && process->exitStatus() == QProcess::NormalExit;
        MkvCrop crop;
        if (ok)
            crop = Mkv::parseCrop(QString::fromLocal8Bit(process->readAllStandardError()),
                                  m_scanSourceWidth.value(file), m_scanSourceHeight.value(file));
        // Insert even when there are no bars: the entry's presence is what
        // tells the queue the file has already been probed.
        m_scanCrops.insert(file, crop);
        retireScanProbe(process);
        return;
    }

    // Phase one: mkvmerge identified the file. Note whether it needs the
    // usual track work, then look for bars before accepting it.
    const bool ok = process->exitCode() == 0 && process->exitStatus() == QProcess::NormalExit;
    qint64 duration = 0;
    int width = 0, height = 0;
    if (ok) {
        const auto json = QJsonDocument::fromJson(process->readAllStandardOutput());
        const auto tracks = Mkv::parseTracks(json);
        const auto plan = Mkv::makePlan(tracks, subtitleLanguage(), audioLanguage());
        m_scanTrackLists.insert(file, planTracks(plan));
        m_scanNeedsWork.insert(
            file, plan.removedCount() > 0 || !plan.extractSubtitleIds.isEmpty());
        for (const auto &track : tracks) {
            if (track.type == "video" && track.width > 0 && track.height > 0) {
                width = track.width;
                height = track.height;
                break;
            }
        }
        duration = Mkv::durationMs(json);
        m_scanSourceWidth.insert(file, width);
        m_scanSourceHeight.insert(file, height);

        if (ffmpegAvailable()) {
            m_scanCropping.insert(process);
            m_scanProbeStarted[process] = QDateTime::currentDateTime();
            process->start(m_ffmpeg, Mkv::cropDetectArguments(file, duration));
            return;
        }
    }
    // Without ffmpeg there is no bar probe; the crop is known to be empty.
    if (ok)
        m_scanCrops.insert(file, MkvCrop{});
    retireScanProbe(process);
}

void Backend::retireScanProbe(QProcess *process) {
    const QString file = m_scanProbeFile.take(process);
    m_scanProbeStarted.remove(process);
    if (!m_scanning)
        return;

    ++m_scanDone;
    m_progress = m_scanDone * 100 / m_scanList.size();
    emit progressChanged();

    // Accepted when there are tracks/subtitles to strip, or black bars to
    // remove — a bar-only file still gets a review row and a tick box.
    if (!file.isEmpty()
        && (m_scanNeedsWork.value(file) || m_scanCrops.value(file).hasBars()))
        m_scanAccepted.append(file);
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
    m_scanCropping.clear();
    m_scanning = false;
    if (m_scanAccepted.isEmpty()) {
        m_scanFoundNothing = true;
        m_scanTrackLists.clear();
        m_scanCrops.clear();
        m_scanNeedsWork.clear();
        m_scanSourceWidth.clear();
        m_scanSourceHeight.clear();
        setStage(Stage::Done);
        return;
    }
    setStage(Stage::Idle);
    // A single dirty file cleans straight away too; pausing for a review
    // after an explicit scan would be odd.
    Mkv::sortNaturally(m_scanAccepted);
    if (m_scanAccepted.size() == 1) {
        const QString &only = m_scanAccepted.first();
        const bool barsTicked = m_scanCrops.value(only).hasBars() && removeBarsDefault();
        if (m_scanNeedsWork.value(only) || barsTicked)
            m_autoCleanOnce = true;
    }
    // Accepted files are in probe-completion order; present them sorted.
    initQueue(m_scanAccepted, m_scanTrackLists, m_scanCrops, m_scanNeedsWork);
    m_scanTrackLists.clear();
    m_scanCrops.clear();
    m_scanNeedsWork.clear();
    m_scanSourceWidth.clear();
    m_scanSourceHeight.clear();
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
    // A bar-only file the user left unticked has nothing else to do; drop it
    // rather than rewriting it unchanged.
    for (int i = m_queuePaths.size() - 1; i >= 0; --i) {
        const bool bars = m_queueCrops.value(i).hasBars() && m_queueRemoveBars.value(i);
        if (!m_queueNeedsWork.value(i, true) && !bars)
            removeQueueRow(i);
    }
    if (m_queuePaths.isEmpty()) {
        setStage(Stage::Idle);
        return;
    }
    m_queueIndex = -1;
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
    m_queueCrops.removeAt(index);
    m_queueCropKnown.removeAt(index);
    m_queueRemoveBars.removeAt(index);
    m_queueNeedsWork.removeAt(index);
    m_queueCropped.removeAt(index);
    if (index < m_queueIndex)
        --m_queueIndex;
}

void Backend::clean() {
    if (m_stage != Stage::Ready)
        return;
    m_progress = 0;
    emit progressChanged();
    m_toolError.clear();
    // Nothing to strip and no crop wanted: leave the original exactly as it
    // is instead of rewriting an identical file.
    if (m_plan.extractSubtitleIds.isEmpty() && m_plan.removedCount() == 0
        && !cropSelected(m_queueIndex)) {
        m_queueStates[m_queueIndex] = "done";
        m_progress = 100;
        emit progressChanged();
        if (batch()) {
            advance();
            return;
        }
        setStage(Stage::Done);
        return;
    }
    startExtraction();
}

void Backend::startExtraction() {
    if (m_plan.extractSubtitleIds.isEmpty()) {
        // No subtitle work: go straight to the crop decision (which may still
        // remux when no bars are wanted).
        startBars();
        return;
    }
    // Plain, SDH, and forced tracks each number within their own class, so
    // the common triple lands as ".en.srt", ".sdh.en.srt", ".forced.en.srt".
    int groupIndices[] = {0, 0, 0};
    for (const int id : m_plan.extractSubtitleIds) {
        const MkvTrack *track = m_plan.track(id);
        const MkvSubtitleClass subtitleClass = Mkv::subtitleClass(*track);
        const int groupIndex = groupIndices[static_cast<int>(subtitleClass)]++;
        MkvExtractJob job;
        job.trackId = id;
        job.outputPath = Mkv::availablePath(
            Mkv::subtitleFileName(m_sourcePath, *track, groupIndex, subtitleLanguage()));
        // SDH tracks can be turned into standard subtitles, but only plain
        // SubRip can be cleaned; ASS and friends keep their SDH name.
        if (m_settings && m_settings->convertSdh()
            && subtitleClass == MkvSubtitleClass::Sdh
            && Mkv::subtitleExtension(track->codecId).compare(QStringLiteral("srt"),
                                                              Qt::CaseInsensitive) == 0) {
            job.standardPath = Mkv::subtitleFileName(m_sourcePath, *track, groupIndex,
                                                     subtitleLanguage(), true);
        }
        m_extractJobs.append(job);
    }
    setStage(Stage::Extracting);
    m_process.start(m_mkvextract, Mkv::extractArguments(m_sourcePath, m_extractJobs));
}

// Writes the cleaned standard copy and drops the SDH-only file. On any
// failure the extracted SDH file is left exactly as it was.
bool Backend::convertSdhSubtitle(MkvExtractJob &job) {
    QFile sdhFile(job.outputPath);
    if (!sdhFile.open(QIODevice::ReadOnly))
        return false;
    const QString source = QString::fromUtf8(sdhFile.readAll());
    sdhFile.close();

    const Sdh::CleanResult cleaned = Sdh::cleanSrt(source);
    // Nothing that looks like SubRip: leave it alone. A cue with only sound
    // descriptions is also kept, rather than writing an empty subtitle.
    if (cleaned.cues == 0 || cleaned.kept == 0)
        return false;

    const QString target = Mkv::availablePath(job.standardPath);
    QSaveFile out(target);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    const QByteArray bytes = cleaned.text.toUtf8();
    if (out.write(bytes) != bytes.size() || !out.commit())
        return false;

    // The standard file is safely on disk before the only SDH copy goes.
    // If the removal is refused, keeping both is harmless.
    QFile::remove(job.outputPath);
    job.outputPath = target;
    return true;
}

// Decide whether the current file needs a video re-encode to lose its bars.
// The crop may already be known from a scan; otherwise ffmpeg probes it now
// (after the subtitles are out, so an unreadable video fails the same way
// regardless of the crop setting).
void Backend::startBars() {
    const bool wanted = cropSelected(m_queueIndex);
    if (wanted && !m_cropKnown) {
        if (!ffmpegAvailable()) {
            recordQueueError(tr("ffmpeg is required to remove black bars."));
            return;
        }
        setStage(Stage::Cropping);
        m_stdoutBuffer.clear();
        m_stderrTail.clear();
        m_inspectTimeout.setInterval(m_cropTimeoutMs);
        m_inspectTimeout.start();
        m_process.start(m_ffmpeg, Mkv::cropDetectArguments(m_sourcePath, m_durationMs));
        return;
    }
    if (wanted && m_crop.hasBars()) {
        startVideoEncode();
        return;
    }
    // The probe found no bars and there is nothing else to strip: do not
    // rewrite an identical file.
    if (m_plan.extractSubtitleIds.isEmpty() && m_plan.removedCount() == 0) {
        m_queueStates[m_queueIndex] = "done";
        m_progress = 100;
        emit progressChanged();
        if (batch()) {
            advance();
            return;
        }
        setStage(Stage::Done);
        return;
    }
    startRemux();
}

// The cleaned file replaces the original in place. The tool writes to a
// private temporary name — never "*.remux" — so the watcher's sweep can only
// ever find a fully written file: naming the in-progress output ".remux" let
// a sweep swap a half-written file into place, after which the finishing job
// unlinked it and the remux was gone too (both files lost). Once the tool
// exits cleanly the temp becomes the verified "<source>.remux" and is swapped
// in atomically; a failed or cancelled run only ever removes its own temp.
void Backend::prepareTempOutput() {
    m_outputPath = m_sourcePath;
    m_partialPath = m_sourcePath + QStringLiteral(".remux");
    m_remuxTempPath = m_sourcePath + QStringLiteral(".mkvclean-%1.tmp")
                          .arg(QCoreApplication::applicationPid());
    // Drop temps left behind by an interrupted earlier run of this file.
    // Only touch ones that have been idle for an hour, so a live temp from
    // another mkvclean process working the same file is never deleted.
    // Match by hand rather than with a QDir glob: release names contain
    // "[…]" and "(…)", which QDir's wildcard-to-regex conversion mangles.
    const QFileInfo sourceInfo(m_sourcePath);
    const QDir sourceDir(sourceInfo.absolutePath());
    const QString tempPrefix = sourceInfo.fileName() + QStringLiteral(".mkvclean-");
    const auto now = QDateTime::currentDateTime();
    for (const auto &name : sourceDir.entryList(QDir::Files)) {
        if (!name.startsWith(tempPrefix) || !name.endsWith(QStringLiteral(".tmp")))
            continue;
        const QFileInfo stale(sourceDir.filePath(name));
        if (stale.lastModified().secsTo(now) > 3600)
            QFile::remove(stale.absoluteFilePath());
    }
    QFile::remove(m_remuxTempPath);
}

void Backend::startVideoEncode() {
    if (!ffmpegAvailable()) {
        recordQueueError(tr("ffmpeg is required to remove black bars."));
        return;
    }
    prepareTempOutput();
    setStage(Stage::Encoding);
    m_stdoutBuffer.clear();
    m_stderrTail.clear();
    m_process.start(m_ffmpeg, Mkv::encodeArguments(m_sourcePath, m_remuxTempPath, m_plan, m_crop));
}

void Backend::startRemux() {
    prepareTempOutput();
    setStage(Stage::Remuxing);
    m_stdoutBuffer.clear();
    m_stderrTail.clear();
    m_process.start(m_mkvmerge, Mkv::remuxArguments(m_sourcePath, m_remuxTempPath, m_plan));
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
        m_scanCropping.clear();
        m_scanList.clear();
        m_scanAccepted.clear();
        m_scanTrackLists.clear();
        m_scanCrops.clear();
        m_scanNeedsWork.clear();
        m_scanSourceWidth.clear();
        m_scanSourceHeight.clear();
        m_progress = 0;
        emit progressChanged();
        setStage(Stage::Idle);
        return;
    }
    if (m_stage != Stage::Extracting && m_stage != Stage::Cropping && m_stage != Stage::Encoding
        && m_stage != Stage::Remuxing)
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
    m_remuxTempPath.clear();
    m_queuePaths.clear();
    m_queueStates.clear();
    m_queueErrors.clear();
    m_queueExtracted.clear();
    m_queueKeptAudioIds.clear();
    m_queueTracks.clear();
    m_queueCrops.clear();
    m_queueCropKnown.clear();
    m_queueRemoveBars.clear();
    m_queueNeedsWork.clear();
    m_queueCropped.clear();
    m_queueIndex = -1;
    m_batchAborted = false;
    m_scanning = false;
    m_scanFoundNothing = false;
    m_autoCleanOnce = false;
    m_scanList.clear();
    m_scanAccepted.clear();
    m_scanIndex = -1;
    m_scanTrackLists.clear();
    m_scanCrops.clear();
    m_scanNeedsWork.clear();
    m_scanSourceWidth.clear();
    m_scanSourceHeight.clear();
    m_scanCropping.clear();
    m_stdoutBuffer.clear();
    clearCropState();
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
    // Drain anything the readyRead slots have not seen yet: the last
    // cropdetect line (or mkvmerge progress/error) can arrive with the exit.
    readStderr();
    parseToolOutput();

    if (m_cancelling) {
        m_cancelling = false;
        // Only the private temp is ours to remove; a verified ".remux" from an
        // earlier attempt is a good cleaned file and stays for a later pass.
        QFile::remove(m_remuxTempPath);
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
        const auto json = QJsonDocument::fromJson(output);
        const auto trackList = Mkv::parseTracks(json);
        m_plan = Mkv::makePlan(trackList, subtitleLanguage(), audioLanguage());
        if (m_plan.tracks.isEmpty()) {
            recordQueueError(tr("This file could not be read."));
            return;
        }
        for (const auto &track : trackList) {
            if (track.type == "video" && track.width > 0 && track.height > 0) {
                m_sourceWidth = track.width;
                m_sourceHeight = track.height;
                break;
            }
        }
        m_durationMs = Mkv::durationMs(json);
        // A scan already probed this file; reuse the answer instead of
        // running cropdetect on it a second time.
        if (m_queueIndex >= 0 && m_queueIndex < m_queueCropKnown.size()
            && m_queueCropKnown.at(m_queueIndex)) {
            m_crop = m_queueCrops.value(m_queueIndex);
            m_cropKnown = true;
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
        for (auto &job : m_extractJobs) {
            if (!job.standardPath.isEmpty())
                convertSdhSubtitle(job);
            m_extractedFiles.append(job.outputPath);
        }
        if (batch())
            m_queueExtracted[m_queueIndex] = m_extractedFiles;
        m_extractJobs.clear();
        startBars();
        return;
    case Stage::Cropping: {
        MkvCrop crop;
        if (!failed)
            crop = Mkv::parseCrop(QString::fromLocal8Bit(m_stderrTail), m_sourceWidth,
                                  m_sourceHeight);
        m_crop = crop;
        m_cropKnown = true;
        if (m_queueIndex >= 0 && m_queueIndex < m_queueCrops.size()) {
            m_queueCrops[m_queueIndex] = crop;
            m_queueCropKnown[m_queueIndex] = true;
        }
        // If the probe itself failed and bars were the only reason to touch
        // the file, say so rather than reporting a clean that did nothing.
        if (failed && cropSelected(m_queueIndex) && m_plan.extractSubtitleIds.isEmpty()
            && m_plan.removedCount() == 0) {
            recordQueueError(tr("Black-bar detection failed."));
            return;
        }
        startBars();
        return;
    }
    case Stage::Encoding:
        if (failed) {
            QFile::remove(m_remuxTempPath);
            QString detail = m_toolError;
            if (detail.isEmpty()) {
                // ffmpeg reports encode errors on stderr, not as #GUI# lines.
                const QStringList lines =
                    QString::fromLocal8Bit(m_stderrTail).split('\n', Qt::SkipEmptyParts);
                for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
                    if (!it->trimmed().isEmpty()) {
                        detail = it->trimmed();
                        break;
                    }
                }
            }
            recordQueueError(detail.isEmpty() ? tr("Re-encoding the video failed.") : detail);
            return;
        }
        finishOutput(true);
        return;
    case Stage::Remuxing:
        if (failed) {
            QFile::remove(m_remuxTempPath);
            recordQueueError(m_toolError.isEmpty() ? tr("Remuxing failed.") : m_toolError);
            return;
        }
        finishOutput(false);
        return;
    default:
        return;
    }
}

void Backend::finishOutput(bool wasCropped) {
    // The tool finished writing the temp file, so it is a verified cleaned
    // copy. Swap it straight into place with one atomic rename. If that is
    // refused (permissions, a read-only mount), the original is untouched:
    // keep the verified output alongside it as "<source>.remux" for a later
    // pass instead of losing it.
    QString swapError;
    if (!Mkv::replaceOriginal(m_sourcePath, m_remuxTempPath, &swapError)) {
        QFile::remove(m_partialPath);
        if (!QFile::rename(m_remuxTempPath, m_partialPath)) {
            QFile::remove(m_remuxTempPath);
            recordQueueError(tr("The cleaned file could not be prepared."));
            return;
        }
        recordQueueError(tr("The cleaned file could not be moved into place; it was left as %1.")
                             .arg(m_partialPath));
        return;
    }
    m_queueStates[m_queueIndex] = "done";
    m_queueKeptAudioIds[m_queueIndex] = m_plan.keepAudioId;
    if (m_queueIndex >= 0 && m_queueIndex < m_queueCropped.size())
        m_queueCropped[m_queueIndex] = wasCropped;
    m_progress = 100;
    emit progressChanged();
    if (batch()) {
        advance();
        return;
    }
    setStage(Stage::Done);
}

void Backend::readStderr() {
    const QByteArray data = m_process.readAllStandardError();
    if (data.isEmpty())
        return;
    m_stderrTail.append(data);
    // cropdetect logs one line per sampled frame; keep only a bounded tail so
    // a long file cannot grow memory without limit.
    constexpr int limit = 262144;
    if (m_stderrTail.size() > limit)
        m_stderrTail = m_stderrTail.right(limit / 2);
}

void Backend::parseToolOutput() {
    if (m_stage != Stage::Remuxing && m_stage != Stage::Encoding)
        return;
    m_stdoutBuffer.append(m_process.readAllStandardOutput());
    int newline = -1;
    while ((newline = m_stdoutBuffer.indexOf('\n')) >= 0) {
        const QString line = QString::fromLocal8Bit(m_stdoutBuffer.left(newline)).trimmed();
        m_stdoutBuffer.remove(0, newline + 1);
        if (line.isEmpty())
            continue;

        if (m_stage == Stage::Remuxing) {
            static const QRegularExpression progressExpression("#GUI#progress\\s+(\\d+)%");
            static const QRegularExpression errorExpression("#GUI#error\\s+(.*)");
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
            continue;
        }

        // ffmpeg's -progress output: out_time_us is microseconds written.
        if (line.startsWith(QStringLiteral("out_time_us="))
            || line.startsWith(QStringLiteral("out_time_ms="))) {
            const qint64 micros = line.mid(12).toLongLong();
            if (m_durationMs > 0 && micros >= 0) {
                const int value =
                    qBound(0, static_cast<int>(micros / 1000 * 100 / m_durationMs), 99);
                if (value != m_progress) {
                    m_progress = value;
                    emit progressChanged();
                }
            }
        } else if (line == QStringLiteral("progress=end")) {
            if (m_progress != 100) {
                m_progress = 100;
                emit progressChanged();
            }
        }
    }
    // Never let a stream without newlines grow without bound.
    if (m_stdoutBuffer.size() > 65536)
        m_stdoutBuffer.clear();
}
