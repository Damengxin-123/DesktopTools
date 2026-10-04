#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QStringList>

class QImage;
class QSaveFile;

// 截图历史：PNG 实体保存在数据目录的 screenshots 子目录，索引保存元数据与小缩略图。
// 大图不以 base64 内联进索引，避免历史文件膨胀。
class ScreenshotService final : public QObject
{
    Q_OBJECT
public:
    // 从指定数据根目录加载历史索引。
    explicit ScreenshotService(const QString& dataRoot, QObject* parent = nullptr);
    // 返回历史摘要；列表直接使用随索引保存的缩略图。
    QVariantMap snapshot() const;
    // 保存一张截图；返回完整记录（不含原图数据地址）。
    QVariantMap add(const QImage& image, const QString& capturedAtIso);
    // 读取完整记录；包含原图的数据地址与磁盘路径，供预览和打开目录。
    QVariantMap read(const QString& id) const;
    // 批量删除历史记录，并删除对应的 PNG 文件。
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
        QString id;        // 稳定标识，同时是 PNG 文件名。
        QString capturedAt; // 截图时间，ISO 8601 格式。
        QString file;      // PNG 文件名（不含路径）。
        qint64 bytes = 0;  // PNG 文件大小。
        int width = 0;     // 图像宽度（设备像素）。
        int height = 0;    // 图像高度（设备像素）。
        QString thumbnail; // 缩略图数据地址。
    };

    // 校验并加载索引，损坏时停止写入以保护原文件。
    void load();
    // 校验路径位于数据根目录中，且不经过链接或联接点。
    bool safePath(const QString& path) const;
    // 原子写入索引；成功后更新内存并发出变更信号。
    QVariantMap commit(QVector<Item> items);
    // 删除指定记录的 PNG 文件，索引提交成功后调用。
    void removeFiles(const QVector<Item>& items);
    // 把记录转换为网页摘要。
    static QVariantMap summary(const Item& item);
    // 返回记录位置；不存在时返回负数。
    int position(const QVector<Item>& items, const QString& id) const;

    QString m_dataRoot;  // 调用方指定的数据根目录。
    QString m_root;      // 截图数据目录。
    QString m_indexPath; // 索引路径。
    QVector<Item> m_items; // 最近截图排在前面的记录。
    QString m_loadError; // 索引损坏等不可覆盖错误。
};
