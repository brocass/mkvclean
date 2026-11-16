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
    // Video tracks only; 0 when the container doesn't say.
    int width = 0;
    int height = 0;
};

// The non-black part of a frame, as ffmpeg's cropdetect reports it: the
// content size and the offset of the bars trimmed from the left and top.
struct MkvCrop {
    int width = 0;
    int height = 0;
    int x = 0;
    int y = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;

    bool isValid() const { return width > 0 && height > 0; }
    // A valid crop that actually removes something; a full-frame answer is a
    // video without bars. A couple of pixels is edge noise from cropdetect
    // (a plain 160x120 test pattern reports 158x120), not letterboxing, so
    // require at least four pixels trimmed in one dimension.
    bool hasBars() const
    {
        return isValid() && sourceWidth > 0 && sourceHeight > 0
               && (sourceWidth - width >= 4 || sourceHeight - height >= 4);
    }
    // "1920:800:0:140", the ffmpeg crop filter payload.
    QString filter() const
    {
        return QString("%1:%2:%3:%4").arg(width).arg(height).arg(x).arg(y);
    }
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

// One subtitle extraction. standardPath is set only for an SDH track that the
// user asked to convert: mkvextract writes outputPath ("….sdh.en.srt"), then a
// cleaned copy is written to standardPath ("….en.srt").
struct MkvExtractJob {
    int trackId = -1;
    QString outputPath;
    QString standardPath;
};

using MkvExtractJobs = QList<MkvExtractJob>;

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
// suffix for languageCode ("eng" → ".en.", "dan" → ".da."). asStandard names
// an SDH track as if it were plain, for a converted copy.
QString subtitleFileName(const QString &inputPath, const MkvTrack &track, int groupIndex,
                         const QString &languageCode = "eng", bool asStandard = false);

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

// Swap a verified remux into place with one atomic rename over the original.
// The original is never removed first, so a refused swap leaves both files
// exactly as they were (the remux stays as "<original>.remux") for a later
// pass to complete the swap.
bool replaceOriginal(const QString &originalPath, const QString &remuxPath, QString *error = nullptr);

QStringList extractArguments(const QString &inputPath, const MkvExtractJobs &jobs);
QStringList remuxArguments(const QString &inputPath, const QString &outputPath, const MkvPlan &plan);

// Container duration in milliseconds from `mkvmerge -J`, 0 when unknown.
qint64 durationMs(const QJsonDocument &identification);

// A fast black-bar probe: decodes only keyframes, skipping the first and last
// few percent (fades are black), so even a large library is quick to scan.
QStringList cropDetectArguments(const QString &inputPath, qint64 durationMs);

// The most common cropdetect rectangle in the output, ignoring a full-frame
// answer and implausibly small ones (a black scene, not letterboxing). Needs
// the source dimensions to know what "full frame" means.
MkvCrop parseCrop(const QString &output, int sourceWidth, int sourceHeight);

// The HandBrake "1080p x265 Slow and Low" equivalent: crop to the detected
// rectangle, deinterlace only interlaced frames, shrink to fit 1920x1080
// without ever upscaling, libx265 10-bit slow CRF 21.5 with the preset's
// advanced options, audio copied, chapters and metadata kept.
QStringList encodeArguments(const QString &inputPath, const QString &outputPath,
                            const MkvPlan &plan, const MkvCrop &crop);

}
