#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QStringList>

// 录屏历史：MP4 实体保存在数据目录的 recordings 子目录，索引保存元数据与首帧缩略图。
// 视频体积远大于截图，实体永不内联进索引；录制方先 reserve() 取得输出路径，
// 编码成功后用 add() 登记，取消或失败时自行删除半成品文件。
class RecordingService final : public QObject
{
    Q_OBJECT
public:
    // 从指定数据根目录加载历史索引。
    explicit RecordingService(const QString& dataRoot, QObject* parent = nullptr);
    // 返回历史摘要；列表直接使用随索引保存的首帧缩略图。
    QVariantMap snapshot() const;
    // 预约一个输出文件路径（确保目录存在）；录制完成后须调用 add 登记。
    QVariantMap reserve();
    // 登记一段已完成的录制；path 必须是 reserve() 返回的路径且文件已存在。
    // meta 支持 recordedAt（ISO 8601）、width、height、durationMs 与可选 thumbnail。
    QVariantMap add(const QString& path, const QVariantMap& meta);
    // 读取完整记录；包含磁盘路径，供打开目录与复制文件。
    QVariantMap read(const QString& id) const;
    // 批量删除历史记录，并删除对应的视频文件。
    QVariantMap remove(const QStringList& ids);
    // 清空全部历史与文件。
    QVariantMap clear();
    // 用资源管理器打开记录所在目录并选中文件。
    QVariantMap openDirectory(const QString& id) const;
signals:
    // 历史（新增、删除或清空）发生变化。
    void changed();
private:
    // 索引中的单条记录。
    struct Item {
        QString id;        // 稳定标识，同时是 MP4 文件名。
        QString recordedAt; // 录制完成时间，ISO 8601 格式。
        QString file;      // 视频文件名（不含路径）。
        qint64 bytes = 0;  // 视频文件大小。
        int width = 0;     // 视频宽度（设备像素）。
        int height = 0;    // 视频高度（设备像素）。
        qint64 durationMs = 0; // 有效录制时长（不含暂停）。
        QString thumbnail; // 首帧缩略图数据地址，录制极短时允许为空。
    };

    // 校验并加载索引，损坏时停止写入以保护原文件。
    void load();
    // 校验路径位于数据根目录中，且不经过链接或联接点。
    bool safePath(const QString& path) const;
    // 原子写入索引；成功后更新内存并发出变更信号。
    QVariantMap commit(QVector<Item> items);
    // 删除指定记录的视频文件，索引提交成功后调用。
    void removeFiles(const QVector<Item>& items);
    // 把记录转换为网页摘要。
    static QVariantMap summary(const Item& item);
    // 返回记录位置；不存在时返回负数。
    int position(const QVector<Item>& items, const QString& id) const;

    QString m_dataRoot;  // 调用方指定的数据根目录。
    QString m_root;      // 录屏数据目录。
    QString m_indexPath; // 索引路径。
    QVector<Item> m_items; // 最近录制排在前面的记录。
    QString m_loadError; // 索引损坏等不可覆盖错误。
};
