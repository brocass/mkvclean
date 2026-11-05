#pragma once

// Adapted from Omacut (MIT, David Heinemeier Hansson): an abstract picker so
// the backend can be tested with a fake while the real one talks to the
// xdg-desktop-portal file chooser.

#include <QList>
#include <QObject>
#include <QUrl>

class FilePicker : public QObject {
    Q_OBJECT

public:
    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~FilePicker() = default;

    virtual void openFiles() = 0;
    virtual void openFolder() = 0;

signals:
    void filesSelected(const QList<QUrl> &urls);
    void failed(const QString &message);
};
