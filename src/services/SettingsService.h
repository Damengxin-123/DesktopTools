#pragma once

#include <QJsonObject>
#include <QObject>
#include <QVariantMap>

// 管理全局字体、热键和下载目录配置，并兼容原有设置文件。
class SettingsService final : public QObject
{
    Q_OBJECT

public:
    // 加载指定数据根目录下的配置，缺少配置时使用默认值。
    explicit SettingsService(const QString& dataRoot, QObject* parent = nullptr);
    // 获取网页使用的设置字段。
    QVariantMap snapshot() const;
    // 校验完整设置并探测下载目录可写性，兼容未传下载目录的旧调用方。
    static QVariantMap validate(const QVariantMap& settings);
    // 校验并原子保存设置，同时保留文件内未识别的字段。
    QVariantMap save(const QVariantMap& settings);

signals:
    // 设置成功写入后通知界面更新。
    void changed();

private:
    // 加载时仅检查值格式，保存时进一步检查目录存在性与写入权限。
    static QVariantMap validateValues(const QVariantMap& settings, bool checkDirectory);
    // 把保留旧版键名的配置转换为网页设置字段。
    QVariantMap data() const;

    QString m_path; // 系统设置文件的绝对路径。
    QString m_loadError; // 加载损坏文件后的只读保护错误。
    QJsonObject m_document; // 原始配置对象，保存时保留额外字段。
};
