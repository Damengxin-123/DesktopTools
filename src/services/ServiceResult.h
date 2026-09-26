#pragma once

#include <QString>
#include <QVariantMap>

// 统一服务调用结果，使网页和原生调用方使用相同的错误处理协议。
namespace ServiceResult {
// 包装成功数据；data 只使用可由 WebChannel 序列化的 Qt 值类型。
inline QVariantMap success(const QVariant& data = {})
{
    return {{QStringLiteral("ok"), true}, {QStringLiteral("data"), data},
            {QStringLiteral("error"), QString()}};
}

// 包装可直接显示给用户的中文错误。
inline QVariantMap failure(const QString& error)
{
    return {{QStringLiteral("ok"), false}, {QStringLiteral("data"), QVariant()},
            {QStringLiteral("error"), error}};
}
}
