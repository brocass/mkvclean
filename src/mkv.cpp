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
        // mkvmerge spells the flag "flag_hearing_impaired" in its JSON.
        track.isHearingImpaired = properties.value("flag_hearing_impaired").toBool() ||
                                  properties.value("hearing_impaired").toBool();
        if (track.id >= 0 && !track.type.isEmpty())
            tracks.append(track);
    }
    return tracks;
}

namespace {
struct LanguageEntry {
    const char *code;
    const char *suffix;
    const char *name;
};

// ISO 639-2 (as MKV stores it) → media-server suffix → display name.
const LanguageEntry kLanguages[] = {
    {"eng", "en", "English"}, {"dan", "da", "Danish"}, {"ger", "de", "German"},
    {"fre", "fr", "French"}, {"spa", "es", "Spanish"}, {"ita", "it", "Italian"},
    {"por", "pt", "Portuguese"}, {"dut", "nl", "Dutch"}, {"swe", "sv", "Swedish"},
    {"nor", "no", "Norwegian"}, {"fin", "fi", "Finnish"}, {"pol", "pl", "Polish"},
    {"rus", "ru", "Russian"}, {"jpn", "ja", "Japanese"}, {"kor", "ko", "Korean"},
    {"chi", "zh", "Chinese"}, {"cze", "cs", "Czech"}, {"slo", "sk", "Slovak"},
    {"hun", "hu", "Hungarian"}, {"rum", "ro", "Romanian"}, {"gre", "el", "Greek"},
    {"tur", "tr", "Turkish"}, {"ara", "ar", "Arabic"}, {"heb", "he", "Hebrew"},
    {"hin", "hi", "Hindi"}, {"ukr", "uk", "Ukrainian"}, {"tha", "th", "Thai"},
    {"vie", "vi", "Vietnamese"}, {"ind", "id", "Indonesian"}, {"may", "ms", "Malay"},
    {"fil", "fil", "Filipino"}, {"cat", "ca", "Catalan"}, {"ice", "is", "Icelandic"},
    {"bul", "bg", "Bulgarian"}, {"hrv", "hr", "Croatian"}, {"srp", "sr", "Serbian"},
    {"slv", "sl", "Slovenian"}, {"lit", "lt", "Lithuanian"}, {"lav", "lv", "Latvian"},
    {"est", "et", "Estonian"}, {"cym", "cy", "Welsh"}, {"gle", "ga", "Irish"},
    {"alb", "sq", "Albanian"}, {"mac", "mk", "Macedonian"}, {"bel", "be", "Belarusian"},
    {"bos", "bs", "Bosnian"}, {"bas", "eu", "Basque"}, {"glg", "gl", "Galician"},
    {"geo", "ka", "Georgian"}, {"per", "fa", "Persian"}, {"urd", "ur", "Urdu"},
};
}

QString Mkv::languageSuffix(const QString &code) {
    for (const auto &entry : kLanguages)
        if (code == entry.code)
            return entry.suffix;
    return code;
}

QList<QPair<QString, QString>> Mkv::languages() {
    QList<QPair<QString, QString>> list;
    for (const auto &entry : kLanguages)
        list.append(qMakePair(QString(entry.code), QString(entry.name)));
    return list;
}

MkvPlan Mkv::makePlan(const QList<MkvTrack> &tracks, const QString &subtitleLanguage,
                      const QString &audioLanguage) {
    MkvPlan plan;
    plan.tracks = tracks;

    // Subtitles in the preferred language are pulled out as files first; the
    // remux drops all subs. Forced always comes along, but SDH/CC is only a
    // fallback for when there is no plain subtitle at all.
    bool hasPlain = false;
    for (const auto &track : tracks)
        if (track.type == "subtitles" && track.language == subtitleLanguage &&
            subtitleClass(track) == MkvSubtitleClass::Plain)
            hasPlain = true;
    for (const auto &track : tracks) {
        if (track.type != "subtitles" || track.language != subtitleLanguage)
            continue;
        if (hasPlain && subtitleClass(track) == MkvSubtitleClass::Sdh)
            continue;
        plan.extractSubtitleIds.append(track.id);
    }

    // One audio track survives: the default track in the preferred language
    // wins, then the first in that language, then — a genuinely foreign film
    // stays watchable — the default foreign track, then the first audio
    // track at all. -1 means no audio exists, and the output is video-only.
    const MkvTrack *firstAudio = nullptr, *defaultAudio = nullptr, *firstPreferred = nullptr;
    for (const auto &track : tracks) {
        if (track.type != "audio")
            continue;
        if (!firstAudio)
            firstAudio = &track;
        if (track.isDefault && !defaultAudio)
            defaultAudio = &track;
        if (track.language == audioLanguage) {
            if (!firstPreferred)
                firstPreferred = &track;
            if (track.isDefault) {
                plan.keepAudioId = track.id;
                return plan;
            }
        }
    }
    if (firstPreferred)
        plan.keepAudioId = firstPreferred->id;
    else if (defaultAudio)
        plan.keepAudioId = defaultAudio->id;
    else if (firstAudio)
        plan.keepAudioId = firstAudio->id;
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
QString Mkv::subtitleFileName(const QString &inputPath, const MkvTrack &track, int groupIndex,
                              const QString &languageCode) {
    const QFileInfo info(inputPath);
    QString name = info.absolutePath() + "/" + info.completeBaseName();
    switch (subtitleClass(track)) {
    case MkvSubtitleClass::Forced: name += ".forced"; break;
    case MkvSubtitleClass::Sdh: name += ".sdh"; break;
    case MkvSubtitleClass::Plain: break;
    }
    if (groupIndex > 0)
        name += "." + QString::number(groupIndex + 1);
    return name + "." + languageSuffix(languageCode) + "." + subtitleExtension(track.codecId);
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

QStringList Mkv::withoutExcluded(const QStringList &paths, const QStringList &excludedFolders) {
    QStringList kept;
    for (const auto &path : paths) {
        bool excluded = false;
        for (const auto &folder : excludedFolders) {
            const QString prefix = folder.endsWith('/') ? folder : folder + '/';
            if (path == folder || path.startsWith(prefix)) {
                excluded = true;
                break;
            }
        }
        if (!excluded)
            kept.append(path);
    }
    return kept;
}

bool Mkv::replaceOriginal(const QString &originalPath, const QString &remuxPath, QString *error) {
    // Swap the verified remux into place; the original only leaves its spot
    // once the swap is underway, and when anything blocks it (original in
    // use, permissions, ...) the original stays intact and the remux is left
    // behind as "<original>.remux" for a later pass to complete the swap.
    if (QFileInfo::exists(originalPath) && !QFile::remove(originalPath)) {
        if (error)
            *error = "the original could not be removed";
        return false;
    }
    if (!QFile::rename(remuxPath, originalPath)) {
        if (error)
            *error = "the original file is in use";
        return false;
    }
    return true;
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
    sortNaturally(files);
    return files;
}

void Mkv::sortNaturally(QStringList &paths) {
    QCollator collator;
    collator.setNumericMode(true);
    std::sort(paths.begin(), paths.end(), collator);
}

QStringList Mkv::extractArguments(const QString &inputPath, const MkvExtractJobs &jobs) {
    QStringList arguments{"tracks", inputPath};
    for (const auto &job : jobs)
        arguments.append(QString("%1:%2").arg(job.first).arg(job.second));
    return arguments;
}

QStringList Mkv::remuxArguments(const QString &inputPath, const QString &outputPath, const MkvPlan &plan) {
    // --gui-mode turns progress and errors into parseable "#GUI#…" stdout lines.
    QStringList arguments{"--gui-mode", "-o", outputPath, "--no-subtitles"};
    if (plan.keepAudioId < 0)
        arguments << "--no-audio";
    else
        arguments << "--audio-tracks" << QString::number(plan.keepAudioId);
    // Players show leftover track names ("English [Original]") instead of the
    // language; blank the name of every track that survives.
    for (const auto &track : plan.tracks)
        if (track.type == "video" || track.id == plan.keepAudioId)
            arguments << "--track-name" << QString("%1:").arg(track.id);
    arguments << inputPath;
    return arguments;
}
