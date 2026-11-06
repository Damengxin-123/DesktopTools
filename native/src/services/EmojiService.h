#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

class ClipboardService;

// 管理表情图像文件的分类、检索关键字和缩略图；仅保存原文件引用，不复制文件实体。
class EmojiService final : public QObject
{
    Q_OBJECT
public:
    // 从数据目录加载表情库；剪贴板服务用于复制文件时屏蔽历史记录，测试可为空。
    explicit EmojiService(const QString& dataRoot, ClipboardService* clipboard = nullptr, QObject* parent = nullptr);
    // 返回分类和表情列表，可按关键字过滤文件名与检索词。
    QVariantMap snapshot(const QString& query = {}) const;
    // 添加本地图像文件，生成缩略图并保存检索关键字。
    QVariantMap add(const QVariantMap& item);
    // 修改表情的检索关键字和所属分类。
    QVariantMap save(const QVariantMap& item);
    // 批量移除表情记录，不删除原文件。
    QVariantMap remove(const QStringList& ids);
    // 新增或重命名分类；空标识表示新增。
    QVariantMap saveCategory(const QString& id, const QString& name);
    // 删除分类并把其中的表情移入默认分类。
    QVariantMap removeCategory(const QString& id);
    // 打开原文件所在目录并选中该文件，原文件丢失时返回中文错误。
    QVariantMap openDirectory(const QString& id);
    // 复制原文件到系统剪贴板，不触发剪贴板历史记录。
    QVariantMap copyFile(const QString& id);
signals:
    // 表情库或分类成功写入后通知界面刷新。
    void changed();
private:
    // 分类的稳定标识和显示名称。
    struct Category
    {
        QString id; // 分类稳定标识，默认分类固定为 default。
        QString name; // 分类显示名称。
    };
    // 一个表情的持久化业务字段。
    struct Emoji
    {
        QString id; // 不随修改变化的稳定标识。
        QString categoryId; // 所属分类标识。
        QString name; // 显示名称，取原文件名。
        QString path; // 原文件的绝对路径，仅作引用。
        QStringList keywords; // 检索关键字。
        QString addedAt; // 添加时间。
        int width = 0; // 原图像素宽度。
        int height = 0; // 原图像素高度。
        QString thumbnail; // 缩略图数据地址，用于列表显示。
    };
    // 校验并加载表情库，损坏时停止写入以保护原数据。
    void load();
    // 校验分类和表情条目的结构规则。
    QString validate(const QVector<Category>& categories, const QVector<Emoji>& items) const;
    // 通过临时文件原子保存，成功后才替换内存数据。
    QVariantMap commit(const QVector<Category>& categories, const QVector<Emoji>& items);
    // 将当前业务数据转换为网页可接收的快照。
    QVariantMap data(const QString& query) const;
    // 将单个表情转换为可序列化字段。
    static QVariantMap emojiMap(const Emoji& item);
    // 返回条目位置，不存在时返回负数。
    qsizetype position(const QString& id) const;
    // 读取受限尺寸的原始图像并生成缩略图数据地址。
    static QString buildThumbnail(const QString& path, int* width, int* height);

    QString m_path; // 表情库索引的绝对路径。
    QString m_loadError; // 加载错误，存在时拒绝覆盖。
    QVector<Category> m_categories; // 按显示顺序排列的分类。
    QVector<Emoji> m_items; // 最近添加排在前面的表情。
    ClipboardService* m_clipboard; // 剪贴板服务，复制文件时使用，可为空。
};
