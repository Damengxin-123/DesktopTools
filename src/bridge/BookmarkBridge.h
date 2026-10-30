#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantMap>

class BookmarkService;

/** 书签窗口左侧面板页的唯一原生接口：书签增删查改、网址检测与右侧加载请求。 */
class BookmarkBridge final : public QObject
{
    Q_OBJECT
public:
    explicit BookmarkBridge(BookmarkService* service, QObject* parent = nullptr);

    /** 返回书签列表。 */
    Q_INVOKABLE QVariantMap getBookmarks() const;
    /** 新建或更新书签；空标识表示新建。 */
    Q_INVOKABLE QVariantMap saveBookmark(const QVariantMap& item);
    /** 批量删除书签。 */
    Q_INVOKABLE QVariantMap deleteBookmarks(const QStringList& ids);
    /** 调整书签顺序；空目标表示移到末尾。 */
    Q_INVOKABLE QVariantMap moveBookmark(const QString& id, const QString& beforeId);
    /** 开始一次网址可达性检测，结果通过 urlChecked 返回。 */
    Q_INVOKABLE QVariantMap checkUrl(const QString& address);
    /** 请求右侧浏览区加载指定地址。 */
    Q_INVOKABLE QVariantMap openUrl(const QString& address);

signals:
    /** 一次网址检测结束，携带统一结果结构。 */
    void urlChecked(const QVariantMap& result);
    /** 面板请求在右侧浏览区加载地址。 */
    void openRequested(const QString& url);
    /** 书签名录已持久化，方便与外部状态联动。 */
    void bookmarksChanged();

private:
    BookmarkService* m_service; ///< 由外部持有生命周期的书签服务。
};
