#include "backend.h"

#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

#include "filepicker.h"

Backend::Backend(FilePicker *picker, QObject *parent, const QString &mkvmerge, const QString &mkvextract)
    : QObject(parent),
      m_mkvmerge(mkvmerge.isEmpty() ? QStandardPaths::findExecutable("mkvmerge") : mkvmerge),
      m_mkvextract(mkvextract.isEmpty() ? QStandardPaths::findExecutable("mkvextract") : mkvextract),
      m_picker(picker) {
    if (m_picker) {
        connect(m_picker, &FilePicker::filesSelected, this, [this](const QList<QUrl> &urls) {
            QStringList paths;
            for (const auto &url : urls)
                paths.append(url.toLocalFile());
            openPaths(paths);
        });
        connect(m_picker, &FilePicker::failed, this,
                [this](const QString &message) { failWith(message); });
    }

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
    switch (m_stage) {
    case Stage::Inspecting: return tr("Inspecting…");
    case Stage::Extracting: return tr("Extracting subtitles…");
    case Stage::Remuxing: return tr("Writing cleaned file…");
    default: return {};
    }
}

QVariantList Backend::tracks() const {
    QVariantList list;
    for (const auto &track : m_plan.tracks) {
        QString action;
        if (track.type == "video")
            action = "keep";
        else if (track.id == m_plan.keepAudioId)
            action = "keep";
        else if (m_plan.extractSubtitleIds.contains(track.id))
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

    m_queuePaths = files;
    m_queueStates.clear();
    m_queueErrors.clear();
    m_queueExtracted.clear();
    m_queueKeptAudioIds.clear();
    for (int i = 0; i < files.size(); ++i) {
        m_queueStates.append("pending");
        m_queueErrors.append(QString());
        m_queueExtracted.append(QStringList());
        m_queueKeptAudioIds.append(-1);
    }
    m_queueIndex = -1;
    m_batchAborted = false;
    advance();
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
    m_process.start(m_mkvmerge, {"-J", m_sourcePath});
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
        const QString target = Mkv::availablePath(Mkv::subtitleFileName(m_sourcePath, *track, groupIndex));
        m_extractJobs.append({id, target});
    }
    setStage(Stage::Extracting);
    m_process.start(m_mkvextract, Mkv::extractArguments(m_sourcePath, m_extractJobs));
}

void Backend::startRemux() {
    // The cleaned file replaces the original in place. mkvmerge writes
    // straight to its destination, so the remux goes to a ".part" name first:
    // a failed or cancelled run then leaves the original untouched.
    m_outputPath = m_sourcePath;
    m_partialPath = m_sourcePath + ".part";
    QFile::remove(m_partialPath);  // stale leftover from an earlier crash
    setStage(Stage::Remuxing);
    m_process.start(m_mkvmerge, Mkv::remuxArguments(m_sourcePath, m_partialPath, m_plan));
}

void Backend::cancel() {
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
        m_plan = Mkv::makePlan(Mkv::parseTracks(QJsonDocument::fromJson(output)));
        if (m_plan.tracks.isEmpty()) {
            recordQueueError(tr("This file could not be read."));
            return;
        }
        if (!m_plan.error.isEmpty()) {
            recordQueueError(tr("No English audio track in this file."));
            return;
        }
        if (batch()) {
            // No review step in a batch: straight to the work.
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
    case Stage::Remuxing:
        if (failed) {
            QFile::remove(m_partialPath);
            recordQueueError(m_toolError.isEmpty() ? tr("Remuxing failed.") : m_toolError);
            return;
        }
        // The remux verified good, so the original goes now: to the trash when
        // the desktop offers one, deleted otherwise — replacing it in place is
        // the point of the app.
        if (!QFile::moveToTrash(m_sourcePath) && !QFile::remove(m_sourcePath)) {
            QFile::remove(m_partialPath);
            recordQueueError(tr("Could not replace the original file."));
            return;
        }
        if (!QFile::rename(m_partialPath, m_outputPath)) {
            recordQueueError(tr("Could not move the finished file into place."));
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
