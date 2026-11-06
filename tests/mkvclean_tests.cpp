#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include "backend.h"
#include "mkv.h"
#include "sdh.h"
#include "settings.h"
#include "theme.h"
#include "translator.h"
#include "watcher.h"

class MkvCleanTests : public QObject {
    Q_OBJECT

private slots:
    void parsesTracks()
    {
        const auto tracks = Mkv::parseTracks(QJsonDocument::fromJson(R"({
            "tracks": [
                {"id": 0, "type": "video", "codec": "AVC/H.264", "properties": {"codec_id": "V_MPEG4/ISO/AVC"}},
                {"id": 1, "type": "audio", "codec": "AAC", "properties": {"codec_id": "A_AAC", "language": "eng", "default_track": true, "track_name": "Stereo"}},
                {"id": 2, "type": "subtitles", "codec": "SubRip/SRT", "properties": {"codec_id": "S_TEXT/UTF8", "language": "eng", "forced_track": true, "flag_hearing_impaired": true}}
            ]
        })"));
        QCOMPARE(tracks.size(), 3);
        QCOMPARE(tracks.at(0).language, QString("und"));
        QCOMPARE(tracks.at(1).name, QString("Stereo"));
        QVERIFY(tracks.at(1).isDefault);
        QVERIFY(tracks.at(2).isForced);
        QVERIFY(tracks.at(2).isHearingImpaired);  // spelled flag_hearing_impaired in real JSON
        QCOMPARE(tracks.at(2).codecId, QString("S_TEXT/UTF8"));
    }

    void planKeepsDefaultEnglishAudio()
    {
        const auto plan = Mkv::makePlan({
            audio(1, "eng", false), audio(2, "eng", true), audio(3, "dan", true),
        });
        QCOMPARE(plan.keepAudioId, 2);
        QCOMPARE(plan.removedCount(), 2);
    }

    void planFallsBackToFirstEnglishAudio()
    {
        const auto plan = Mkv::makePlan({audio(1, "dan", true), audio(2, "eng", false)});
        QCOMPARE(plan.keepAudioId, 2);
    }

    void planKeepsTheForeignDefaultAudio()
    {
        // A genuinely foreign film keeps whatever would play by default.
        QCOMPARE(Mkv::makePlan({audio(1, "dan", true), audio(2, "jpn", false)}).keepAudioId, 1);
        QCOMPARE(Mkv::makePlan({audio(1, "dan", false), audio(2, "jpn", true)}).keepAudioId, 2);
        // Nothing marked default: the first audio track stays.
        QCOMPARE(Mkv::makePlan({audio(1, "dan", false), audio(2, "jpn", false)}).keepAudioId, 1);
    }

    void planWithoutAudioKeepsVideoOnly()
    {
        MkvTrack video;
        video.id = 0;
        video.type = "video";
        const auto plan = Mkv::makePlan({video, subtitles(3, "eng")});
        QCOMPARE(plan.keepAudioId, -1);
        QCOMPARE(plan.extractSubtitleIds, QList<int>({3}));
        QCOMPARE(plan.removedCount(), 0);
    }

    void planCollectsOnlyEnglishSubtitles()
    {
        MkvTrack forcedSub = subtitles(4, "eng");
        forcedSub.isForced = true;
        const auto plan = Mkv::makePlan({
            audio(1, "eng", true),
            subtitles(3, "eng"), forcedSub, subtitles(5, "dan"),
        });
        QCOMPARE(plan.extractSubtitleIds, QList<int>({3, 4}));
        // Only the Danish subtitle vanishes without being extracted first.
        QCOMPARE(plan.removedCount(), 1);
    }

    void planSkipsSdhWhenPlainExists()
    {
        MkvTrack sdh = subtitles(4, "eng");
        sdh.isHearingImpaired = true;
        MkvTrack forced = subtitles(5, "eng");
        forced.isForced = true;
        const auto plan = Mkv::makePlan({
            audio(1, "eng", true),
            subtitles(3, "eng"), sdh, forced, subtitles(6, "dan"),
        });
        // Plain and forced extract; the redundant SDH is dropped with the rest.
        QCOMPARE(plan.extractSubtitleIds, QList<int>({3, 5}));
        QCOMPARE(plan.removedCount(), 2);
    }

    void planExtractsSdhWhenNoPlainExists()
    {
        MkvTrack sdh = subtitles(4, "eng");
        sdh.isHearingImpaired = true;
        MkvTrack sdh2 = subtitles(5, "eng");
        sdh2.isHearingImpaired = true;
        MkvTrack forced = subtitles(6, "eng");
        forced.isForced = true;
        const auto plan = Mkv::makePlan({audio(1, "eng", true), sdh, sdh2, forced});
        QCOMPARE(plan.extractSubtitleIds, QList<int>({4, 5, 6}));
        QCOMPARE(plan.removedCount(), 0);
    }

    void subtitleExtensions()
    {
        QCOMPARE(Mkv::subtitleExtension("S_TEXT/UTF8"), QString("srt"));
        QCOMPARE(Mkv::subtitleExtension("S_TEXT/ASS"), QString("ass"));
        QCOMPARE(Mkv::subtitleExtension("S_TEXT/WEBVTT"), QString("vtt"));
        QCOMPARE(Mkv::subtitleExtension("S_HDMV/PGS"), QString("sup"));
        QCOMPARE(Mkv::subtitleExtension("S_VOBSUB"), QString("sub"));
        QCOMPARE(Mkv::subtitleExtension("S_HDMV/TEXTST"), QString("textst"));
    }

    void classifiesSubtitles()
    {
        MkvTrack track = subtitles(1, "eng");
        QCOMPARE(Mkv::subtitleClass(track), MkvSubtitleClass::Plain);

        track.isForced = true;
        QCOMPARE(Mkv::subtitleClass(track), MkvSubtitleClass::Forced);

        // Forced wins even when the hearing-impaired flag is also set.
        track.isHearingImpaired = true;
        QCOMPARE(Mkv::subtitleClass(track), MkvSubtitleClass::Forced);

        track = subtitles(2, "eng");
        track.isHearingImpaired = true;
        QCOMPARE(Mkv::subtitleClass(track), MkvSubtitleClass::Sdh);

        // No flag, but the name says it — how releases actually tag SDH.
        for (const QString &name : {QString("English SDH"), QString("English [CC]"),
                                    QString("Hearing Impaired"), QString("cc")}) {
            track = subtitles(3, "eng");
            track.name = name;
            QCOMPARE(Mkv::subtitleClass(track), MkvSubtitleClass::Sdh);
        }

        track = subtitles(4, "eng");
        track.name = "English";
        QCOMPARE(Mkv::subtitleClass(track), MkvSubtitleClass::Plain);
    }

    void subtitleFileNames()
    {
        MkvTrack plain = subtitles(3, "eng"), forced = subtitles(4, "eng"),
                 second = subtitles(5, "eng"), sdh = subtitles(6, "eng"), sdhSecond = subtitles(7, "eng");
        forced.isForced = true;
        sdh.isHearingImpaired = true;
        sdhSecond.isHearingImpaired = true;
        QCOMPARE(Mkv::subtitleFileName("/movies/My Movie.2020.mkv", plain, 0),
                 QString("/movies/My Movie.2020.en.srt"));
        QCOMPARE(Mkv::subtitleFileName("/movies/My Movie.2020.mkv", forced, 0),
                 QString("/movies/My Movie.2020.forced.en.srt"));
        QCOMPARE(Mkv::subtitleFileName("/movies/My Movie.2020.mkv", second, 1),
                 QString("/movies/My Movie.2020.2.en.srt"));
        QCOMPARE(Mkv::subtitleFileName("/movies/My Movie.2020.mkv", sdh, 0),
                 QString("/movies/My Movie.2020.sdh.en.srt"));
        QCOMPARE(Mkv::subtitleFileName("/movies/My Movie.2020.mkv", sdhSecond, 1),
                 QString("/movies/My Movie.2020.sdh.2.en.srt"));
        // A converted SDH track is named like a plain subtitle.
        QCOMPARE(Mkv::subtitleFileName("/movies/My Movie.2020.mkv", sdh, 0, "eng", true),
                 QString("/movies/My Movie.2020.en.srt"));
        QCOMPARE(Mkv::subtitleFileName("/movies/My Movie.2020.mkv", sdhSecond, 1, "eng", true),
                 QString("/movies/My Movie.2020.2.en.srt"));
    }

    void sdhRemovesHearingImpairedText()
    {
        const QString input =
            "1\n00:00:01,000 --> 00:00:02,000\n[DOOR CLOSES]\n\n"
            "2\n00:00:02,500 --> 00:00:04,000\nJOHN: Hello there.\n\n"
            "3\n00:00:04,500 --> 00:00:06,000\n(whispering) I can't hear you.\n\n"
            "4\n00:00:06,000 --> 00:00:07,000\n♪ Music playing ♪\n\n"
            "5\n00:00:07,500 --> 00:00:10,000\n- MAN: [LAUGHS] Run!\n";
        const auto result = Sdh::cleanSrt(input);
        QCOMPARE(result.cues, 5);
        QCOMPARE(result.kept, 4);
        QVERIFY(!result.text.contains("DOOR CLOSES"));
        QVERIFY(!result.text.contains("JOHN"));
        QVERIFY(!result.text.contains("whispering"));
        QVERIFY(!result.text.contains("LAUGHS"));
        QVERIFY(!result.text.contains("MAN:"));
        QVERIFY(result.text.contains("Hello there."));
        QVERIFY(result.text.contains("I can't hear you."));
        QVERIFY(result.text.contains("Music playing"));
        QVERIFY(result.text.contains("Run!"));
        // Cues are renumbered without gaps; their timings are untouched.
        QVERIFY(result.text.startsWith("1\n00:00:02,500 --> 00:00:04,000\n"));
        QVERIFY(result.text.contains("\n\n2\n00:00:04,500"));
        QVERIFY(result.text.contains("\n\n3\n00:00:06,000"));
        QVERIFY(result.text.contains("\n\n4\n00:00:07,500"));
    }

    void sdhLeavesPlainDialogueAlone()
    {
        const QString input =
            "1\n00:00:01,000 --> 00:00:02,000\nHe said: come in.\n\n"
            "2\n00:00:02,500 --> 00:00:04,000\nI saw it yesterday.\n";
        const auto result = Sdh::cleanSrt(input);
        QCOMPARE(result.cues, 2);
        QCOMPARE(result.kept, 2);
        QCOMPARE(result.text, input);
    }

    void sdhKeepsTheLineEndingStyle()
    {
        const QString input =
            QChar(0xFEFF)
            + QString("1\r\n00:00:01,000 --> 00:00:02,000\r\n[DOOR CLOSES]\r\n\r\n"
                      "2\r\n00:00:02,500 --> 00:00:04,000\r\nHello.\r\n");
        const auto result = Sdh::cleanSrt(input);
        QCOMPARE(result.kept, 1);
        QVERIFY(result.text.startsWith(QChar(0xFEFF) + QString("1\r\n")));
        QVERIFY(result.text.contains("Hello.\r\n"));
        QVERIFY(!result.text.contains("\n\n"));
    }

    void availableNamesAvoidExistingFiles()
    {
        QTemporaryDir dir;
        QCOMPARE(Mkv::availablePath(dir.filePath("Movie.en.srt")), dir.filePath("Movie.en.srt"));
        write(dir.filePath("Movie.en.srt"), "taken");
        QCOMPARE(Mkv::availablePath(dir.filePath("Movie.en.srt")), dir.filePath("Movie.en-2.srt"));
        write(dir.filePath("Movie.en-2.srt"), "taken");
        QCOMPARE(Mkv::availablePath(dir.filePath("Movie.en.srt")), dir.filePath("Movie.en-3.srt"));
    }

    void remuxKeepsOnlyThePlannedAudio()
    {
        MkvTrack video;
        video.id = 0;
        video.type = "video";
        const auto plan = Mkv::makePlan({video, audio(7, "eng", true), audio(8, "dan", false)});
        const auto arguments = Mkv::remuxArguments("/in.mkv", "/out.mkv", plan);
        QVERIFY(arguments.contains("--no-subtitles"));
        QVERIFY(arguments.contains("--audio-tracks"));
        QCOMPARE(arguments.at(arguments.indexOf("--audio-tracks") + 1), QString("7"));
        // Track names are blanked on the survivors: the video and kept audio.
        QCOMPARE(arguments.count("--track-name"), 2);
        QVERIFY(arguments.contains("0:"));
        QVERIFY(arguments.contains("7:"));
        QCOMPARE(arguments.last(), QString("/in.mkv"));

        // A file with no audio at all remuxes video-only.
        const auto silent = Mkv::remuxArguments("/in.mkv", "/out.mkv", Mkv::makePlan({video}));
        QVERIFY(silent.contains("--no-audio"));
        QVERIFY(!silent.contains("--audio-tracks"));
    }

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeReadsAccent()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml", "accent = \"#112233\"\n");

        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void themeFollowsHyprlandRounding()
    {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":0}");

        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        write(response, "{\"int\":6}");
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorFollowsTheLanguageFile()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "pt-BR\n");
        write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})");
        write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }

    void translatorPicksPluralForms()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "ru\n");
        write(dir.path() + "/en.json", R"({"%n clips": {"one": "%n clip", "other": "%n clips"}})");
        write(dir.path() + "/ru.json", R"({"%n clips": {"one": "%n клип", "few": "%n клипа", "many": "%n клипов", "other": "%n клипа"}})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 21), QString("%n клип"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 22), QString("%n клипа"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 25), QString("%n клипов"));
        QCOMPARE(Translator::pluralCategory("ar", 11), QString("many"));
        QCOMPARE(Translator::pluralCategory("ja", 1), QString("other"));
    }

    void backendRejectsNonMkvFiles()
    {
        // Dummy tool paths: the suffix check must fail before either runs.
        Backend backend(nullptr, nullptr, "/bin/true", "/bin/true");
        QTemporaryDir dir;
        write(dir.filePath("notes.txt"), "hello");
        backend.openPath(dir.filePath("notes.txt"));
        QCOMPARE(backend.state(), QString("error"));
        QCOMPARE(backend.errorText(), QString("This is not an MKV file."));
    }

    void backendReportsMissingTools()
    {
        Backend backend(nullptr, nullptr, "/nonexistent/mkvmerge", "/nonexistent/mkvextract");
        QCOMPARE(backend.state(), QString("error"));
        QVERIFY(backend.errorText().contains("MKVToolNix"));
    }

    void collectsMkvFilesFromFolders()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("Season 1");
        QDir(dir.path()).mkpath("Season 2/extra");
        write(dir.filePath("Season 1/e10.mkv"), "x");
        write(dir.filePath("Season 1/e02.mkv"), "x");
        write(dir.filePath("Season 2/extra/e01.MKV"), "x");
        write(dir.filePath("notes.txt"), "x");

        const auto files = Mkv::collectMkvFiles({dir.path(), dir.filePath("notes.txt")});
        QCOMPARE(files.size(), 3);
        // Full-path natural sort: Season 1 before Season 2, e02 before e10.
        QVERIFY(files.at(0).endsWith("Season 1/e02.mkv"));
        QVERIFY(files.at(1).endsWith("Season 1/e10.mkv"));
        QVERIFY(files.at(2).endsWith("Season 2/extra/e01.MKV"));

        // A file passed both directly and via its folder is collected once;
        // a non-MKV on its own collects nothing.
        QCOMPARE(Mkv::collectMkvFiles({dir.path(), dir.filePath("Season 1/e02.mkv")}).size(), 3);
        QCOMPARE(Mkv::collectMkvFiles({dir.filePath("notes.txt")}).size(), 0);
    }

    void backendExtractsAndCleansARealFile()
    {
        const QString mkvmerge = QStandardPaths::findExecutable("mkvmerge");
        const QString mkvextract = QStandardPaths::findExecutable("mkvextract");
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        if (mkvmerge.isEmpty() || mkvextract.isEmpty() || ffmpeg.isEmpty())
            QSKIP("needs ffmpeg and MKVToolNix");

        QTemporaryDir dir;
        const QString sample = makeSampleMkv(dir.path(), ffmpeg, mkvmerge);
        QVERIFY2(!sample.isEmpty(), "could not build the sample MKV");

        Backend backend(nullptr, nullptr, mkvmerge, mkvextract);
        backend.openPath(sample);
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), QString("ready"), 15000);
        QCOMPARE(backend.extractCount(), 1);
        // The Danish audio is the only track that vanishes without extraction.
        QCOMPARE(backend.removedCount(), 1);

        backend.clean();
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), QString("done"), 30000);

        // English subtitles landed next to the original with the content intact.
        QCOMPARE(backend.extractedFiles().size(), 1);
        const QString srt = backend.extractedFiles().first();
        QVERIFY(srt.endsWith(".en.srt"));
        QFile srtFile(srt);
        QVERIFY(srtFile.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(srtFile.readAll()).contains("Hello from the English track"));

        // The cleaned file replaced the original in place, keeping the video
        // and the English audio, nothing else; no temp or ".english" strays.
        QCOMPARE(backend.outputPath(), sample);
        QVERIFY(!QFile::exists(sample + ".remux"));
        QVERIFY(!QFile::exists(dir.filePath("sample.english.mkv")));
        const auto kept = Mkv::parseTracks(inspect(mkvmerge, sample));
        QCOMPARE(kept.size(), 2);
        QCOMPARE(kept.at(0).type, QString("video"));
        QCOMPARE(kept.at(1).type, QString("audio"));
        QCOMPARE(kept.at(1).language, QString("eng"));
        // Leftover track names are stripped along the way.
        QVERIFY(kept.at(0).name.isEmpty());
        QVERIFY(kept.at(1).name.isEmpty());
    }

    void backendConvertsSdhToStandardSubtitles()
    {
        const QString mkvmerge = QStandardPaths::findExecutable("mkvmerge");
        const QString mkvextract = QStandardPaths::findExecutable("mkvextract");
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        if (mkvmerge.isEmpty() || mkvextract.isEmpty() || ffmpeg.isEmpty())
            QSKIP("needs ffmpeg and MKVToolNix");

        QTemporaryDir dir;
        const QString sample = makeSdhMkv(dir.path(), ffmpeg, mkvmerge);
        QVERIFY2(!sample.isEmpty(), "could not build the SDH sample MKV");

        AppSettings settings(dir.filePath("config.json"), dir.filePath("no-legacy"));
        settings.setConvertSdh(true);
        Backend backend(nullptr, nullptr, mkvmerge, mkvextract);
        backend.setSettings(&settings);
        backend.openPath(sample);
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), QString("ready"), 15000);
        backend.clean();
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), QString("done"), 30000);

        QCOMPARE(backend.extractedFiles().size(), 1);
        const QString srt = backend.extractedFiles().first();
        QVERIFY2(srt.endsWith(".en.srt"), qPrintable(srt));
        // The SDH-only copy is gone; the standard file has the dialogue only.
        QVERIFY(!QFile::exists(dir.filePath("sdh.sdh.en.srt")));
        QFile file(srt);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString contents = QString::fromUtf8(file.readAll());
        QVERIFY(contents.contains("Hello from the SDH track"));
        QVERIFY(!contents.contains("DOOR CLOSES"));
    }

    void backendKeepsSdhWhenConversionIsOff()
    {
        const QString mkvmerge = QStandardPaths::findExecutable("mkvmerge");
        const QString mkvextract = QStandardPaths::findExecutable("mkvextract");
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        if (mkvmerge.isEmpty() || mkvextract.isEmpty() || ffmpeg.isEmpty())
            QSKIP("needs ffmpeg and MKVToolNix");

        QTemporaryDir dir;
        const QString sample = makeSdhMkv(dir.path(), ffmpeg, mkvmerge, "kept.mkv");
        QVERIFY2(!sample.isEmpty(), "could not build the SDH sample MKV");

        AppSettings settings(dir.filePath("config.json"), dir.filePath("no-legacy"));
        QVERIFY(!settings.convertSdh());
        Backend backend(nullptr, nullptr, mkvmerge, mkvextract);
        backend.setSettings(&settings);
        backend.openPath(sample);
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), QString("ready"), 15000);
        backend.clean();
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), QString("done"), 30000);

        QCOMPARE(backend.extractedFiles().size(), 1);
        const QString srt = backend.extractedFiles().first();
        QVERIFY2(srt.endsWith(".sdh.en.srt"), qPrintable(srt));
        QFile file(srt);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString contents = QString::fromUtf8(file.readAll());
        QVERIFY(contents.contains("[DOOR CLOSES]"));
        QVERIFY(contents.contains("Hello from the SDH track"));
    }

    void backendCleansABatch()
    {
        const QString mkvmerge = QStandardPaths::findExecutable("mkvmerge");
        const QString mkvextract = QStandardPaths::findExecutable("mkvextract");
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        if (mkvmerge.isEmpty() || mkvextract.isEmpty() || ffmpeg.isEmpty())
            QSKIP("needs ffmpeg and MKVToolNix");

        QTemporaryDir dir;
        const QString first = makeSampleMkv(dir.path(), ffmpeg, mkvmerge, "a-first.mkv");
        const QString second = makeSampleMkv(dir.path(), ffmpeg, mkvmerge, "b-second.mkv");
        QVERIFY2(!first.isEmpty() && !second.isEmpty(), "could not build the sample MKVs");

        Backend backend(nullptr, nullptr, mkvmerge, mkvextract);
        backend.openPaths({second, first});  // the sort puts a-first first
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), QString("done"), 60000);

        QVERIFY(backend.batch());
        QVERIFY(!backend.batchAborted());
        QCOMPARE(backend.queueCleanedCount(), 2);
        QCOMPARE(backend.queueFailedCount(), 0);
        QCOMPARE(Mkv::parseTracks(inspect(mkvmerge, first)).size(), 2);
        QCOMPARE(Mkv::parseTracks(inspect(mkvmerge, second)).size(), 2);
        QVERIFY(QFile::exists(dir.filePath("a-first.en.srt")));
        QVERIFY(QFile::exists(dir.filePath("b-second.en.srt")));
    }

    void backendBatchContinuesPastFailures()
    {
        const QString mkvmerge = QStandardPaths::findExecutable("mkvmerge");
        const QString mkvextract = QStandardPaths::findExecutable("mkvextract");
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        if (mkvmerge.isEmpty() || mkvextract.isEmpty() || ffmpeg.isEmpty())
            QSKIP("needs ffmpeg and MKVToolNix");

        QTemporaryDir dir;
        // The failure is a corrupt file; it must not stop the batch.
        const QString bad = dir.filePath("a-bad.mkv");
        write(bad, "this is not really a matroska file");
        const QString good = makeSampleMkv(dir.path(), ffmpeg, mkvmerge, "b-good.mkv");
        QVERIFY2(!good.isEmpty(), "could not build the sample MKV");

        Backend backend(nullptr, nullptr, mkvmerge, mkvextract);
        backend.openPaths({good, bad});
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), QString("done"), 60000);

        QCOMPARE(backend.queueCleanedCount(), 1);
        QCOMPARE(backend.queueFailedCount(), 1);
        QCOMPARE(backend.queue().at(0).toMap().value("error").toString(),
                 QString("This file could not be read."));
        // The failed file was not modified; the good one was cleaned.
        QFile badFile(bad);
        QVERIFY(badFile.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(badFile.readAll()), QString("this is not really a matroska file"));
        QCOMPARE(Mkv::parseTracks(inspect(mkvmerge, good)).size(), 2);
    }

    void backendKeepsForeignAudioWhenNoEnglishExists()
    {
        const QString mkvmerge = QStandardPaths::findExecutable("mkvmerge");
        const QString mkvextract = QStandardPaths::findExecutable("mkvextract");
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        if (mkvmerge.isEmpty() || mkvextract.isEmpty() || ffmpeg.isEmpty())
            QSKIP("needs ffmpeg and MKVToolNix");

        QTemporaryDir dir;
        // Video + Danish audio only + English subtitles: a foreign film.
        const QString sample = makeSampleMkv(dir.path(), ffmpeg, mkvmerge, "foreign.mkv", false);
        QVERIFY2(!sample.isEmpty(), "could not build the sample MKV");

        Backend backend(nullptr, nullptr, mkvmerge, mkvextract);
        backend.openPath(sample);
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), QString("ready"), 15000);
        QCOMPARE(backend.extractCount(), 1);

        backend.clean();
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), QString("done"), 30000);

        // The foreign audio survived, the subtitles came out, nothing else.
        const auto kept = Mkv::parseTracks(inspect(mkvmerge, sample));
        QCOMPARE(kept.size(), 2);
        QCOMPARE(kept.at(0).type, QString("video"));
        QCOMPARE(kept.at(1).type, QString("audio"));
        QCOMPARE(kept.at(1).language, QString("dan"));
        QVERIFY(QFile::exists(dir.filePath("foreign.en.srt")));
    }

    void backendSkipsSdhWhenPlainExists()
    {
        const QString mkvmerge = QStandardPaths::findExecutable("mkvmerge");
        const QString mkvextract = QStandardPaths::findExecutable("mkvextract");
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        if (mkvmerge.isEmpty() || mkvextract.isEmpty() || ffmpeg.isEmpty())
            QSKIP("needs ffmpeg and MKVToolNix");

        QTemporaryDir dir;
        // Video + default English audio + three English subtitles: plain, SDH
        // (flagged), and forced — each with its own content to tell them apart.
        const QString sample = makeSampleMkv(dir.path(), ffmpeg, mkvmerge, "rich.mkv");
        QVERIFY2(!sample.isEmpty(), "could not build the sample MKV");
        const QString plainSrt = dir.filePath("plain.srt"), sdhSrt = dir.filePath("sdh.srt"),
                      forcedSrt = dir.filePath("forced.srt"), rich = dir.filePath("rich-subs.mkv");
        write(plainSrt, "1\n00:00:00,000 --> 00:00:01,500\nPlain line\n");
        write(sdhSrt, "1\n00:00:00,000 --> 00:00:01,500\n[sdh line]\n");
        write(forcedSrt, "1\n00:00:00,000 --> 00:00:01,500\nForced line\n");
        QVERIFY(QProcess::execute(mkvmerge, {"-q", "-o", rich, sample,
                                             "--language", "0:eng", "--track-name", "0:English", plainSrt,
                                             "--language", "0:eng", "--hearing-impaired-flag", "0:yes",
                                             "--track-name", "0:English SDH", sdhSrt,
                                             "--language", "0:eng", "--forced-display-flag", "0:yes",
                                             "--track-name", "0:English forced", forcedSrt}) == 0);

        Backend backend(nullptr, nullptr, mkvmerge, mkvextract);
        backend.openPath(rich);
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), QString("ready"), 15000);
        // Two plain (the sample's own plus plain.srt) plus forced; the SDH is
        // skipped because plain tracks exist.
        QCOMPARE(backend.extractCount(), 3);

        backend.clean();
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), QString("done"), 30000);

        QCOMPARE(backend.extractedFiles().size(), 3);
        const auto read = [](const QString &path) {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
        };
        const QString base = dir.filePath("rich-subs");
        QVERIFY(backend.extractedFiles().contains(base + ".en.srt"));
        QVERIFY(backend.extractedFiles().contains(base + ".2.en.srt"));
        QVERIFY(backend.extractedFiles().contains(base + ".forced.en.srt"));
        QVERIFY(!QFile::exists(base + ".sdh.en.srt"));
        QVERIFY(read(base + ".en.srt").contains("Hello from the English track"));
        QVERIFY(read(base + ".2.en.srt").contains("Plain line"));
        QVERIFY(read(base + ".forced.en.srt").contains("Forced line"));
    }

    void backendExtractsSdhWhenNoPlainExists()
    {
        const QString mkvmerge = QStandardPaths::findExecutable("mkvmerge");
        const QString mkvextract = QStandardPaths::findExecutable("mkvextract");
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        if (mkvmerge.isEmpty() || mkvextract.isEmpty() || ffmpeg.isEmpty())
            QSKIP("needs ffmpeg and MKVToolNix");

        QTemporaryDir dir;
        // Reuse the sample builder for its streams, then mux an SDH-only file:
        // video + default English audio + one SDH subtitle, no plain.
        QVERIFY2(!makeSampleMkv(dir.path(), ffmpeg, mkvmerge, "assets.mkv").isEmpty(),
                 "could not build the sample streams");
        const QString sdhSrt = dir.filePath("sdh.srt"), sdhOnly = dir.filePath("sdh-only.mkv");
        write(sdhSrt, "1\n00:00:00,000 --> 00:00:01,500\n[sdh line]\n");
        // No track name: detection must come from the flag alone.
        QVERIFY(QProcess::execute(mkvmerge, {"-q", "-o", sdhOnly, dir.filePath("video.mkv"),
                                             "--language", "0:eng", "--default-track-flag", "0:yes",
                                             dir.filePath("eng.wav"),
                                             "--language", "0:eng", "--hearing-impaired-flag", "0:yes",
                                             sdhSrt}) == 0);

        Backend backend(nullptr, nullptr, mkvmerge, mkvextract);
        backend.openPath(sdhOnly);
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), QString("ready"), 15000);
        QCOMPARE(backend.extractCount(), 1);

        backend.clean();
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), QString("done"), 30000);

        QCOMPARE(backend.extractedFiles(), QStringList({dir.filePath("sdh-only.sdh.en.srt")}));
        QFile extracted(dir.filePath("sdh-only.sdh.en.srt"));
        QVERIFY(extracted.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(extracted.readAll()).contains("[sdh line]"));
    }

    void replaceOriginalSwapsAndKeepsOnFailure()
    {
        QTemporaryDir dir;
        const QString original = dir.filePath("Movie.mkv"), remux = original + ".remux";

        write(original, "old");
        write(remux, "new");
        QVERIFY(Mkv::replaceOriginal(original, remux));
        QFile swapped(original);
        QVERIFY(swapped.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(swapped.readAll()), QString("new"));
        QVERIFY(!QFile::exists(remux));

        // A read-only directory blocks the swap; the verified remux survives
        // for a later retry, and the original is untouched.
        write(original, "old2");
        write(remux, "new2");
        QVERIFY(QFile::setPermissions(dir.path(), QFile::ReadOwner | QFile::ExeOwner));
        QVERIFY(!Mkv::replaceOriginal(original, remux));
        QVERIFY(QFile::exists(remux));
        QVERIFY(QFile::exists(original));
        // A refused swap must not disturb the original's contents either.
        QFile untouched(original);
        QVERIFY(untouched.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(untouched.readAll()), QString("old2"));
        untouched.close();
        QVERIFY(QFile::setPermissions(dir.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }

    void remuxNeverWritesTheRemuxName()
    {
        const QString mkvmerge = QStandardPaths::findExecutable("mkvmerge");
        const QString mkvextract = QStandardPaths::findExecutable("mkvextract");
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        if (mkvmerge.isEmpty() || mkvextract.isEmpty() || ffmpeg.isEmpty())
            QSKIP("needs ffmpeg and MKVToolNix");

        QTemporaryDir dir;
        // A real release name: brackets and parentheses must not trip the
        // temp-cleanup matcher.
        const QString sample = makeSampleMkv(
            dir.path(), ffmpeg, mkvmerge,
            "Movie (2026) [WEBRip-1080p] [x265] [NeoNoir].mkv");
        QVERIFY2(!sample.isEmpty(), "could not build the sample MKV");

        // A temp left by an interrupted run; it must be swept when this clean
        // starts (it is already older than the one-hour idle threshold).
        const QString staleTemp = sample + ".mkvclean-999999.tmp";
        write(staleTemp, "stale partial");
        QFile staleFile(staleTemp);
        QVERIFY(staleFile.open(QIODevice::ReadWrite));
        QVERIFY(staleFile.setFileTime(QDateTime::currentDateTime().addSecs(-7200),
                                      QFileDevice::FileModificationTime));
        staleFile.close();

        // Wrap mkvmerge so the remux output name can be inspected. It must be
        // a private temp ending in ".tmp", never "*.remux": the watcher's
        // sweep retries every "*.remux", and if it caught a half-written one
        // it would swap it in, after which the finishing job would unlink it
        // and both files would be gone.
        const QString marker = dir.filePath("outputs.log");
        const QString wrapper = dir.filePath("mkvmerge-wrapper.sh");
        write(wrapper, QString("#!/bin/sh\n"
                               "if [ \"$1\" = \"-J\" ]; then exec '%1' \"$@\"; fi\n"
                               "prev=\"\"\n"
                               "for a in \"$@\"; do\n"
                               "  if [ \"$prev\" = \"-o\" ]; then out=\"$a\"; fi\n"
                               "  prev=\"$a\"\n"
                               "done\n"
                               "printf '%s\\n' \"$out\" >> '%2'\n"
                               "exec '%1' \"$@\"\n")
                           .arg(mkvmerge, marker).toUtf8());
        QVERIFY(QFile::setPermissions(wrapper, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

        Backend backend(nullptr, nullptr, wrapper, mkvextract);
        backend.openPath(sample);
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), QString("ready"), 15000);
        backend.clean();
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), QString("done"), 30000);

        QFile logFile(marker);
        QVERIFY(logFile.open(QIODevice::ReadOnly));
        const QStringList outputs =
            QString::fromUtf8(logFile.readAll()).split('\n', Qt::SkipEmptyParts);
        QVERIFY(!outputs.isEmpty());
        for (const auto &output : outputs) {
            QVERIFY2(!output.endsWith(".remux"),
                     qPrintable("mkvmerge wrote directly to " + output));
            QVERIFY(output.endsWith(".tmp"));
        }
        // The swap still lands in place and leaves no remux or temp behind;
        // the stale temp from the interrupted run was swept.
        QVERIFY(QFile::exists(sample));
        QVERIFY(!QFile::exists(sample + ".remux"));
        QVERIFY(!QFile::exists(staleTemp));
        QCOMPARE(QDir(dir.path()).entryList({"*.tmp"}, QDir::Files).size(), 0);
    }

    void watcherCompletesPendingSwaps()
    {
        QTemporaryDir dir;
        const QString original = dir.filePath("Movie.mkv");
        write(original, "old contents");
        write(original + ".remux", "new contents");

        Watcher watcher({dir.path()}, 500, nullptr, "/bin/true", "/bin/true");
        QStringList log;
        connect(&watcher, &Watcher::logLine, this,
                [&log](const QString &line) { log.append(line); });
        watcher.start();

        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(original + ".remux"), 10000);
        QFile swapped(original);
        QVERIFY(swapped.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(swapped.readAll()), QString("new contents"));
        QVERIFY(log.contains("mkvclean: completed replacement of " + original));
    }

    void settingsRoundTrip()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("watched");
        const QString config = dir.filePath("nested/config.json");

        AppSettings settings(config, dir.filePath("no-legacy"));
        QCOMPARE(settings.audioLanguage(), QString("eng"));
        QCOMPARE(settings.subtitleLanguage(), QString("eng"));
        QVERIFY(settings.languageModel().size() > 30);

        settings.addWatchFolder(dir.filePath("watched"));
        settings.addWatchFolder("/nonexistent-folder");
        settings.addWatchFolder(dir.filePath("watched"));
        QCOMPARE(settings.watchFolders(), QStringList({dir.filePath("watched")}));

        settings.setAudioLanguage("dan");
        settings.setSubtitleLanguage("ger");
        settings.setAudioLanguage("not-a-code");  // rejected, keeps the old value
        // The settings panel calls these from QML, which needs Q_INVOKABLE.
        QVERIFY(QMetaObject::invokeMethod(&settings, "setConvertSdh", Q_ARG(bool, true)));
        QVERIFY(settings.convertSdh());

        AppSettings reloaded(config, dir.filePath("no-legacy"));
        QCOMPARE(reloaded.watchFolders(), QStringList({dir.filePath("watched")}));
        QCOMPARE(reloaded.audioLanguage(), QString("dan"));
        QCOMPARE(reloaded.subtitleLanguage(), QString("ger"));
        QVERIFY(reloaded.convertSdh());
        reloaded.setConvertSdh(false);
        QVERIFY(!reloaded.convertSdh());

        reloaded.removeWatchFolder(dir.filePath("watched"));
        QVERIFY(reloaded.watchFolders().isEmpty());

        // Excluded folders round-trip the same way.
        reloaded.addExcludedFolder(dir.filePath("watched"));
        reloaded.addExcludedFolder(dir.filePath("watched"));
        QCOMPARE(reloaded.excludedFolders(), QStringList({dir.filePath("watched")}));
        AppSettings reloadedAgain(config, dir.filePath("no-legacy"));
        QCOMPARE(reloadedAgain.excludedFolders(), QStringList({dir.filePath("watched")}));
        reloadedAgain.removeExcludedFolder(dir.filePath("watched"));
        QVERIFY(reloadedAgain.excludedFolders().isEmpty());
    }

    void settingsFollowExternalChanges()
    {
        QTemporaryDir dir;
        const QString config = dir.filePath("config.json");
        // The window and the watch service are separate processes in real use;
        // this stands in for both sides of the shared file.
        AppSettings window(config, dir.filePath("no-legacy"));
        AppSettings service(config, dir.filePath("no-legacy"));
        QVERIFY(!service.convertSdh());

        window.setConvertSdh(true);
        QTRY_VERIFY_WITH_TIMEOUT(service.convertSdh(), 5000);

        window.setAudioLanguage("dan");
        QTRY_COMPARE_WITH_TIMEOUT(service.audioLanguage(), QString("dan"), 5000);
    }

    void withoutExcludedDropsNestedPaths()
    {
        const QStringList paths{"/videos/Movies/a.mkv", "/videos/Music Videos/b.mkv",
                                "/videos/Music/c.mkv", "/videos/Movies/d.mkv"};
        const auto kept = Mkv::withoutExcluded(paths, {"/videos/Music Videos"});
        QCOMPARE(kept, QStringList({"/videos/Movies/a.mkv", "/videos/Music/c.mkv",
                                    "/videos/Movies/d.mkv"}));
        // A sibling whose name merely starts the same stays.
        QVERIFY(Mkv::withoutExcluded({"/videos/Music/a.mkv"}, {"/videos/Music Videos"})
                == QStringList({"/videos/Music/a.mkv"}));
        QVERIFY(Mkv::withoutExcluded({"/videos/Music Videos"}, {"/videos/Music Videos"}).isEmpty());
        QCOMPARE(Mkv::withoutExcluded(paths, {}), paths);
    }

    void settingsImportsLegacyWatchFolders()
    {
        QTemporaryDir dir;
        const QString legacy = dir.filePath("watch-folders");
        write(legacy, "# my folders\n/videos/tv\n\n/videos/movies\n");

        AppSettings settings(dir.filePath("config.json"), legacy);
        QCOMPARE(settings.watchFolders(), QStringList({"/videos/tv", "/videos/movies"}));
        // The import is saved to the new config straight away.
        QVERIFY(QFile::exists(dir.filePath("config.json")));
    }

    void languageSuffixes()
    {
        QCOMPARE(Mkv::languageSuffix("eng"), QString("en"));
        QCOMPARE(Mkv::languageSuffix("dan"), QString("da"));
        QCOMPARE(Mkv::languageSuffix("ger"), QString("de"));
        QCOMPARE(Mkv::languageSuffix("zho"), QString("zho"));
    }

    void planHonorsPreferredLanguages()
    {
        MkvTrack danishSub = subtitles(4, "dan");
        const auto plan = Mkv::makePlan({audio(1, "dan", true), audio(2, "eng", false),
                                         subtitles(3, "eng"), danishSub},
                                        QStringLiteral("dan"), QStringLiteral("dan"));
        QCOMPARE(plan.keepAudioId, 1);
        QCOMPARE(plan.extractSubtitleIds, QList<int>({4}));
    }

    void subtitleFileNamesFollowTheLanguage()
    {
        MkvTrack track = subtitles(3, "dan");
        QCOMPARE(Mkv::subtitleFileName("/movies/Movie.mkv", track, 0, "dan"),
                 QString("/movies/Movie.da.srt"));
        QCOMPARE(Mkv::subtitleFileName("/movies/Movie.mkv", track, 0, "ger"),
                 QString("/movies/Movie.de.srt"));
    }

    void watcherProcessesNewFilesOnly()
    {
        const QString mkvmerge = QStandardPaths::findExecutable("mkvmerge");
        const QString mkvextract = QStandardPaths::findExecutable("mkvextract");
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        if (mkvmerge.isEmpty() || mkvextract.isEmpty() || ffmpeg.isEmpty())
            QSKIP("needs ffmpeg and MKVToolNix");

        QTemporaryDir dir;
        // Present before the watcher starts: must be snapshotted, never cleaned.
        QVERIFY2(!makeSampleMkv(dir.path(), ffmpeg, mkvmerge, "old.mkv").isEmpty(),
                 "could not build the sample MKV");

        Watcher watcher({dir.path()}, 500, nullptr, mkvmerge, mkvextract);
        QStringList log;
        connect(&watcher, &Watcher::logLine, this,
                [&log](const QString &line) { log.append(line); });
        watcher.start();

        // Arriving after startup: settled, then cleaned.
        const QString fresh = makeSampleMkv(dir.path(), ffmpeg, mkvmerge, "new.mkv");
        QVERIFY2(!fresh.isEmpty(), "could not build the second sample MKV");

        QTRY_VERIFY_WITH_TIMEOUT(Mkv::parseTracks(inspect(mkvmerge, fresh)).size() == 2, 30000);
        QVERIFY(QFile::exists(dir.filePath("new.en.srt")));
        QVERIFY(log.contains("mkvclean: cleaned " + fresh));

        // The pre-existing file kept all of its tracks and its subtitles.
        QCOMPARE(Mkv::parseTracks(inspect(mkvmerge, dir.filePath("old.mkv"))).size(), 4);
        QVERIFY(!QFile::exists(dir.filePath("old.en.srt")));
    }

    void backendScanCleansOnlyDirtyFiles()
    {
        const QString mkvmerge = QStandardPaths::findExecutable("mkvmerge");
        const QString mkvextract = QStandardPaths::findExecutable("mkvextract");
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        if (mkvmerge.isEmpty() || mkvextract.isEmpty() || ffmpeg.isEmpty())
            QSKIP("needs ffmpeg and MKVToolNix");

        QTemporaryDir dir;
        const QString dirty = makeSampleMkv(dir.path(), ffmpeg, mkvmerge, "dirty.mkv");
        QVERIFY2(!dirty.isEmpty(), "could not build the sample MKV");
        // A file that is already clean: video plus one audio, nothing else.
        const QString clean = dir.filePath("clean.mkv");
        QVERIFY(QProcess::execute(mkvmerge, {"-q", "-o", clean, dir.filePath("video.mkv"),
                                             "--language", "0:eng", "--default-track-flag", "0:yes",
                                             dir.filePath("eng.wav")}) == 0);

        Backend backend(nullptr, nullptr, mkvmerge, mkvextract);
        backend.scanFolders({dir.path()});
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), QString("done"), 60000);

        QVERIFY(!backend.scanFoundNothing());
        QCOMPARE(backend.queueCleanedCount(), 1);
        QCOMPARE(Mkv::parseTracks(inspect(mkvmerge, dirty)).size(), 2);
        // The already-clean file was not probed a second time.
        QCOMPARE(Mkv::parseTracks(inspect(mkvmerge, clean)).size(), 2);
        QVERIFY(!QFile::exists(dir.filePath("clean.en.srt")));
    }

    void watcherSkipsExcludedFolders()
    {
        const QString mkvmerge = QStandardPaths::findExecutable("mkvmerge");
        const QString mkvextract = QStandardPaths::findExecutable("mkvextract");
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        if (mkvmerge.isEmpty() || mkvextract.isEmpty() || ffmpeg.isEmpty())
            QSKIP("needs ffmpeg and MKVToolNix");

        QTemporaryDir dir;
        QDir(dir.path()).mkpath("Music Videos");
        AppSettings settings(dir.filePath("config.json"), dir.filePath("no-legacy"));
        settings.addExcludedFolder(dir.filePath("Music Videos"));

        Watcher watcher({dir.path()}, 500, nullptr, mkvmerge, mkvextract, &settings);
        watcher.start();

        // One file in the excluded folder, one next to it: only the latter
        // may be processed, and the excluded one must not even be settled.
        QVERIFY2(!makeSampleMkv(dir.filePath("Music Videos"), ffmpeg, mkvmerge, "mv.mkv").isEmpty(),
                 "could not build the sample MKV");
        const QString kept = makeSampleMkv(dir.path(), ffmpeg, mkvmerge, "movie.mkv");
        QVERIFY2(!kept.isEmpty(), "could not build the second sample MKV");

        QTRY_VERIFY_WITH_TIMEOUT(Mkv::parseTracks(inspect(mkvmerge, kept)).size() == 2, 30000);
        QTest::qWait(1500);  // long enough for the excluded file's settle, had it armed
        QCOMPARE(Mkv::parseTracks(inspect(mkvmerge, dir.filePath("Music Videos/mv.mkv"))).size(), 4);
        QVERIFY(!QFile::exists(dir.filePath("Music Videos/mv.en.srt")));
    }

    void backendInspectTimeoutReportsUnreadable()
    {
        QTemporaryDir dir;
        write(dir.filePath("movie.mkv"), "");
        // A "mkvmerge" that hangs forever: the timeout must kill the probe.
        Backend backend(nullptr, nullptr, makeSleepTool(dir.path()), "/bin/true", 200);
        backend.openPath(dir.filePath("movie.mkv"));
        QTRY_COMPARE(backend.state(), QString("error"));
        QCOMPARE(backend.errorText(), QString("This file could not be read."));
    }

    void backendScanTimeoutSkipsHungFiles()
    {
        QTemporaryDir dir;
        write(dir.filePath("movie.mkv"), "");
        Backend backend(nullptr, nullptr, makeSleepTool(dir.path()), "/bin/true", 200);
        backend.scanFolders({dir.path()});
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), QString("done"), 15000);
        QVERIFY(backend.scanFoundNothing());
    }

    void backendScanCancelStopsImmediately()
    {
        QTemporaryDir dir;
        write(dir.filePath("movie.mkv"), "");
        Backend backend(nullptr, nullptr, makeSleepTool(dir.path()), "/bin/true", 30000);
        backend.scanFolders({dir.path()});
        QVERIFY(backend.scanning());
        backend.cancel();
        QTRY_COMPARE(backend.state(), QString("idle"));
        QVERIFY(!backend.scanning());
    }

    void backendScanIsBusyUntilItEnds()
    {
        QTemporaryDir dir;
        write(dir.filePath("a.mkv"), "");
        write(dir.filePath("b.mkv"), "");
        Backend backend(nullptr, nullptr, makeSleepTool(dir.path()), "/bin/true", 30000);
        backend.scanFolders({dir.path()});
        QVERIFY(backend.scanning());
        QCOMPARE(backend.scanTotal(), 2);

        // A running scan counts as busy: reset must not tear it down, and a
        // second scan must not stack another probe pool over the first.
        backend.reset();
        QVERIFY(backend.scanning());
        QCOMPARE(backend.scanTotal(), 2);
        backend.scanFolders({dir.path()});
        QVERIFY(backend.scanning());
        QCOMPARE(backend.scanTotal(), 2);

        // Cancel is still the way out.
        backend.cancel();
        QTRY_COMPARE(backend.state(), QString("idle"));
        QVERIFY(!backend.scanning());
    }

    void backendScanReviewTrimsTheList()
    {
        const QString mkvmerge = QStandardPaths::findExecutable("mkvmerge");
        const QString mkvextract = QStandardPaths::findExecutable("mkvextract");
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        if (mkvmerge.isEmpty() || mkvextract.isEmpty() || ffmpeg.isEmpty())
            QSKIP("needs ffmpeg and MKVToolNix");

        QTemporaryDir dir;
        const QString keep = makeSampleMkv(dir.path(), ffmpeg, mkvmerge, "a-keep.mkv");
        const QString clean = makeSampleMkv(dir.path(), ffmpeg, mkvmerge, "b-clean.mkv");
        QVERIFY2(!keep.isEmpty() && !clean.isEmpty(), "could not build the sample MKVs");

        Backend backend(nullptr, nullptr, mkvmerge, mkvextract);
        backend.setReviewAfterScan(true);
        backend.scanFolders({dir.path()});
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), QString("review"), 60000);
        QCOMPARE(backend.queueCount(), 2);

        // Each review row carries the track plan from its probe, for the
        // expandable details view.
        const auto rows = backend.queue();
        for (const auto &row : rows) {
            const auto rowTracks = row.toMap().value("tracks").toList();
            QVERIFY(!rowTracks.isEmpty());
            QVERIFY(row.toMap().value("action").isNull());
        }

        // The anomaly stays as it is; the rest gets processed. (Queue order
        // is probe-completion order, so find it by path.)
        int keepIndex = -1;
        const auto queue = backend.queue();
        for (int i = 0; i < queue.size(); ++i)
            if (queue.at(i).toMap().value("path").toString() == keep)
                keepIndex = i;
        QVERIFY(keepIndex >= 0);
        backend.removeFromQueue(keepIndex);
        QCOMPARE(backend.queueCount(), 1);
        backend.startBatch();
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), QString("done"), 60000);

        QCOMPARE(Mkv::parseTracks(inspect(mkvmerge, clean)).size(), 2);
        QCOMPARE(Mkv::parseTracks(inspect(mkvmerge, keep)).size(), 4);
    }

    void backendScanReviewEmptiesToIdle()
    {
        const QString mkvmerge = QStandardPaths::findExecutable("mkvmerge");
        const QString mkvextract = QStandardPaths::findExecutable("mkvextract");
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        if (mkvmerge.isEmpty() || mkvextract.isEmpty() || ffmpeg.isEmpty())
            QSKIP("needs ffmpeg and MKVToolNix");

        QTemporaryDir dir;
        QVERIFY2(!makeSampleMkv(dir.path(), ffmpeg, mkvmerge, "only.mkv").isEmpty(),
                 "could not build the sample MKV");

        Backend backend(nullptr, nullptr, mkvmerge, mkvextract);
        backend.setReviewAfterScan(true);
        backend.scanFolders({dir.path()});
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), QString("review"), 60000);
        QCOMPARE(backend.queueCount(), 1);

        backend.removeFromQueue(0);
        QCOMPARE(backend.queueCount(), 0);
        QCOMPARE(backend.state(), QString("idle"));
    }

private:
    // A fake "mkvmerge" that hangs forever, for timeout tests.
    QString makeSleepTool(const QString &directory)
    {
        const QString path = QDir(directory).filePath("mkvmerge-fake");
        write(path, "#!/bin/sh\nexec sleep 600\n");
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        return path;
    }

    static MkvTrack audio(int id, const QString &language, bool isDefault)
    {
        MkvTrack track;
        track.id = id;
        track.type = "audio";
        track.codec = "AAC";
        track.codecId = "A_AAC";
        track.language = language;
        track.isDefault = isDefault;
        return track;
    }

    static MkvTrack subtitles(int id, const QString &language)
    {
        MkvTrack track;
        track.id = id;
        track.type = "subtitles";
        track.codec = "SubRip/SRT";
        track.codecId = "S_TEXT/UTF8";
        track.language = language;
        return track;
    }

    static void write(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }

    static QJsonDocument inspect(const QString &mkvmerge, const QString &path)
    {
        QProcess process;
        process.start(mkvmerge, {"-J", path});
        process.waitForFinished(15000);
        return QJsonDocument::fromJson(process.readAllStandardOutput());
    }

    // A tiny MKV with video, English (default) and Danish audio, and English
    // subtitles — the shape mkvclean exists for. Without English audio it
    // becomes the batch's failure case.
    static QString makeSampleMkv(const QString &directory, const QString &ffmpeg, const QString &mkvmerge,
                                 const QString &name = "sample.mkv", bool withEnglishAudio = true)
    {
        QDir dir(directory);
        const QString video = dir.filePath("video.mkv"), engWav = dir.filePath("eng.wav"),
                      danWav = dir.filePath("dan.wav"), engSrt = dir.filePath("eng.srt"),
                      sample = dir.filePath(name);
        if (QProcess::execute(ffmpeg, {"-hide_banner", "-loglevel", "error", "-y",
                                       "-f", "lavfi", "-i", "testsrc=duration=2:size=160x120:rate=10",
                                       "-c:v", "libx264", "-pix_fmt", "yuv420p", video}) != 0)
            return {};
        QStringList tones{QString("550")};
        if (withEnglishAudio)
            tones.prepend(QString("440"));
        for (const auto &tone : tones) {
            const QString path = tone == "440" ? engWav : danWav;
            if (QProcess::execute(ffmpeg, {"-hide_banner", "-loglevel", "error", "-y",
                                           "-f", "lavfi", "-i", "sine=frequency=" + tone + ":duration=2",
                                           "-c:a", "pcm_s16le", path}) != 0)
                return {};
        }
        write(engSrt, "1\n00:00:00,000 --> 00:00:01,500\nHello from the English track\n");
        QStringList arguments{"-q", "-o", sample, video};
        if (withEnglishAudio)
            arguments << "--language" << "0:eng" << "--default-track-flag" << "0:yes"
                      << "--track-name" << "0:English stereo" << engWav;
        arguments << "--language" << "0:dan" << danWav << "--language" << "0:eng" << engSrt;
        if (QProcess::execute(mkvmerge, arguments) != 0)
            return {};
        return sample;
    }

    // Video + English audio + one English subtitle track flagged as hearing
    // impaired, carrying both a sound description and dialogue.
    static QString makeSdhMkv(const QString &directory, const QString &ffmpeg,
                              const QString &mkvmerge, const QString &name = "sdh.mkv")
    {
        QDir dir(directory);
        const QString video = dir.filePath("video.mkv"), audio = dir.filePath("eng.wav"),
                      srt = dir.filePath("sdh.srt"), sample = dir.filePath(name);
        if (QProcess::execute(ffmpeg, {"-hide_banner", "-loglevel", "error", "-y",
                                       "-f", "lavfi", "-i", "testsrc=duration=2:size=160x120:rate=10",
                                       "-c:v", "libx264", "-pix_fmt", "yuv420p", video}) != 0)
            return {};
        if (QProcess::execute(ffmpeg, {"-hide_banner", "-loglevel", "error", "-y",
                                       "-f", "lavfi", "-i", "sine=frequency=440:duration=2",
                                       "-c:a", "pcm_s16le", audio}) != 0)
            return {};
        write(srt, "1\n00:00:00,000 --> 00:00:01,000\n[DOOR CLOSES]\n\n"
                   "2\n00:00:01,200 --> 00:00:02,000\nHello from the SDH track\n");
        const QStringList arguments{"-q", "-o", sample, video,
                                    "--language", "0:eng", "--default-track-flag", "0:yes", audio,
                                    "--language", "0:eng", "--hearing-impaired-flag", "0:yes", srt};
        if (QProcess::execute(mkvmerge, arguments) != 0)
            return {};
        return sample;
    }
};

QTEST_MAIN(MkvCleanTests)
#include "mkvclean_tests.moc"
