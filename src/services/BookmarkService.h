#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include <memory>

class QNetworkAccessManager;
class QSaveFile;

/** 负责网页书签的索引持久化、排序与网址可用性检测，不依赖任何界面控件。 */
class BookmarkService final : public QObject
{
    Q_OBJECT

public:
    /** 从指定数据根目录加载书签索引。 */
    explicit BookmarkService(const QString& dataRoot, QObject* parent = nullptr);
    /** 返回书签列表摘要与迁移警告。 */
    QVariantMap snapshot() const;
    /** 新建或更新书签；空标识表示新建。 */
    QVariantMap save(const QVariantMap& item);
    /** 批量移除书签索引记录。 */
    QVariantMap remove(const QStringList& ids);
    /** 移动书签到指定书签之前；空目标表示末尾。 */
    QVariantMap move(const QString& id, const QString& beforeId);
    /** 异步检测网址是否可访问并读取网页标题，结果通过 probeFinished 返回。 */
    QVariantMap probe(const QString& address);
    /** 规范化用户输入为可用于加载的地址，失败时返回可直接显示的原因。 */
    static QVariantMap normalize(const QString& address);

signals:
    /** 成功提交索引变更后通知界面刷新。 */
    void changed();
    /** 一次网址检测结束，携带本次令牌、标题与错误信息。 */
    void probeFinished(const QVariantMap& result);

private:
    /** 持久化的书签记录。 */
    struct Item {
        QString id;        ///< 书签的稳定标识。
        QString name;      ///< 显示名称，默认取网页标题。
        QString url;       ///< 规范化后的网页地址。
        QString createdAt; ///< 创建时间，采用 ISO 8601 格式。
        QString updatedAt; ///< 最近更新时间，采用 ISO 8601 格式。
    };
    /** 一次网址检测的共享状态，可在多个 Qt 连接间累积响应。 */
    struct ProbeState {
        QByteArray buffer;      ///< 已接收的响应正文，用于尽早解析标题。
        bool exhausted = false; ///< 已取得标题或达到体积上限，主动中断剩余下载。
        bool timedOut = false;  ///< 等待超时后被中断。
    };

    /** 加载索引；损坏时保留文件并进入只读保护。 */
    void load();
    /** 写入临时索引文件，调用方负责最终提交。 */
    bool prepareIndex(QSaveFile& file, const QVector<Item>& items, QString& error) const;
    /** 原子持久化索引并替换内存状态。 */
    QVariantMap commit(const QVector<Item>& items);
    /** 返回书签位置；不存在时返回负数。 */
    int position(const QString& id) const;
    /** 解析 HTML 正文中的标题文本；无法识别时返回空。 */
    static QString htmlTitle(const QByteArray& bytes, const QString& contentType);
    /** 抽取响应正文中的字符编码名称。 */
    static QString charsetName(const QByteArray& bytes, const QString& contentType);
    /** 把 HTML 实体还原为文本字符。 */
    static QString decodeEntities(const QString& text);
    /** 把字节序列按指定编码解码，失败时回退 UTF-8。 */
    static QString decodeBytes(const QByteArray& bytes, const QString& charset);

    QString m_bookmarkRoot;  ///< 书签数据目录。
    QString m_indexPath;     ///< 书签索引路径。
    QVector<Item> m_items;   ///< 按显示顺序保存的书签。
    QStringList m_warnings;  ///< 加载时可恢复问题的说明。
    QString m_loadError;     ///< 索引损坏等不可覆盖错误。
    QNetworkAccessManager* m_network; ///< 网址检测使用的网络管理器。
    int m_probeToken = 0;    ///< 检测序号，使界面可忽略过期结果。
};
