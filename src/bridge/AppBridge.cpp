#include "AppBridge.h"

#include "app/GlobalHotkey.h"
#include "services/DownloadService.h"
#include "services/NoteService.h"
#include "services/ServiceResult.h"
#include "services/SettingsService.h"
#include "services/ShortcutService.h"
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonValue>
#include <QUrl>
#include <QWidget>

AppBridge::AppBridge(const QString& dataRoot, QWidget* window, bool nativeIntegration)
    : QObject(window), m_dataRoot(QDir(dataRoot).absolutePath()), m_window(window),
      m_shortcuts(new ShortcutService(m_dataRoot, this)),
      m_notes(new NoteService(m_dataRoot, this)),
      m_settings(new SettingsService(m_dataRoot, this)),
      m_downloads(new DownloadService(m_dataRoot, this)),
      m_hotkey(new GlobalHotkey(nativeIntegration, this))
{
    connect(m_shortcuts, &ShortcutService::changed, this, &AppBridge::shortcutsChanged);
    connect(m_notes, &NoteService::changed, this, &AppBridge::notesChanged);
    connect(m_settings, &SettingsService::changed, this, &AppBridge::settingsChanged);
    connect(m_downloads, &DownloadService::changed, this, &AppBridge::downloadsChanged);
    connect(m_hotkey, &GlobalHotkey::activated, this, &AppBridge::activateWindowRequested);
    const auto result = m_settings->snapshot();
    if (result.value("ok").toBool()) {
        const auto settings = result.value("data").toMap();
        m_hotkey->setShortcut(settings.value("hotkeyModifier").toInt(),
                             settings.value("hotkeyKey").toInt(), &m_hotkeyWarning);
    } else {
        m_hotkeyWarning = result.value("error").toString();
    }
}

QVariantMap AppBridge::getShortcuts() const { return m_shortcuts->snapshot(); }
QVariantMap AppBridge::saveShortcut(const QVariantMap& item) { return m_shortcuts->saveShortcut(item); }
QVariantMap AppBridge::deleteShortcuts(const QStringList& ids) { return m_shortcuts->removeShortcuts(ids); }
QVariantMap AppBridge::moveShortcut(const QString& id, const QString& categoryId, const QString& beforeId)
{ return m_shortcuts->moveShortcut(id, categoryId, beforeId); }
QVariantMap AppBridge::saveShortcutCategory(const QString& id, const QString& name)
{ return m_shortcuts->saveCategory(id, name); }
QVariantMap AppBridge::deleteShortcutCategory(const QString& id) { return m_shortcuts->removeCategory(id); }
QVariantMap AppBridge::openShortcut(const QString& id) { return m_shortcuts->openShortcut(id); }

QVariantMap AppBridge::chooseTarget(int type)
{
    if (type != 0 && type != 2)
        return ServiceResult::failure(QStringLiteral("请选择文件或目录类型。"));
    const QString path = type == 0
        ? QFileDialog::getExistingDirectory(m_window, QStringLiteral("选择目录"))
        : QFileDialog::getOpenFileName(m_window, QStringLiteral("选择文件"));
    if (path.isEmpty())
        return ServiceResult::success(QVariantMap{{"cancelled", true}});
    return ServiceResult::success(QVariantMap{{"target", path}, {"title", QFileInfo(path).fileName()}});
}

QVariantMap AppBridge::copyText(const QString& text)
{
    QApplication::clipboard()->setText(text);
    return ServiceResult::success();
}

QVariantMap AppBridge::getNotes() const { return m_notes->snapshot(); }
QVariantMap AppBridge::getNote(const QString& id) const { return m_notes->readNote(id); }
QVariantMap AppBridge::saveNote(const QVariantMap& note) { return m_notes->saveNote(note); }
QVariantMap AppBridge::deleteNotes(const QStringList& ids) { return m_notes->removeNotes(ids); }
QVariantMap AppBridge::moveNote(const QString& id, const QString& categoryId, const QString& beforeId)
{ return m_notes->moveNote(id, categoryId, beforeId); }
QVariantMap AppBridge::saveNoteCategory(const QString& id, const QString& name)
{ return m_notes->saveCategory(id, name); }
QVariantMap AppBridge::deleteNoteCategory(const QString& id) { return m_notes->removeCategory(id); }

QVariantMap AppBridge::getDownloads() const { return m_downloads->snapshot(); }

QVariantMap AppBridge::createDownload(const QVariantMap& task)
{
    if (!QJsonValue::fromVariant(task.value(QStringLiteral("url"))).isString())
        return ServiceResult::failure(QStringLiteral("请填写有效的下载网址。"));
    if (task.contains(QStringLiteral("directory"))
        && !QJsonValue::fromVariant(task.value(QStringLiteral("directory"))).isString())
        return ServiceResult::failure(QStringLiteral("下载目录必须是文件夹路径。"));
    const auto settings = m_settings->snapshot();
    if (!settings.value(QStringLiteral("ok")).toBool())
        return settings;
    return m_downloads->createTask(task.value(QStringLiteral("url")).toString(),
        task.value(QStringLiteral("directory")).toString(),
        settings.value(QStringLiteral("data")).toMap().value(QStringLiteral("downloadDirectory")).toString());
}

QVariantMap AppBridge::getDownloadFiles(const QString& id) const { return m_downloads->files(id); }
QVariantMap AppBridge::confirmDownloadFiles(const QString& id, const QVariantList& indices)
{ return m_downloads->confirmFiles(id, indices); }
QVariantMap AppBridge::pauseDownload(const QString& id) { return m_downloads->pauseTask(id); }
QVariantMap AppBridge::resumeDownload(const QString& id) { return m_downloads->resumeTask(id); }
QVariantMap AppBridge::cancelDownload(const QString& id) { return m_downloads->cancelTask(id); }
QVariantMap AppBridge::removeDownload(const QString& id) { return m_downloads->removeTask(id); }
QVariantMap AppBridge::openDownloadDirectory(const QString& id) { return m_downloads->openDirectory(id); }
bool AppBridge::hasActiveDownloads() const { return m_downloads->hasActiveTasks(); }

QVariantMap AppBridge::chooseDownloadDirectory()
{
    const QString initial = m_settings->snapshot().value(QStringLiteral("data")).toMap()
                                .value(QStringLiteral("downloadDirectory")).toString();
    const QString directory = QFileDialog::getExistingDirectory(m_window, QStringLiteral("选择下载目录"), initial);
    return ServiceResult::success(QVariantMap{{QStringLiteral("directory"), directory},
        {QStringLiteral("cancelled"), directory.isEmpty()}});
}

QVariantMap AppBridge::getSettings() const { return m_settings->snapshot(); }

QVariantMap AppBridge::saveSettings(const QVariantMap& settings)
{
    const auto current = m_settings->snapshot();
    if (!current.value("ok").toBool())
        return current;
    // 与快照合并，兼容未传下载目录的旧调用方以及只修改单个设置。
    auto candidate = current.value(QStringLiteral("data")).toMap();
    for (auto iterator = settings.cbegin(); iterator != settings.cend(); ++iterator)
        candidate.insert(iterator.key(), iterator.value());
    const auto validated = SettingsService::validate(candidate);
    if (!validated.value("ok").toBool())
        return validated;
    const auto next = validated.value("data").toMap();
    QString error;
    if (!m_hotkey->prepareShortcut(next.value("hotkeyModifier").toInt(), next.value("hotkeyKey").toInt(), &error))
        return ServiceResult::failure(error);
    const auto saved = m_settings->save(next);
    if (!saved.value("ok").toBool()) {
        m_hotkey->cancelShortcut();
        return saved;
    }
    m_hotkey->commitShortcut();
    m_hotkeyWarning.clear();
    return saved;
}

QVariantMap AppBridge::resetSettings()
{
    return saveSettings({{"fontSize", 16}, {"hotkeyModifier", 0}, {"hotkeyKey", 0x77},
        {"downloadDirectory", QString()}});
}

QVariantMap AppBridge::openDataDirectory()
{
    if (!QDir().mkpath(m_dataRoot) || !QDesktopServices::openUrl(QUrl::fromLocalFile(m_dataRoot)))
        return ServiceResult::failure(QStringLiteral("无法打开数据目录。"));
    return ServiceResult::success();
}

QVariantMap AppBridge::getAppInfo() const
{
    return ServiceResult::success(QVariantMap{{"version", QCoreApplication::applicationVersion()},
        {"dataPath", m_dataRoot}, {"hotkeyWarning", m_hotkeyWarning}});
}
