#include "mkv.h"

#include <QCollator>
#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>

#include <filesystem>
#include <system_error>

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
        const QString dimensions = properties.value("pixel_dimensions")
                                       .toString(properties.value("display_dimensions").toString());
        const auto sides = dimensions.split('x');
        if (sides.size() == 2) {
            track.width = sides.at(0).toInt();
            track.height = sides.at(1).toInt();
        }
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
                              const QString &languageCode, bool asStandard) {
    const QFileInfo info(inputPath);
    QString name = info.absolutePath() + "/" + info.completeBaseName();
    switch (asStandard ? MkvSubtitleClass::Plain : subtitleClass(track)) {
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
    // Atomic swap: POSIX rename(2) puts the remux in the original's place in
    // one step. It is never a remove followed by a rename, so the original
    // cannot be unlinked while the replacement is missing. Renaming over an
    // open file is fine on Linux (the open handle keeps the old inode alive).
    // If the rename is refused (permissions, read-only mount, ...) both files
    // are left exactly as they were: the original intact and the verified
    // remux alongside it as "<original>.remux" for a later pass.
    std::error_code ec;
    std::filesystem::rename(std::filesystem::path(remuxPath.toStdString()),
                            std::filesystem::path(originalPath.toStdString()), ec);
    if (ec) {
        if (error)
            *error = QString::fromStdString(ec.message());
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
        arguments.append(QString("%1:%2").arg(job.trackId).arg(job.outputPath));
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

qint64 Mkv::durationMs(const QJsonDocument &identification) {
    const auto container = identification.object().value("container").toObject();
    const auto value = container.value("properties").toObject().value("duration");
    const double nanoseconds = value.toVariant().toDouble();
    return nanoseconds > 0 ? static_cast<qint64>(nanoseconds / 1000000.0) : 0;
}

QStringList Mkv::cropDetectArguments(const QString &inputPath, qint64 durationMs) {
    // Keyframes alone are enough to see where the picture ends, and decoding
    // only those keeps even a 4K library scan cheap. Fades and title cards
    // are excluded by starting a little way in.
    QStringList arguments{"-hide_banner", "-nostdin", "-loglevel", "info"};
    qint64 startMs = 0, spanMs = 0;
    if (durationMs > 120000) {
        startMs = durationMs / 20;             // 5%
        spanMs = durationMs - 2 * startMs;     // up to 90%
    }
    if (startMs > 0)
        arguments << "-ss" << QString::number(startMs / 1000.0, 'f', 3);
    arguments << "-skip_frame" << "nokey" << "-i" << inputPath;
    if (spanMs > 0)
        arguments << "-t" << QString::number(spanMs / 1000.0, 'f', 3);
    arguments << "-map" << "0:v:0" << "-an" << "-sn" << "-dn"
              << "-vf" << "cropdetect=limit=24:round=2:reset=0:skip=0"
              << "-f" << "null" << "-";
    return arguments;
}

MkvCrop Mkv::parseCrop(const QString &output, int sourceWidth, int sourceHeight) {
    static const QRegularExpression expression("crop=(\\d+):(\\d+):(-?\\d+):(-?\\d+)");
    auto matches = expression.globalMatch(output);
    // A single odd frame (a fade, a dark scene) must not win: take the most
    // common plausible rectangle across the sample.
    QList<QString> order;
    QHash<QString, int> counts;
    QHash<QString, MkvCrop> crops;
    while (matches.hasNext()) {
        const auto match = matches.next();
        const QString key = match.captured(0);
        if (!counts.contains(key))
            order.append(key);
        ++counts[key];
        MkvCrop crop;
        crop.sourceWidth = sourceWidth;
        crop.sourceHeight = sourceHeight;
        crop.width = match.captured(1).toInt();
        crop.height = match.captured(2).toInt();
        crop.x = qMax(0, match.captured(3).toInt());
        crop.y = qMax(0, match.captured(4).toInt());
        crops.insert(key, crop);
    }

    MkvCrop best;
    int bestCount = 0;
    for (const auto &key : order) {
        const MkvCrop crop = crops.value(key);
        if (!crop.isValid())
            continue;
        if (sourceWidth > 0 && sourceHeight > 0) {
            // A full-frame crop is a video without bars; a rectangle smaller
            // than a quarter of the frame is a black scene, not letterboxing.
            if (crop.width >= sourceWidth && crop.height >= sourceHeight)
                continue;
            if (crop.width < sourceWidth / 4 || crop.height < sourceHeight / 4)
                continue;
        }
        if (counts.value(key) > bestCount) {
            best = crop;
            bestCount = counts.value(key);
        }
    }
    return best;
}

QStringList Mkv::encodeArguments(const QString &inputPath, const QString &outputPath,
                                 const MkvPlan &plan, const MkvCrop &crop) {
    QStringList arguments{"-hide_banner", "-nostdin", "-y", "-loglevel", "warning",
                          "-i", inputPath, "-map", "0:v:0"};

    // mkvmerge track ids and ffmpeg stream indices differ; audio streams are
    // numbered in container order, which is the order makePlan preserved.
    int audioOrdinal = -1, ordinal = 0;
    for (const auto &track : plan.tracks) {
        if (track.type != "audio")
            continue;
        if (track.id == plan.keepAudioId) {
            audioOrdinal = ordinal;
            break;
        }
        ++ordinal;
    }
    if (audioOrdinal >= 0)
        arguments << "-map" << QString("0:a:%1").arg(audioOrdinal);
    else
        arguments << "-an";
    arguments << "-map_chapters" << "0" << "-map_metadata" << "0";

    // The "1080p x265 Slow and Low" preset: x265 10-bit, slow, RF 21.5, plus
    // the advanced option string stored in the preset.
    arguments << "-c:v" << "libx265" << "-preset" << "slow" << "-crf" << "21.5"
              << "-pix_fmt" << "yuv420p10le"
              << "-x265-params"
              << "strong-intra-smoothing=0:rect=0:aq-mode=1:rd=4:psy-rd=0.75:"
                 "psy-rdoq=4.0:rdoq-level=1:rskip=2";

    // Crop first, deinterlace the frames flagged as interlaced (HandBrake's
    // preset uses decomb; ffmpeg has no decomb filter, but yadif with
    // deint=interlaced is a pass-through for progressive video), then shrink
    // to the preset's 1920x1080 ceiling. The bounds are the smaller of the
    // target and the input, so a DVD never grows.
    QStringList filters;
    if (crop.isValid())
        filters << "crop=" + crop.filter();
    filters << "yadif=deint=interlaced";
    filters << "scale=w='min(1920,iw)':h='min(1080,ih)':"
               "force_original_aspect_ratio=decrease:force_divisible_by=2";
    arguments << "-vf" << filters.join(',') << "-fps_mode" << "passthrough";

    if (audioOrdinal >= 0)
        arguments << "-c:a" << "copy";
    arguments << "-sn" << "-progress" << "pipe:1" << "-nostats"
              << "-f" << "matroska" << outputPath;
    return arguments;
}
