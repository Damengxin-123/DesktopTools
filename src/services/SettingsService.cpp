#include "SettingsService.h"
#include "ServiceResult.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>

#include <cmath>

namespace
{
// 只接受有限整数，避免小数或字符串被静默转换为设置值。
bool integerValue(const QVariant& value, int minimum, int maximum)
{
    const QJsonValue number = QJsonValue::fromVariant(value);
    const double result = number.toDouble();
    return number.isDouble() && std::isfinite(result) && std::floor(result) == result
        && result >= minimum && result <= maximum;
}
}

SettingsService::SettingsService(const QString& dataRoot, QObject* parent)
    : QObject(parent)
    , m_path(QDir(dataRoot).filePath(QStringLiteral("setting/system_config.json")))
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
    const QVariantMap result = validate(data());
    if (!result.value(QStringLiteral("ok")).toBool())
        m_loadError = QStringLiteral("设置文件内容无效，已禁止覆盖：%1").arg(result.value(QStringLiteral("error")).toString());
}

QVariantMap SettingsService::data() const
{
    return {
        {QStringLiteral("fontSize"), m_document.contains(QStringLiteral("tree_view_font_size"))
             ? m_document.value(QStringLiteral("tree_view_font_size")).toVariant() : QVariant(16)},
        {QStringLiteral("hotkeyModifier"), m_document.contains(QStringLiteral("hotkey_modifier"))
             ? m_document.value(QStringLiteral("hotkey_modifier")).toVariant() : QVariant(0)},
        {QStringLiteral("hotkeyKey"), m_document.contains(QStringLiteral("hotkey_key"))
             ? m_document.value(QStringLiteral("hotkey_key")).toVariant() : QVariant(0x77)}
    };
}

QVariantMap SettingsService::validate(const QVariantMap& settings)
{
    if (!integerValue(settings.value(QStringLiteral("fontSize")), 8, 32))
        return ServiceResult::failure(QStringLiteral("字体大小必须为 8 至 32 的整数。"));
    if (!integerValue(settings.value(QStringLiteral("hotkeyModifier")), 0, 15))
        return ServiceResult::failure(QStringLiteral("热键修饰组合必须为 0 至 15 的整数。"));
    if (!integerValue(settings.value(QStringLiteral("hotkeyKey")), 1, 254))
        return ServiceResult::failure(QStringLiteral("热键必须使用有效的虚拟键码。"));
    const int key = settings.value(QStringLiteral("hotkeyKey")).toInt();
    if (key == 0x10 || key == 0x11 || key == 0x12 || key == 0x5B || key == 0x5C
        || (key >= 0xA0 && key <= 0xA5))
        return ServiceResult::failure(QStringLiteral("热键不能只包含 Ctrl、Alt、Shift 或 Win 修饰键。"));
    for (auto iterator = settings.cbegin(); iterator != settings.cend(); ++iterator)
    {
        if (iterator.key() != QStringLiteral("fontSize") && iterator.key() != QStringLiteral("hotkeyModifier")
            && iterator.key() != QStringLiteral("hotkeyKey"))
            return ServiceResult::failure(QStringLiteral("未知的设置字段：%1").arg(iterator.key()));
    }
    return ServiceResult::success(QVariantMap{
        {QStringLiteral("fontSize"), settings.value(QStringLiteral("fontSize")).toInt()},
        {QStringLiteral("hotkeyModifier"), settings.value(QStringLiteral("hotkeyModifier")).toInt()},
        {QStringLiteral("hotkeyKey"), key}
    });
}

QVariantMap SettingsService::snapshot() const
{
    return m_loadError.isEmpty() ? ServiceResult::success(data()) : ServiceResult::failure(m_loadError);
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
    document.insert(QStringLiteral("tree_view_font_size"), candidate.value(QStringLiteral("fontSize")).toInt());
    document.insert(QStringLiteral("hotkey_modifier"), candidate.value(QStringLiteral("hotkeyModifier")).toInt());
    document.insert(QStringLiteral("hotkey_key"), candidate.value(QStringLiteral("hotkeyKey")).toInt());
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath()))
        return ServiceResult::failure(QStringLiteral("无法创建设置目录。"));
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly))
        return ServiceResult::failure(QStringLiteral("无法写入设置：%1").arg(file.errorString()));
    const QByteArray payload = QJsonDocument(document).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size() || !file.commit())
        return ServiceResult::failure(QStringLiteral("保存设置失败，原设置保持不变：%1").arg(file.errorString()));
    m_document = document;
    emit changed();
    return ServiceResult::success(candidate);
}
