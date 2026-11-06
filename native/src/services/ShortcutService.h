#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

// 管理快捷方式及分类，独立于网页和原生控件保存业务数据。
class ShortcutService final : public QObject
{
    Q_OBJECT

public:
    // 从指定数据目录加载新版配置，必要时导入旧版配置。
    explicit ShortcutService(const QString& dataRoot, QObject* parent = nullptr);
    // 获取分类、快捷方式和旧数据导入警告。
    QVariantMap snapshot() const;
    // 新增或重命名分类；空标识表示新增。
    QVariantMap saveCategory(const QString& id, const QString& name);
    // 删除分类并把条目原子迁移至默认分类，有冲突时拒绝删除。
    QVariantMap removeCategory(const QString& id);
    // 新增或更新快捷方式，校验分类、类型及分类内重复项。
    QVariantMap saveShortcut(const QVariantMap& item);
    // 原子删除一批快捷方式，任一标识不存在时不修改数据。
    QVariantMap removeShortcuts(const QStringList& ids);
    // 把条目移到指定分类和指定条目前；空参照标识表示分类末尾。
    QVariantMap moveShortcut(const QString& id, const QString& categoryId, const QString& beforeId);
    // 按存储的条目标识调用系统打开目标。
    QVariantMap openShortcut(const QString& id);

signals:
    // 配置成功写入后通知界面重新读取快照。
    void changed();

private:
    // 分类的稳定标识和显示名称。
    struct Category
    {
        QString id; // 分类稳定标识，默认分类固定为 default。
        QString name; // 分类显示名称。
    };
    // 一条快捷方式的持久化业务字段。
    struct Shortcut
    {
        QString id; // 不随排序和重命名变化的稳定标识。
        QString categoryId; // 所属分类标识。
        QString title; // 用户填写的显示标题。
        QString target; // 本地路径或网址。
        int type = 0; // 目标类型：0 目录、1 网址、2 文件。
    };
    // 读取新版配置或执行一次旧数据导入。
    void load();
    // 从旧版逗号分隔文本导入可无歧义识别的条目。
    void importLegacy(const QString& path);
    // 校验分类、引用、标识、类型和重复规则。
    QString validate(const QVector<Category>& categories, const QVector<Shortcut>& items) const;
    // 通过临时文件原子保存，成功后才替换内存数据。
    QVariantMap commit(const QVector<Category>& categories, const QVector<Shortcut>& items);
    // 将当前业务数据转换为网页可接收的快照。
    QVariantMap data() const;

    QString m_path; // 新版 JSON 配置的绝对路径。
    QString m_legacyPath; // 仅用于首次导入的旧配置路径。
    QString m_loadError; // 加载失败后用于禁止覆盖原始配置的错误。
    QStringList m_warnings; // 导入时无法恢复的原始行的中文说明。
    QVector<Category> m_categories; // 按显示顺序排列的分类。
    QVector<Shortcut> m_items; // 按分类内显示顺序保存的快捷方式。
};
