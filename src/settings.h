#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantList>

// User preferences in ~/.config/mkvclean/config.json: the watched folders and
// the preferred audio/subtitle languages (ISO 639-2). Saved atomically on
// every change; the legacy watch-folders file is imported on first load.
class AppSettings : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList watchFolders READ watchFolders NOTIFY changed)
    Q_PROPERTY(QStringList excludedFolders READ excludedFolders NOTIFY changed)
    Q_PROPERTY(QString audioLanguage READ audioLanguage NOTIFY changed)
    Q_PROPERTY(QString subtitleLanguage READ subtitleLanguage NOTIFY changed)
    Q_PROPERTY(QVariantList languageModel READ languageModel CONSTANT)

public:
    explicit AppSettings(const QString &path = {}, const QString &legacyWatchFoldersPath = {},
                         QObject *parent = nullptr);

    static QString defaultPath();
    static QString defaultLegacyPath();

    QStringList watchFolders() const { return m_watchFolders; }
    QStringList excludedFolders() const { return m_excludedFolders; }
    QString audioLanguage() const { return m_audioLanguage; }
    QString subtitleLanguage() const { return m_subtitleLanguage; }
    QVariantList languageModel() const;

    Q_INVOKABLE void addWatchFolder(const QString &folder);
    Q_INVOKABLE void removeWatchFolder(const QString &folder);
    Q_INVOKABLE void addExcludedFolder(const QString &folder);
    Q_INVOKABLE void removeExcludedFolder(const QString &folder);
    void setAudioLanguage(const QString &code);
    void setSubtitleLanguage(const QString &code);

signals:
    void changed();

private:
    void load(const QString &legacyPath);
    bool save();
    static QString validCode(const QString &code);

    QString m_path;
    QStringList m_watchFolders;
    QStringList m_excludedFolders;
    QString m_audioLanguage = "eng";
    QString m_subtitleLanguage = "eng";
};
