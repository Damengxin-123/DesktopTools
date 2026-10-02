#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

class QSaveFile;

// 管理网格图项目索引与每个项目的栅格数据；坐标以画布中心为原点，只保存被涂色的格子。
class GridMapService final : public QObject
{
    Q_OBJECT

public:
    // 从指定数据根目录加载项目索引。
    explicit GridMapService(const QString& dataRoot, QObject* parent = nullptr);
    // 返回项目摘要列表。
    QVariantMap snapshot() const;
    // 新建空白网格图项目。
    QVariantMap create(const QString& title);
    // 重命名项目。
    QVariantMap rename(const QString& id, const QString& title);
    // 批量删除项目及其栅格数据文件。
    QVariantMap remove(const QStringList& ids);
    // 读取项目的完整栅格与线条设置。
    QVariantMap read(const QString& id) const;
    // 保存栅格颜色、网格线颜色和粗细，并刷新更新时间。
    QVariantMap save(const QString& id, const QVariantMap& data);

signals:
    // 成功提交索引或项目数据后通知界面刷新。
    void changed();

private:
    // 索引中的项目摘要。
    struct Item {
        QString id;        // 项目的稳定标识。
        QString title;     // 显示名称。
        QString updatedAt; // 最近更新时间，采用 ISO 8601 格式。
    };

    // 校验并加载项目索引，损坏时停止写入以保护原数据。
    void load();
    // 校验路径位于数据根目录中，且不经过链接或联接点。
    bool safePath(const QString& path) const;
    // 校验用于磁盘路径的单个片段。
    static bool safeSegment(const QString& name);
    // 生成只含十六进制字符的稳定项目标识。
    static QString newId();
    // 将项目记录转换为公开摘要。
    static QVariantMap summary(const Item& item);
    // 返回项目位置；不存在时返回负数。
    int position(const QString& id) const;
    // 项目数据文件的绝对路径。
    QString mapPath(const QString& id) const;
    // 写入临时索引文件，调用方负责最终提交。
    bool prepareIndex(QSaveFile& file, const QVector<Item>& items, QString& error) const;
    // 原子持久化索引并替换内存状态。
    QVariantMap commitItems(const QVector<Item>& items);
    // 校验保存请求中的格子坐标与颜色，输出规范化的三元组列表。
    QVariantList normalizeCells(const QVariantList& cells) const;
    // 校验线条设置，输出规范化后的网格线颜色与粗细。
    bool normalizeStyle(const QVariantMap& data, QString* lineColor, int* lineWidth) const;

    QString m_dataRoot;           // 调用方指定的数据根目录。
    QString m_root;               // 网格图数据目录。
    QString m_indexPath;          // 项目索引路径。
    QVector<Item> m_items;        // 按创建顺序保存的项目。
    QStringList m_warnings;       // 可恢复问题的说明。
    QString m_loadError;          // 索引损坏等不可覆盖错误。
};
