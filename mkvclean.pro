QT += core gui qml quick quickcontrols2 dbus

CONFIG += c++17 release
TARGET = mkvclean
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/cli.h \
    src/filepicker.h \
    src/mkv.h \
    src/portalfilepicker.h \
    src/settings.h \
    src/theme.h \
    src/translator.h \
    src/watcher.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/cli.cpp \
    src/mkv.cpp \
    src/portalfilepicker.cpp \
    src/settings.cpp \
    src/theme.cpp \
    src/translator.cpp \
    src/watcher.cpp

RESOURCES += src/resources.qrc
