// mkvclean — extract English subtitles from an MKV and keep only the default
// English audio track.

#include <QCoreApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTextStream>
#include <QUrl>

#include "backend.h"
#include "cli.h"
#include "portalfilepicker.h"
#include "settings.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    QStringList arguments;
    for (int i = 0; i < argc; ++i)
        arguments.append(QString::fromLocal8Bit(argv[i]));

    // Headless commands never touch the display.
    if (arguments.size() >= 2 && arguments.at(1) == "clean") {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName("mkvclean");
        const bool json = arguments.contains("--json");
        QStringList inputs;
        for (int i = 2; i < arguments.size(); ++i)
            if (!arguments.at(i).startsWith("--"))
                inputs.append(QFileInfo(arguments.at(i)).absoluteFilePath());
        if (inputs.isEmpty()) {
            QTextStream(stderr) << "usage: mkvclean clean <file-or-folder>… [--json]\n";
            return 1;
        }
        return runClean(app, inputs, json);
    }
    if (arguments.size() >= 2 && arguments.at(1) == "watch") {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName("mkvclean");
        return runWatch(app, arguments.mid(2));
    }
    if (arguments.size() >= 3 && arguments.at(1) == "skill" && arguments.at(2) == "install") {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName("mkvclean");
        return runSkillInstall();
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("mkvclean");
    app.setApplicationVersion("0.5.9");

    // Matches the window to mkvclean.desktop, so the compositor (Wayland
    // app_id) and the launcher pick up the installed icon.
    app.setDesktopFileName("mkvclean");
    app.setWindowIcon(QIcon::fromTheme("mkvclean"));

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    AppSettings settings;
    PortalFilePicker picker;
    Backend backend(&picker);
    backend.setSettings(&settings);
    backend.setReviewAfterScan(true);

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("settings", &settings);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    // `mkvclean [open] <file.mkv>` opens straight into the file.
    QString file;
    for (int i = 1; i < arguments.size(); ++i) {
        const QString &argument = arguments.at(i);
        if (argument == "open" || argument.startsWith("-"))
            continue;
        file = argument;
        break;
    }
    if (!file.isEmpty())
        backend.openPath(QFileInfo(file).absoluteFilePath());

    return app.exec();
}
