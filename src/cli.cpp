#include "cli.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QVariantMap>

#include "backend.h"

namespace {

void printSingleResult(const Backend &backend, bool json) {
    const auto plan = backend.plan();
    const MkvTrack *audio = plan.track(plan.keepAudioId);
    if (json) {
        const QJsonObject result{
            {"input", backend.sourcePath()},
            {"output", backend.outputPath()},
            {"subtitles", QJsonArray::fromStringList(backend.extractedFiles())},
            {"kept_audio", QJsonObject{{"id", plan.keepAudioId},
                                       {"language", audio ? audio->language : QString()},
                                       {"codec", audio ? audio->codec : QString()}}},
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
