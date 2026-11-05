#include "mkv.h"

#include <QCollator>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>

const MkvTrack *MkvPlan::track(int id) const {
    for (const auto &candidate : tracks)
        if (candidate.id == id)
            return &candidate;
    return nullptr;
}

int MkvPlan::removedCount() const {
    int removed = 0;
    for (const auto &candidate : tracks) {
        if (candidate.type == "video")
            continue;
        if (candidate.id == keepAudioId)
            continue;
        if (extractSubtitleIds.contains(candidate.id))
            continue;
        ++removed;
    }
    return removed;
}

QList<MkvTrack> Mkv::parseTracks(const QJsonDocument &identification) {
    QList<MkvTrack> tracks;
    const auto array = identification.object().value("tracks").toArray();
    for (const auto &value : array) {
        const auto object = value.toObject();
        const auto properties = object.value("properties").toObject();
        MkvTrack track;
        track.id = object.value("id").toInt(-1);
        track.type = object.value("type").toString();
        track.codec = object.value("codec").toString();
        track.codecId = properties.value("codec_id").toString();
        track.language = properties.value("language").toString("und");
        track.name = properties.value("track_name").toString();
        track.isDefault = properties.value("default_track").toBool();
        track.isForced = properties.value("forced_track").toBool();
        track.isHearingImpaired = properties.value("hearing_impaired").toBool();
        if (track.id >= 0 && !track.type.isEmpty())
            tracks.append(track);
    }
    return tracks;
}

MkvPlan Mkv::makePlan(const QList<MkvTrack> &tracks) {
    MkvPlan plan;
    plan.tracks = tracks;

    // English subtitles are pulled out as files first; the remux drops all subs.
    for (const auto &track : tracks)
        if (track.type == "subtitles" && track.language == "eng")
            plan.extractSubtitleIds.append(track.id);

    // The default English audio track survives; without any English audio
    // there is nothing sensible to keep, so refuse rather than guess.
    const MkvTrack *firstEnglish = nullptr;
    for (const auto &track : tracks) {
        if (track.type != "audio" || track.language != "eng")
            continue;
        if (!firstEnglish)
            firstEnglish = &track;
        if (track.isDefault) {
            plan.keepAudioId = track.id;
            break;
        }
    }
    if (plan.keepAudioId < 0 && firstEnglish)
        plan.keepAudioId = firstEnglish->id;
    if (plan.keepAudioId < 0)
        plan.error = "no-english-audio";
    return plan;
}

QString Mkv::subtitleExtension(const QString &codecId) {
    if (codecId == "S_TEXT/UTF8" || codecId == "S_TEXT/ASCII")
        return "srt";
    if (codecId == "S_TEXT/ASS" || codecId == "S_TEXT/SSA")
        return "ass";
    if (codecId == "S_TEXT/WEBVTT")
        return "vtt";
    if (codecId == "S_TEXT/USF")
        return "usf";
    if (codecId == "S_HDMV/PGS")
        return "sup";
    if (codecId == "S_VOBSUB")
        return "sub";
    // Anything exotic still extracts; the CodecID tail is a fair extension.
    const QString tail = codecId.section('/', -1).toLower();
    return tail.isEmpty() ? "sub" : tail;
}

MkvSubtitleClass Mkv::subtitleClass(const MkvTrack &track) {
    if (track.isForced)
        return MkvSubtitleClass::Forced;
    if (track.isHearingImpaired)
        return MkvSubtitleClass::Sdh;
    // Releases often skip the flag and just say so in the track name.
    static const QRegularExpression sdhName("\\b(sdh|cc|hearing[ -]?impaired)\\b",
                                            QRegularExpression::CaseInsensitiveOption);
    if (sdhName.match(track.name).hasMatch())
        return MkvSubtitleClass::Sdh;
    return MkvSubtitleClass::Plain;
}

// groupIndex counts within the track's class, so a file with a plain, an SDH,
// and a forced track yields ".en.srt", ".sdh.en.srt", and ".forced.en.srt".
QString Mkv::subtitleFileName(const QString &inputPath, const MkvTrack &track, int groupIndex) {
    const QFileInfo info(inputPath);
    QString name = info.absolutePath() + "/" + info.completeBaseName();
    switch (subtitleClass(track)) {
    case MkvSubtitleClass::Forced: name += ".forced"; break;
    case MkvSubtitleClass::Sdh: name += ".sdh"; break;
    case MkvSubtitleClass::Plain: break;
    }
    if (groupIndex > 0)
        name += "." + QString::number(groupIndex + 1);
    return name + ".en." + subtitleExtension(track.codecId);
}

QString Mkv::availablePath(const QString &desired) {
    if (!QFileInfo::exists(desired))
        return desired;
    const QFileInfo info(desired);
    const QString base = info.absolutePath() + "/" + info.completeBaseName();
    const QString suffix = info.suffix();
    for (int i = 2;; ++i) {
        const QString candidate = QString("%1-%2.%3").arg(base).arg(i).arg(suffix);
        if (!QFileInfo::exists(candidate))
            return candidate;
    }
}

QStringList Mkv::collectMkvFiles(const QStringList &paths) {
    QStringList files;
    for (const auto &path : paths) {
        const QFileInfo info(path);
        if (info.isDir()) {
            QDirIterator it(info.absoluteFilePath(), {"*.mkv", "*.MKV"},
                            QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext())
                files.append(it.next());
        } else if (info.exists() && info.suffix().compare("mkv", Qt::CaseInsensitive) == 0) {
            files.append(info.absoluteFilePath());
        }
    }
    files.removeDuplicates();
    const QCollator collator = [] {
        QCollator c;
        c.setNumericMode(true);
        return c;
    }();
    std::sort(files.begin(), files.end(), collator);
    return files;
}

QStringList Mkv::extractArguments(const QString &inputPath, const MkvExtractJobs &jobs) {
    QStringList arguments{"tracks", inputPath};
    for (const auto &job : jobs)
        arguments.append(QString("%1:%2").arg(job.first).arg(job.second));
    return arguments;
}

QStringList Mkv::remuxArguments(const QString &inputPath, const QString &outputPath, const MkvPlan &plan) {
    // --gui-mode turns progress and errors into parseable "#GUI#…" stdout lines.
    QStringList arguments{"--gui-mode", "-o", outputPath, "--no-subtitles",
                          "--audio-tracks", QString::number(plan.keepAudioId)};
    // Players show leftover track names ("English [Original]") instead of the
    // language; blank the name of every track that survives.
    for (const auto &track : plan.tracks)
        if (track.type == "video" || track.id == plan.keepAudioId)
            arguments << "--track-name" << QString("%1:").arg(track.id);
    arguments << inputPath;
    return arguments;
}
