#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QVariantMap>
#include <functional>

// 一个被索引卷的基本信息；mftMode 表示使用 NTFS USN 管线。
struct FileSearchVolume
{
    int driveIndex = 0;        // 1-255，参与记录键高位，保证跨盘唯一。
    QString displayRoot;       // 路径前缀，如 "C:" 或测试注入的目录绝对路径。
    QString fileSystem;        // NTFS、exFAT 或 "DIR"（测试注入目录）。
    bool mftMode = false;      // 是否使用 USN 枚举与日志监控。
    quint64 journalId = 0;     // USN 日志标识（仅 mftMode）。
    quint64 serialNumber = 0; // 卷序列号，防止同盘符换盘时复用旧索引。
    quint64 nextUsn = 0;       // 增量读取断点（仅 mftMode）。
};

// 一条文件名索引记录；沿父记录键上溯即可拼出完整路径。
struct FileSearchRecord
{
    quint64 parent = 0;
    QString name;
    // 指纹、序号与类型复用结构体原有填充空间，不增加每条记录大小。
    quint32 nameMask = 0;
    // NTFS 引用号高 16 位序号，区分同一 MFT 槽位被重复利用的文件。
    quint16 sequence = 0;
    // 记录是否为目录。
    bool isDirectory = false;
};

// 纯内存文件名索引与查询：不含线程与系统调用，可独立测试。
class FileSearchIndex final
{
public:
    // 记录键：高 8 位为卷序号，低 56 位为文件引用号或扫描哈希。
    static quint64 key(int driveIndex, quint64 frn);
    // 扫描模式按完整路径（大小写不敏感）生成稳定键。
    static quint64 scanKey(int driveIndex, const QString& nativePath);

    void clear();
    // 校验并应用 V2 USN 数据批次；重命名旧记录不会删除子树，删除优先。
    bool applyJournalRecords(int drive, const QByteArray& bytes, bool enumeration, QString* error);
    // 插入或更新记录；父记录尚未存在时也允许（稍后由更新补齐）。
    void upsert(quint64 key, quint64 parentKey, const QString& name, bool isDirectory);
    // 删除记录及全部子孙，返回实际删除的键。
    QVector<quint64> remove(const QVector<quint64>& keys);
    bool contains(quint64 key) const;
    const FileSearchRecord* record(quint64 key) const;
    int size() const { return m_records.size(); }
    int fileCount() const { return m_fileCount; }
    int directoryCount() const { return m_directoryCount; }
    // 直接子记录键列表（一次全表扫描）。
    QVector<quint64> childKeys(quint64 parentKey) const;
    // 返回属于指定卷的全部记录键（换盘或失效时整卷清除用）。
    QVector<quint64> volumeKeys(int driveIndex) const;
    // 清理父记录缺失的孤儿（元数据子项、失效重解析点等）。
    void dropOrphans();
    // 拼接完整路径；父链断裂时返回空串。
    QString pathOf(quint64 key) const;

    // 查询：空格分隔 AND 词元；支持 * ? 通配与 ext:/folder:/file: 过滤；
    // 整体 path: 前缀或调用方传入 searchPath 时按完整路径匹配。
    struct Query
    {
        QString text;
        bool searchPath = false;
        QVector<QString> substrings;
        QVector<QString> wildcards;
        QVector<QString> extensions;
        bool foldersOnly = false;
        bool filesOnly = false;
        bool isEmpty() const;
        static Query parse(const QString& raw, bool searchPath = false);
    };

    // 执行检索：命中按完整路径排序后返回 offset 页；total 为全部命中数，
    // collected 为实际参与排序的命中数（受 CollectionCap 限制）。
    QVector<quint64> search(const Query& query, int offset, int limit,
                            int* total = nullptr, int* collected = nullptr,
                            const std::function<bool()>& cancelled = {}) const;

    // 命中过多时只对前若干条排序展示，防止界面卡顿。
    static constexpr int CollectionCap = 8000;

    // 索引与卷信息持久化；save/load 均为整体读写。
    static bool save(const QString& filePath, const QVector<FileSearchVolume>& volumes,
                     const FileSearchIndex& index, QString* error);
    static bool load(const QString& filePath, QVector<FileSearchVolume>* volumes,
                     FileSearchIndex* index, QString* error);

private:
    // 计算大小写折叠后的三字符指纹；代理字符返回全位以避免误排除。
    static quint32 nameMask(const QString& text);
    // 最近一次查询的分页缓存，索引改变即失效。
    mutable QVector<quint64> m_cachedHits;
    mutable Query m_cachedQuery;
    mutable int m_cachedTotal = 0;
    mutable quint64 m_cachedRevision = ~quint64(0);
    // 索引变更代数，不改变内容的重复事件不会使缓存失效。
    quint64 m_revision = 0;
    // 词元是否全部命中（子串包含或通配整名）。
    static bool matchTokens(const QString& text, const Query& query);
    // 不区分大小写的通配符匹配（* 任意长度、? 单字符）。
    static bool wildcardMatch(const QString& text, const QString& pattern);

    QHash<quint64, FileSearchRecord> m_records;
    int m_fileCount = 0;
    int m_directoryCount = 0;
};
