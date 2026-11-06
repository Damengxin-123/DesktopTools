#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

class QSaveFile;

/** 负责便签分类、内容和旧格式迁移，不依赖任何界面控件。 */
class NoteService final : public QObject
{
    Q_OBJECT

public:
    /** 从指定数据根目录加载或迁移便签索引。 */
    explicit NoteService(const QString& dataRoot, QObject* parent = nullptr);
    /** 返回分类与便签摘要。 */
    QVariantMap snapshot() const;
    /** 新建或重命名分类；空标识表示新建。 */
    QVariantMap saveCategory(const QString& id, const QString& name);
    /** 移除分类，将其中便签移入默认分类。 */
    QVariantMap removeCategory(const QString& id);
    /** 返回便签内容，将本地图片转为可直接显示的数据地址。 */
    QVariantMap readNote(const QString& id) const;
    /** 新建或更新便签，保留稳定标识及旧格式原件。 */
    QVariantMap saveNote(const QVariantMap& note);
    /** 批量移除索引记录，保留磁盘内容以便人工恢复。 */
    QVariantMap removeNotes(const QStringList& ids);
    /** 移动便签到分类，并放到指定便签之前；空目标表示末尾。 */
    QVariantMap moveNote(const QString& id, const QString& categoryId, const QString& beforeId);

signals:
    /** 成功提交索引变更后通知界面刷新。 */
    void changed();

private:
    /** 持久化的便签分类。 */
    struct Category {
        QString id;   ///< 分类的稳定标识。
        QString name; ///< 分类显示名称。
    };
    /** 持久化的便签摘要与受控内容目录。 */
    struct Note {
        QString id;         ///< 便签的稳定标识。
        QString categoryId; ///< 所属分类标识。
        QString title;      ///< 显示标题，与磁盘目录无关。
        QString updatedAt;  ///< 最近更新时间，采用 ISO 8601 格式。
        QString directory;  ///< note 根目录内的单层目录名称。
    };

    /** 加载新版索引，或首次导入旧目录。 */
    void load();
    /** 首次导入旧分类与便签，保存迁移索引以避免重复导入。 */
    void importLegacy();
    /** 校验路径位于数据根目录中，且不经过链接或联接点。 */
    bool safePath(const QString& path) const;
    /** 校验用于磁盘目录的单个路径片段。 */
    static bool safeDirectoryName(const QString& name);
    /** 将内部便签记录转换为公开摘要。 */
    static QVariantMap summary(const Note& note);
    /** 检查分类是否存在。 */
    bool hasCategory(const QString& id) const;
    /** 返回便签位置；不存在时返回负数。 */
    int notePosition(const QString& id) const;
    /** 写入临时索引文件，调用方负责最终提交。 */
    bool prepareIndex(QSaveFile& file, const QVector<Category>& categories,
                      const QVector<Note>& notes, QString& error) const;
    /** 原子持久化索引并替换内存状态。 */
    QVariantMap commit(const QVector<Category>& categories, const QVector<Note>& notes);
    /** 清洗并嵌入受控图片；传入错误接收参数时拒绝丢失任何图片的保存。 */
    QString safeHtml(const QString& html, const QString& noteDirectory, QString* error = nullptr) const;
    /** 将允许的本地图片或图片数据地址转换为 PNG 数据地址。 */
    QString imageDataUrl(const QString& source, const QString& noteDirectory) const;

    QString m_dataRoot;           ///< 调用方指定的数据根目录。
    QString m_noteRoot;           ///< 便签数据目录。
    QString m_indexPath;          ///< 新版便签索引路径。
    QVector<Category> m_categories; ///< 已加载的分类。
    QVector<Note> m_notes;        ///< 按显示顺序保存的便签。
    QStringList m_warnings;       ///< 迁移时可恢复问题的说明。
    QString m_loadError;          ///< 新版索引损坏等不可覆盖错误。
};
