#include "settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

#include "mkv.h"

AppSettings::AppSettings(const QString &path, const QString &legacyWatchFoldersPath, QObject *parent)
    : QObject(parent),
      m_path(path.isEmpty() ? defaultPath() : path) {
    load(legacyWatchFoldersPath.isEmpty() ? defaultLegacyPath() : legacyWatchFoldersPath);
}

QString AppSettings::defaultPath() {
    return QDir::homePath() + "/.config/mkvclean/config.json";
}

QString AppSettings::defaultLegacyPath() {
    return QDir::homePath() + "/.config/mkvclean/watch-folders";
}

QVariantList AppSettings::languageModel() const {
    QVariantList model;
    for (const auto &language : Mkv::languages())
        model.append(QVariantMap{{"code", language.first}, {"name", language.second}});
    return model;
}

QString AppSettings::validCode(const QString &code) {
    static const QRegularExpression valid("^[a-z]{3}$");
    const QString lowered = code.trimmed().toLower();
    return valid.match(lowered).hasMatch() ? lowered : QString();
}

void AppSettings::addWatchFolder(const QString &folder) {
    const QString absolute = QFileInfo(folder).absoluteFilePath();
    if (!QFileInfo(absolute).isDir() || m_watchFolders.contains(absolute))
        return;
    m_watchFolders.append(absolute);
    save();
}

void AppSettings::removeWatchFolder(const QString &folder) {
    if (m_watchFolders.removeAll(folder))
        save();
}

void AppSettings::addExcludedFolder(const QString &folder) {
    const QString absolute = QFileInfo(folder).absoluteFilePath();
    if (!QFileInfo(absolute).isDir() || m_excludedFolders.contains(absolute))
        return;
    m_excludedFolders.append(absolute);
    save();
}

void AppSettings::removeExcludedFolder(const QString &folder) {
    if (m_excludedFolders.removeAll(folder))
        save();
}

void AppSettings::setAudioLanguage(const QString &code) {
    const QString valid = validCode(code);
    if (valid.isEmpty() || valid == m_audioLanguage)
        return;
    m_audioLanguage = valid;
    save();
}

void AppSettings::setSubtitleLanguage(const QString &code) {
    const QString valid = validCode(code);
    if (valid.isEmpty() || valid == m_subtitleLanguage)
        return;
    m_subtitleLanguage = valid;
    save();
}

void AppSettings::setConvertSdh(bool enabled) {
    if (enabled == m_convertSdh)
        return;
    m_convertSdh = enabled;
    save();
}

void AppSettings::load(const QString &legacyPath) {
    QFile file(m_path);
    if (file.open(QIODevice::ReadOnly)) {
        const auto object = QJsonDocument::fromJson(file.readAll()).object();
        for (const auto &folder : object.value("watchFolders").toArray())
            if (folder.isString())
                m_watchFolders.append(folder.toString());
        for (const auto &folder : object.value("excludedFolders").toArray())
            if (folder.isString())
                m_excludedFolders.append(folder.toString());
        const QString audio = validCode(object.value("audioLanguage").toString());
        if (!audio.isEmpty())
            m_audioLanguage = audio;
        const QString subtitle = validCode(object.value("subtitleLanguage").toString());
        if (!subtitle.isEmpty())
            m_subtitleLanguage = subtitle;
        m_convertSdh = object.value("convertSdh").toBool(false);
        return;
    }

    // First run after the settings panel existed: adopt the folder list the
    // watch commands used to keep, so nothing has to be re-entered.
    QFile legacy(legacyPath);
    if (legacy.open(QIODevice::ReadOnly)) {
        for (const auto &line : QString::fromUtf8(legacy.readAll()).split('\n')) {
            const QString trimmed = line.trimmed();
            if (!trimmed.isEmpty() && !trimmed.startsWith('#'))
                m_watchFolders.append(trimmed);
        }
        if (!m_watchFolders.isEmpty())
            save();
    }
}

bool AppSettings::save() {
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QJsonObject object{
        {"watchFolders", QJsonArray::fromStringList(m_watchFolders)},
        {"excludedFolders", QJsonArray::fromStringList(m_excludedFolders)},
        {"audioLanguage", m_audioLanguage},
        {"subtitleLanguage", m_subtitleLanguage},
        {"convertSdh", m_convertSdh},
    };
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    if (!file.commit())
        return false;
    emit changed();
    return true;
}
