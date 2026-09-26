#pragma once

#include <QJsonObject>
#include <QObject>
#include <QVariantMap>

// 管理全局字体和热键配置，并兼容原有设置文件。
class SettingsService final : public QObject
{
    Q_OBJECT

public:
    // 加载指定数据根目录下的配置，缺少配置时使用默认值。
    explicit SettingsService(const QString& dataRoot, QObject* parent = nullptr);
    // 获取网页使用的设置字段。
    QVariantMap snapshot() const;
    // 校验完整设置并返回规范化字段，供热键注册事务提前检查。
    static QVariantMap validate(const QVariantMap& settings);
    // 校验并原子保存设置，同时保留文件内未识别的字段。
    QVariantMap save(const QVariantMap& settings);

signals:
    // 设置成功写入后通知界面更新。
    void changed();

private:
    // 把保留旧版键名的配置转换为网页设置字段。
    QVariantMap data() const;

    QString m_path; // 系统设置文件的绝对路径。
    QString m_loadError; // 加载损坏文件后的只读保护错误。
    QJsonObject m_document; // 原始配置对象，保存时保留额外字段。
};
