QT += core gui qml quick quickcontrols2 testlib dbus
CONFIG += c++17 testcase
TARGET = mkvclean_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += ../src/backend.h ../src/filepicker.h ../src/mkv.h ../src/theme.h ../src/translator.h
SOURCES += mkvclean_tests.cpp ../src/backend.cpp ../src/mkv.cpp ../src/theme.cpp ../src/translator.cpp
