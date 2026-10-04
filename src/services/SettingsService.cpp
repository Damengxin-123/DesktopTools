#include "SettingsService.h"
#include "ServiceResult.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryFile>

#include <cmath>

namespace
{
// 写入或移除本程序的自启值，使用独立实例检查本次写入与实际回读结果。
bool writeStartupValue(QSettings* storage, const QVariant& value)
{
    QSettings entry(storage->fileName(), storage->format());
    const QString key = QStringLiteral("DesktopTool");
    if (value.isValid())
        entry.setValue(key, value);
    else
        entry.remove(key);
    entry.sync();
    return entry.status() == QSettings::NoError && entry.value(key) == value;
}

// 只接受有限整数，避免小数或字符串被静默转换为设置值。
bool integerValue(const QVariant& value, int minimum, int maximum)
{
    const QJsonValue number = QJsonValue::fromVariant(value);
    const double result = number.toDouble();
    return number.isDouble() && std::isfinite(result) && std::floor(result) == result
        && result >= minimum && result <= maximum;
}

// 获取系统下载目录；系统未提供位置时回退到用户主目录中的 Downloads。
QString defaultDownloadDirectory()
{
    QString path = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (path.isEmpty())
        path = QDir::home().filePath(QStringLiteral("Downloads"));
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

// 校验下载目录，只允许为系统默认位置创建缺失目录。
QString checkDownloadDirectory(const QString& path)
{
    const QString defaultPath = defaultDownloadDirectory();
#ifdef Q_OS_WIN
    const bool isDefault = path.compare(defaultPath, Qt::CaseInsensitive) == 0;
#else
    const bool isDefault = path == defaultPath;
#endif
    if (!QFileInfo::exists(path) && isDefault && !QDir().mkpath(path))
        return QStringLiteral("无法创建系统默认下载目录，请选择其他现有目录。");
    const QFileInfo directory(path);
    if (!directory.exists() || !directory.isDir())
        return QStringLiteral("下载目录不存在或不是文件夹，请选择一个现有目录。");
    QTemporaryFile probe(QDir(path).filePath(QStringLiteral(".desktoptool-write-test-XXXXXX")));
    if (!probe.open() || probe.write("1", 1) != 1 || !probe.flush())
        return QStringLiteral("下载目录无法写入，请检查权限或选择其他目录。");
    return {};
}
// 只允许为修饰键的虚拟键码，两类热键共用。
bool isModifierOnlyKey(int key)
{
    return key == 0x10 || key == 0x11 || key == 0x12 || key == 0x5B || key == 0x5C
        || (key >= 0xA0 && key <= 0xA5);
}
}

SettingsService::SettingsService(const QString& dataRoot, QObject* parent, QSettings* startupSettings)
    : QObject(parent)
    , m_path(QDir(dataRoot).filePath(QStringLiteral("setting/system_config.json")))
    , m_startupSettings(startupSettings)
    // 数据目录使用正斜杠，避免盘符根目录末尾的反斜杠转义结束引号。
    , m_startupCommand(QStringLiteral("\"%1\" --data-dir \"%2\"")
          .arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()), QDir(dataRoot).absolutePath()))
{
    QFile file(m_path);
    if (!file.exists())
        return;
    if (!file.open(QIODevice::ReadOnly))
    {
        m_loadError = QStringLiteral("无法读取设置文件，已禁止覆盖：%1").arg(file.errorString());
        return;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
    {
        m_loadError = QStringLiteral("设置文件损坏，已保留原文件并禁止覆盖。请修复后重新启动。");
        return;
    }
    m_document = document.object();
    // 路径所在磁盘暂时离线时仍允许打开设置并修改目录。
    const QVariantMap result = validateValues(data(), false);
    if (!result.value(QStringLiteral("ok")).toBool())
        m_loadError = QStringLiteral("设置文件内容无效，已禁止覆盖：%1").arg(result.value(QStringLiteral("error")).toString());
}

QVariantMap SettingsService::data() const
{
    // 原生模式以系统自启项为准，允许用户从系统中删除后重新启用。
    if (m_startupSettings)
        m_startupSettings->sync();
    return {
        {QStringLiteral("autoStart"), m_startupSettings
             ? QVariant(!m_startupSettings->value(QStringLiteral("DesktopTool")).toString().isEmpty())
             : m_document.contains(QStringLiteral("auto_start"))
                 ? m_document.value(QStringLiteral("auto_start")).toVariant() : QVariant(false)},
        {QStringLiteral("fontSize"), m_document.contains(QStringLiteral("tree_view_font_size"))
             ? m_document.value(QStringLiteral("tree_view_font_size")).toVariant() : QVariant(16)},
        {QStringLiteral("hotkeyModifier"), m_document.contains(QStringLiteral("hotkey_modifier"))
             ? m_document.value(QStringLiteral("hotkey_modifier")).toVariant() : QVariant(0)},
        {QStringLiteral("hotkeyKey"), m_document.contains(QStringLiteral("hotkey_key"))
             ? m_document.value(QStringLiteral("hotkey_key")).toVariant() : QVariant(0x77)},
        {QStringLiteral("screenshotHotkeyModifier"), m_document.contains(QStringLiteral("screenshot_hotkey_modifier"))
             ? m_document.value(QStringLiteral("screenshot_hotkey_modifier")).toVariant() : QVariant(3)},
        {QStringLiteral("screenshotHotkeyKey"), m_document.contains(QStringLiteral("screenshot_hotkey_key"))
             ? m_document.value(QStringLiteral("screenshot_hotkey_key")).toVariant() : QVariant(0x41)},
        {QStringLiteral("downloadDirectory"), m_document.contains(QStringLiteral("download_directory"))
             ? m_document.value(QStringLiteral("download_directory")).toVariant() : QVariant(defaultDownloadDirectory())}
    };
}

QVariantMap SettingsService::validate(const QVariantMap& settings)
{
    return validateValues(settings, true);
}

QVariantMap SettingsService::validateValues(const QVariantMap& settings, bool checkDirectory)
{
    if (settings.contains(QStringLiteral("autoStart"))
        && !QJsonValue::fromVariant(settings.value(QStringLiteral("autoStart"))).isBool())
        return ServiceResult::failure(QStringLiteral("开机自启必须为勾选或未勾选状态。"));
    if (!integerValue(settings.value(QStringLiteral("fontSize")), 8, 32))
        return ServiceResult::failure(QStringLiteral("字体大小必须为 8 至 32 的整数。"));
    if (!integerValue(settings.value(QStringLiteral("hotkeyModifier")), 0, 15))
        return ServiceResult::failure(QStringLiteral("热键修饰组合必须为 0 至 15 的整数。"));
    if (!integerValue(settings.value(QStringLiteral("hotkeyKey")), 1, 254))
        return ServiceResult::failure(QStringLiteral("热键必须使用有效的虚拟键码。"));
    const int key = settings.value(QStringLiteral("hotkeyKey")).toInt();
    if (isModifierOnlyKey(key))
        return ServiceResult::failure(QStringLiteral("热键不能只包含 Ctrl、Alt、Shift 或 Win 修饰键。"));
    if (!integerValue(settings.value(QStringLiteral("screenshotHotkeyModifier")), 0, 15))
        return ServiceResult::failure(QStringLiteral("截图热键修饰组合必须为 0 至 15 的整数。"));
    if (!integerValue(settings.value(QStringLiteral("screenshotHotkeyKey")), 1, 254))
        return ServiceResult::failure(QStringLiteral("截图热键必须使用有效的虚拟键码。"));
    if (isModifierOnlyKey(settings.value(QStringLiteral("screenshotHotkeyKey")).toInt()))
        return ServiceResult::failure(QStringLiteral("截图热键不能只包含 Ctrl、Alt、Shift 或 Win 修饰键。"));
    for (auto iterator = settings.cbegin(); iterator != settings.cend(); ++iterator)
    {
        if (iterator.key() != QStringLiteral("fontSize") && iterator.key() != QStringLiteral("hotkeyModifier")
            && iterator.key() != QStringLiteral("hotkeyKey") && iterator.key() != QStringLiteral("downloadDirectory")
            && iterator.key() != QStringLiteral("autoStart")
            && iterator.key() != QStringLiteral("screenshotHotkeyModifier")
            && iterator.key() != QStringLiteral("screenshotHotkeyKey"))
            return ServiceResult::failure(QStringLiteral("未知的设置字段：%1").arg(iterator.key()));
    }
    if (settings.contains(QStringLiteral("downloadDirectory"))
        && !QJsonValue::fromVariant(settings.value(QStringLiteral("downloadDirectory"))).isString())
        return ServiceResult::failure(QStringLiteral("下载目录必须是文件夹路径。"));
    QString directory = settings.value(QStringLiteral("downloadDirectory")).toString().trimmed();
    if (directory.isEmpty())
        directory = defaultDownloadDirectory();
    directory = QDir::fromNativeSeparators(directory);
    if (!QDir::isAbsolutePath(directory) || directory.contains(QChar::Null))
        return ServiceResult::failure(QStringLiteral("下载目录必须是有效的绝对路径，请使用目录选择按钮。"));
    directory = QDir::cleanPath(directory);
    if (checkDirectory) {
        const QString directoryError = checkDownloadDirectory(directory);
        if (!directoryError.isEmpty())
            return ServiceResult::failure(directoryError);
    }
    return ServiceResult::success(QVariantMap{
        {QStringLiteral("autoStart"), settings.value(QStringLiteral("autoStart"), false).toBool()},
        {QStringLiteral("fontSize"), settings.value(QStringLiteral("fontSize")).toInt()},
        {QStringLiteral("hotkeyModifier"), settings.value(QStringLiteral("hotkeyModifier")).toInt()},
        {QStringLiteral("hotkeyKey"), key},
        {QStringLiteral("screenshotHotkeyModifier"), settings.value(QStringLiteral("screenshotHotkeyModifier")).toInt()},
        {QStringLiteral("screenshotHotkeyKey"), settings.value(QStringLiteral("screenshotHotkeyKey")).toInt()},
        {QStringLiteral("downloadDirectory"), directory}
    });
}

QVariantMap SettingsService::snapshot() const
{
    return m_loadError.isEmpty() ? validateValues(data(), false) : ServiceResult::failure(m_loadError);
}

QVariantMap SettingsService::save(const QVariantMap& settings)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    QVariantMap candidate = data();
    for (auto iterator = settings.cbegin(); iterator != settings.cend(); ++iterator)
        candidate.insert(iterator.key(), iterator.value());
    const QVariantMap validation = validate(candidate);
    if (!validation.value(QStringLiteral("ok")).toBool())
        return validation;
    candidate = validation.value(QStringLiteral("data")).toMap();
    QJsonObject document = m_document;
    document.insert(QStringLiteral("auto_start"), candidate.value(QStringLiteral("autoStart")).toBool());
    document.insert(QStringLiteral("tree_view_font_size"), candidate.value(QStringLiteral("fontSize")).toInt());
    document.insert(QStringLiteral("hotkey_modifier"), candidate.value(QStringLiteral("hotkeyModifier")).toInt());
    document.insert(QStringLiteral("hotkey_key"), candidate.value(QStringLiteral("hotkeyKey")).toInt());
    document.insert(QStringLiteral("screenshot_hotkey_modifier"), candidate.value(QStringLiteral("screenshotHotkeyModifier")).toInt());
    document.insert(QStringLiteral("screenshot_hotkey_key"), candidate.value(QStringLiteral("screenshotHotkeyKey")).toInt());
    document.insert(QStringLiteral("download_directory"), candidate.value(QStringLiteral("downloadDirectory")).toString());
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath()))
        return ServiceResult::failure(QStringLiteral("无法创建设置目录。"));
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly))
        return ServiceResult::failure(QStringLiteral("无法写入设置：%1").arg(file.errorString()));
    const QByteArray payload = QJsonDocument(document).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size())
        return ServiceResult::failure(QStringLiteral("保存设置失败，原设置保持不变：%1").arg(file.errorString()));
    const QVariant previousStartup = m_startupSettings
        ? m_startupSettings->value(QStringLiteral("DesktopTool")) : QVariant();
    const QVariant nextStartup = candidate.value(QStringLiteral("autoStart")).toBool()
        ? QVariant(m_startupCommand) : QVariant();
    const bool changeStartup = m_startupSettings && previousStartup != nextStartup;
    if (changeStartup && !writeStartupValue(m_startupSettings, nextStartup)) {
        const bool restored = writeStartupValue(m_startupSettings, previousStartup);
        return ServiceResult::failure(restored
            ? QStringLiteral("无法修改开机自启项，请检查当前用户的注册表写入权限。设置未保存。")
            : QStringLiteral("无法修改或恢复开机自启项，请检查系统启动项及注册表写入权限。设置未保存。"));
    }
    if (!file.commit()) {
        const bool restored = !changeStartup || writeStartupValue(m_startupSettings, previousStartup);
        return ServiceResult::failure(restored
            ? QStringLiteral("保存设置失败，原设置保持不变：%1").arg(file.errorString())
            : QStringLiteral("保存设置失败，且无法恢复开机自启项，请检查系统启动项：%1").arg(file.errorString()));
    }
    m_document = document;
    emit changed();
    return ServiceResult::success(candidate);
}
