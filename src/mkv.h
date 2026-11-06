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

// What mkvclean will do with an inspected file: keep the video and one audio
// track — the default English one, falling back to the first English, then to
// the default foreign track (a foreign film stays watchable) — extract the
// English subtitles, and drop the rest. keepAudioId is -1 when there is no
// audio at all, meaning a video-only output.
struct MkvPlan {
    QList<MkvTrack> tracks;
    int keepAudioId = -1;
    QList<int> extractSubtitleIds;

    const MkvTrack *track(int id) const;
    // Tracks that vanish from the output entirely (extra audio, foreign subs);
    // extracted subtitles are counted separately, not here.
    int removedCount() const;
};

using MkvExtractJobs = QList<QPair<int, QString>>;  // track id, output path

namespace Mkv {

QList<MkvTrack> parseTracks(const QJsonDocument &identification);

// Languages are ISO 639-2 codes ("eng", "dan", "ger"); both default to
// English.
MkvPlan makePlan(const QList<MkvTrack> &tracks, const QString &subtitleLanguage = "eng",
                 const QString &audioLanguage = "eng");

// The short suffix media servers expect for a language code ("eng" → "en");
// unknown codes pass through unchanged.
QString languageSuffix(const QString &code);

// (ISO 639-2 code, English name) pairs for the settings dropdowns.
QList<QPair<QString, QString>> languages();

// Forced beats SDH beats plain. SDH comes from the hearing-impaired flag, or
// from the track name ("SDH", "[CC]", "hearing impaired") when the flag is
// missing, as it often is.
MkvSubtitleClass subtitleClass(const MkvTrack &track);

// Subtitle CodecID ("S_TEXT/UTF8") to the file extension players expect ("srt").
QString subtitleExtension(const QString &codecId);

// "<base>[.sdh|.forced][.N].<lang>.<ext>"; groupIndex counts within the
// track's own class of extracted tracks, and <lang> is the media-server
// suffix for languageCode ("eng" → ".en.", "dan" → ".da.").
QString subtitleFileName(const QString &inputPath, const MkvTrack &track, int groupIndex,
                         const QString &languageCode = "eng");

// The desired name, then "-2", "-3"… before the extension, until nothing is there.
QString availablePath(const QString &desired);

// Every MKV among the inputs: files pass through, folders are scanned
// recursively. Sorted naturally (e02 before e10), duplicates removed.
QStringList collectMkvFiles(const QStringList &paths);

// Natural (human) sort: e02 sorts before e10.
void sortNaturally(QStringList &paths);

// Paths with everything under the excluded folders dropped; a folder itself
// matches, a sibling with a similar name ("/videos/Music" vs "/videos/Music
// Videos") does not. Automation (watcher, scan) uses this; explicit cleans
// still process whatever they are pointed at.
QStringList withoutExcluded(const QStringList &paths, const QStringList &excludedFolders);

// Swap a verified remux into place: rename the remux over the original
// atomically; the original is left untouched when the swap is refused (the
// original in use, permissions); the remux file is then left untouched so a
// later pass can complete the swap.
bool replaceOriginal(const QString &originalPath, const QString &remuxPath, QString *error = nullptr);

QStringList extractArguments(const QString &inputPath, const MkvExtractJobs &jobs);
QStringList remuxArguments(const QString &inputPath, const QString &outputPath, const MkvPlan &plan);

}
