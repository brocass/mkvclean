#pragma once

#include <QJsonDocument>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

// One track from `mkvmerge -J` identification output.
struct MkvTrack {
    int id = -1;
    QString type;      // "video", "audio", "subtitles", ...
    QString codec;     // Human-readable, e.g. "SubRip/SRT"
    QString codecId;   // Matroska CodecID, e.g. "S_TEXT/UTF8"
    QString language;  // ISO 639-2, "und" when the file doesn't say
    QString name;
    bool isDefault = false;
    bool isForced = false;
    bool isHearingImpaired = false;
};

// How an extracted subtitle file is tagged: plain (".en"), SDH (".sdh.en"),
// or forced (".forced.en").
enum class MkvSubtitleClass { Plain, Sdh, Forced };

// What mkvclean will do with an inspected file: keep the video and the
// default English audio, extract the English subtitles, drop the rest.
struct MkvPlan {
    QList<MkvTrack> tracks;
    int keepAudioId = -1;
    QList<int> extractSubtitleIds;
    // Non-empty blocks the plan; "no-english-audio" is the only one so far.
    QString error;

    const MkvTrack *track(int id) const;
    // Tracks that vanish from the output entirely (extra audio, foreign subs);
    // extracted subtitles are counted separately, not here.
    int removedCount() const;
};

using MkvExtractJobs = QList<QPair<int, QString>>;  // track id, output path

namespace Mkv {

QList<MkvTrack> parseTracks(const QJsonDocument &identification);
MkvPlan makePlan(const QList<MkvTrack> &tracks);

// Forced beats SDH beats plain. SDH comes from the hearing-impaired flag, or
// from the track name ("SDH", "[CC]", "hearing impaired") when the flag is
// missing, as it often is.
MkvSubtitleClass subtitleClass(const MkvTrack &track);

// Subtitle CodecID ("S_TEXT/UTF8") to the file extension players expect ("srt").
QString subtitleExtension(const QString &codecId);

// "<base>[.sdh|.forced][.N].en.<ext>"; groupIndex counts within the track's
// own class of extracted tracks.
QString subtitleFileName(const QString &inputPath, const MkvTrack &track, int groupIndex);

// The desired name, then "-2", "-3"… before the extension, until nothing is there.
QString availablePath(const QString &desired);

// Every MKV among the inputs: files pass through, folders are scanned
// recursively. Sorted naturally (e02 before e10), duplicates removed.
QStringList collectMkvFiles(const QStringList &paths);

QStringList extractArguments(const QString &inputPath, const MkvExtractJobs &jobs);
QStringList remuxArguments(const QString &inputPath, const QString &outputPath, const MkvPlan &plan);

}
