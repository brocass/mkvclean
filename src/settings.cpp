#include "settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

#include "mkv.h"

AppSettings::AppSettings(const QString &path, const QString &legacyWatchFoldersPath, QObject *parent)
    : QObject(parent),
      m_path(path.isEmpty() ? defaultPath() : path) {
    // The watch service is a separate process: following the file lets a
    // change made in the window reach it without a restart.
    m_configWatcher = new QFileSystemWatcher(this);
    connect(m_configWatcher, &QFileSystemWatcher::directoryChanged, this,
            [this] { reload(); });

    load(legacyWatchFoldersPath.isEmpty() ? defaultLegacyPath() : legacyWatchFoldersPath);
    watchConfigDirectory();
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

void AppSettings::addBlackBarWatchFolder(const QString &folder) {
    const QString absolute = QFileInfo(folder).absoluteFilePath();
    if (!QFileInfo(absolute).isDir() || m_blackBarWatchFolders.contains(absolute))
        return;
    m_blackBarWatchFolders.append(absolute);
    save();
}

void AppSettings::removeBlackBarWatchFolder(const QString &folder) {
    if (m_blackBarWatchFolders.removeAll(folder))
        save();
}

void AppSettings::addBlackBarExcludedFolder(const QString &folder) {
    const QString absolute = QFileInfo(folder).absoluteFilePath();
    if (!QFileInfo(absolute).isDir() || m_blackBarExcludedFolders.contains(absolute))
        return;
    m_blackBarExcludedFolders.append(absolute);
    save();
}

void AppSettings::removeBlackBarExcludedFolder(const QString &folder) {
    if (m_blackBarExcludedFolders.removeAll(folder))
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

void AppSettings::setRemoveBars(bool enabled) {
    if (enabled == m_removeBars)
        return;
    m_removeBars = enabled;
    save();
}

void AppSettings::load(const QString &legacyPath) {
    if (readConfig(&m_watchFolders, &m_excludedFolders, &m_audioLanguage, &m_subtitleLanguage,
                   &m_convertSdh, &m_removeBars, &m_blackBarWatchFolders, &m_blackBarExcludedFolders))
        return;

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

    // Black-bar cleaning gets its own lists; until they are set, fall back to
    // the ordinary watch/exclude lists so an unconfigured install behaves as
    // before.
    if (m_blackBarWatchFolders.isEmpty() && !m_watchFolders.isEmpty())
        m_blackBarWatchFolders = m_watchFolders;
    if (m_blackBarExcludedFolders.isEmpty() && !m_excludedFolders.isEmpty())
        m_blackBarExcludedFolders = m_excludedFolders;
}

bool AppSettings::readConfig(QStringList *watchFolders, QStringList *excludedFolders,
                             QString *audioLanguage, QString *subtitleLanguage, bool *convertSdh,
                             bool *removeBars, QStringList *blackBarWatchFolders,
                             QStringList *blackBarExcludedFolders) {
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;

    const auto object = document.object();
    for (const auto &folder : object.value("watchFolders").toArray())
        if (folder.isString())
            watchFolders->append(folder.toString());
    for (const auto &folder : object.value("excludedFolders").toArray())
        if (folder.isString())
            excludedFolders->append(folder.toString());
    const QString audio = validCode(object.value("audioLanguage").toString());
    if (!audio.isEmpty())
        *audioLanguage = audio;
    const QString subtitle = validCode(object.value("subtitleLanguage").toString());
    if (!subtitle.isEmpty())
        *subtitleLanguage = subtitle;
    *convertSdh = object.value("convertSdh").toBool(false);
    *removeBars = object.value("removeBars").toBool(false);
    for (const auto &folder : object.value("blackBarWatchFolders").toArray())
        if (folder.isString())
            blackBarWatchFolders->append(folder.toString());
    for (const auto &folder : object.value("blackBarExcludedFolders").toArray())
        if (folder.isString())
            blackBarExcludedFolders->append(folder.toString());
    return true;
}

void AppSettings::watchConfigDirectory() {
    if (!m_configWatcher)
        return;
    const QString directory = QFileInfo(m_path).absolutePath();
    if (QFileInfo(directory).isDir() && !m_configWatcher->directories().contains(directory))
        m_configWatcher->addPath(directory);
}

void AppSettings::reload() {
    QStringList watchFolders, excludedFolders;
    QStringList blackBarWatchFolders, blackBarExcludedFolders;
    QString audio = m_audioLanguage, subtitle = m_subtitleLanguage;
    bool convertSdh = m_convertSdh, removeBars = m_removeBars;
    if (!readConfig(&watchFolders, &excludedFolders, &audio, &subtitle, &convertSdh, &removeBars,
                    &blackBarWatchFolders, &blackBarExcludedFolders))
        return;
    if (watchFolders == m_watchFolders && excludedFolders == m_excludedFolders
        && blackBarWatchFolders == m_blackBarWatchFolders && blackBarExcludedFolders == m_blackBarExcludedFolders
        && audio == m_audioLanguage && subtitle == m_subtitleLanguage
        && convertSdh == m_convertSdh && removeBars == m_removeBars)
        return;

    m_watchFolders = watchFolders;
    m_excludedFolders = excludedFolders;
    m_blackBarWatchFolders = blackBarWatchFolders;
    m_blackBarExcludedFolders = blackBarExcludedFolders;
    m_audioLanguage = audio;
    m_subtitleLanguage = subtitle;
    m_convertSdh = convertSdh;
    m_removeBars = removeBars;
    emit changed();
}

bool AppSettings::save() {
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QJsonObject object{
        {"watchFolders", QJsonArray::fromStringList(m_watchFolders)},
        {"excludedFolders", QJsonArray::fromStringList(m_excludedFolders)},
        {"blackBarWatchFolders", QJsonArray::fromStringList(m_blackBarWatchFolders)},
        {"blackBarExcludedFolders", QJsonArray::fromStringList(m_blackBarExcludedFolders)},
        {"audioLanguage", m_audioLanguage},
        {"subtitleLanguage", m_subtitleLanguage},
        {"convertSdh", m_convertSdh},
        {"removeBars", m_removeBars},
    };
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    if (!file.commit())
        return false;
    watchConfigDirectory();
    emit changed();
    return true;
}
