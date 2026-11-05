#include "cli.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTextStream>
#include <QVariantMap>

#include "backend.h"
#include "settings.h"
#include "watcher.h"

namespace {

void printSingleResult(const Backend &backend, bool json) {
    const auto plan = backend.plan();
    const MkvTrack *audio = plan.track(plan.keepAudioId);
    if (json) {
        // null when the file had no audio at all (video-only output).
        const QJsonValue keptAudio = audio
            ? QJsonValue(QJsonObject{{"id", plan.keepAudioId},
                                     {"language", audio->language},
                                     {"codec", audio->codec}})
            : QJsonValue();
        const QJsonObject result{
            {"input", backend.sourcePath()},
            {"output", backend.outputPath()},
            {"subtitles", QJsonArray::fromStringList(backend.extractedFiles())},
            {"kept_audio", keptAudio},
            {"removed", QJsonObject{{"tracks", plan.removedCount()}}},
        };
        QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << "\n";
    } else {
        QTextStream out(stdout);
        for (const auto &file : backend.extractedFiles())
            out << "Extracted " << file << "\n";
        out << "Replaced " << backend.outputPath() << "\n";
    }
}

// One line per file: JSON objects with --json, short text without.
int printBatchResults(const QVariantList &queue, bool json) {
    int failed = 0;
    for (const auto &entry : queue) {
        const auto map = entry.toMap();
        const bool ok = map.value("state").toString() == "done";
        const QString path = map.value("path").toString();
        if (json) {
            QJsonObject result{{"input", path}, {"status", ok ? "ok" : "error"}};
            if (ok) {
                result.insert("output", path);
                result.insert("subtitles", QJsonArray::fromStringList(map.value("subtitles").toStringList()));
                result.insert("kept_audio", QJsonObject{{"id", map.value("keptAudioId").toInt()}});
            } else {
                result.insert("error", map.value("error").toString());
                ++failed;
            }
            QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << "\n";
        } else if (ok) {
            QTextStream(stdout) << "Cleaned " << path << "\n";
        } else {
            QTextStream(stderr) << "mkvclean: " << path << ": " << map.value("error").toString() << "\n";
            ++failed;
        }
    }
    if (!json)
        QTextStream(stdout) << (queue.size() - failed) << " cleaned, " << failed << " failed\n";
    return failed ? 1 : 0;
}

}

int runClean(QCoreApplication &app, const QStringList &inputs, bool json) {
    Backend backend(nullptr, &app);
    AppSettings settings;
    backend.setSettings(&settings);
    int exitCode = 1;

    QObject::connect(&backend, &Backend::stateChanged, &app, [&] {
        const QString state = backend.state();
        if (state == "ready") {
            backend.clean();
            return;
        }
        if (state == "done") {
            if (backend.queueCount() <= 1) {
                printSingleResult(backend, json);
                exitCode = 0;
            } else {
                exitCode = printBatchResults(backend.queue(), json);
            }
            app.quit();
            return;
        }
        if (state == "error") {
            QTextStream(stderr) << "mkvclean: " << backend.errorText() << "\n";
            exitCode = 1;
            app.quit();
        }
    });

    backend.openPaths(inputs);

    // A bad path or missing MKVToolNix fails synchronously, before any
    // process starts; there is nothing to wait for then.
    if (backend.state() == "error") {
        QTextStream(stderr) << "mkvclean: " << backend.errorText() << "\n";
        return 1;
    }

    app.exec();
    return exitCode;
}

int runWatch(QCoreApplication &app, const QStringList &args) {
    AppSettings settings;
    const QString command = args.isEmpty() ? QStringLiteral("run") : args.first();

    if (command == "add" || command == "remove" || command == "exclude" || command == "unexclude") {
        if (args.size() < 2) {
            QTextStream(stderr) << "usage: mkvclean watch " << command << " <folder>\n";
            return 1;
        }
        const QString folder = QFileInfo(args.at(1)).absoluteFilePath();
        if (!QFileInfo(folder).isDir()) {
            QTextStream(stderr) << "mkvclean: not a folder: " << folder << "\n";
            return 1;
        }
        if (command == "add") {
            settings.addWatchFolder(folder);
            QTextStream(stdout) << "Watching " << folder << "\n";
        } else if (command == "remove") {
            if (!settings.watchFolders().contains(folder)) {
                QTextStream(stderr) << "mkvclean: that folder was not watched: " << folder << "\n";
                return 1;
            }
            settings.removeWatchFolder(folder);
            QTextStream(stdout) << "Removed " << folder << "\n";
        } else if (command == "exclude") {
            settings.addExcludedFolder(folder);
            QTextStream(stdout) << "Excluded " << folder << "\n";
        } else {
            if (!settings.excludedFolders().contains(folder)) {
                QTextStream(stderr) << "mkvclean: that folder was not excluded: " << folder << "\n";
                return 1;
            }
            settings.removeExcludedFolder(folder);
            QTextStream(stdout) << "No longer excluded " << folder << "\n";
        }
        QTextStream(stdout) << "If the service is running, restart it: systemctl --user restart mkvclean\n";
        return 0;
    }
    if (command == "list") {
        QTextStream out(stdout);
        const auto folders = settings.watchFolders();
        if (folders.isEmpty())
            out << "No folders configured. Add one with: mkvclean watch add <folder>\n";
        for (const auto &folder : folders)
            out << folder << "\n";
        for (const auto &folder : settings.excludedFolders())
            out << "  excluded: " << folder << "\n";
        return 0;
    }
    if (command == "enable" || command == "disable") {
        const int code = QProcess::execute("systemctl", {"--user", command, "--now", "mkvclean"});
        if (code != 0) {
            QTextStream(stderr) << "mkvclean: systemctl failed — is the mkvclean package installed?\n";
            return 1;
        }
        QTextStream(stdout) << "Service " << (command == "enable" ? "enabled and started" : "stopped and disabled") << "\n";
        return 0;
    }
    if (command == "status") {
        QProcess process;
        process.start("systemctl", {"--user", "is-active", "mkvclean"});
        process.waitForFinished(5000);
        QTextStream out(stdout);
        out << "Service: " << QString::fromUtf8(process.readAllStandardOutput()).trimmed() << "\n";
        const auto folders = settings.watchFolders();
        out << "Watching " << folders.size() << " folder(s)\n";
        for (const auto &folder : folders)
            out << "  " << folder << "\n";
        return 0;
    }
    if (command == "scan") {
        const auto folders = settings.watchFolders();
        if (folders.isEmpty()) {
            QTextStream(stderr) << "mkvclean: no folders to scan. Add one with: mkvclean watch add <folder>\n";
            return 1;
        }
        const bool json = args.contains("--json");
        Backend backend(nullptr, &app);
        backend.setSettings(&settings);
        int exitCode = 1;
        QObject::connect(&backend, &Backend::stateChanged, &app, [&] {
            const QString state = backend.state();
            if (state == "ready") {
                backend.clean();
                return;
            }
            if (state == "done") {
                if (backend.scanFoundNothing()) {
                    QTextStream(stdout) << "Everything is already clean.\n";
                    exitCode = 0;
                } else if (backend.queueCount() <= 1) {
                    printSingleResult(backend, json);
                    exitCode = 0;
                } else {
                    exitCode = printBatchResults(backend.queue(), json);
                }
                app.quit();
                return;
            }
            if (state == "error") {
                QTextStream(stderr) << "mkvclean: " << backend.errorText() << "\n";
                exitCode = 1;
                app.quit();
            }
        });
        backend.scanFolders(folders);
        app.exec();
        return exitCode;
    }
    if (command != "run") {
        QTextStream(stderr) << "usage: mkvclean watch [run|add|remove|exclude|unexclude|list|scan|enable|disable|status] …\n";
        return 1;
    }

    const auto folders = settings.watchFolders();
    if (folders.isEmpty()) {
        QTextStream(stderr) << "mkvclean: no folders to watch. Add one with: mkvclean watch add <folder>\n";
        return 1;
    }
    Watcher watcher(folders, 15000, &app, {}, {}, &settings);
    if (!watcher.toolsAvailable()) {
        QTextStream(stderr) << "mkvclean: MKVToolNix (mkvmerge, mkvextract) is not installed\n";
        return 1;
    }
    QObject::connect(&watcher, &Watcher::logLine, &app,
                     [](const QString &line) { QTextStream(stdout) << line << "\n"; });
    watcher.start();
    return app.exec();
}

int runSkillInstall() {
    QFile bundled(":/skill/SKILL.md");
    if (!bundled.open(QIODevice::ReadOnly)) {
        QTextStream(stderr) << "mkvclean: the bundled skill is missing\n";
        return 1;
    }
    const QString directory = QDir::homePath() + "/.agents/skills/mkvclean";
    if (!QDir().mkpath(directory)) {
        QTextStream(stderr) << "mkvclean: could not create " << directory << "\n";
        return 1;
    }
    QFile target(directory + "/SKILL.md");
    if (!target.open(QIODevice::WriteOnly)) {
        QTextStream(stderr) << "mkvclean: could not write " << target.fileName() << "\n";
        return 1;
    }
    target.write(bundled.readAll());
    QTextStream(stdout) << "Installed " << directory + "/SKILL.md" << "\n";
    return 0;
}
