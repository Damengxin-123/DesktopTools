#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include <QStringList>
#include <QVariantMap>
#include <QUrl>
#include <memory>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

/** 管理相互独立的 HTTP 下载任务、断点恢复及持久化历史。 */
class DownloadService final : public QObject
{
    Q_OBJECT
public:
    /** 加载指定数据目录中的任务历史；未完成任务恢复为暂停状态。 */
    explicit DownloadService(const QString& dataRoot, QObject* parent = nullptr);
    /** 停止网络请求，刷新部分文件并保存可继续的任务状态。 */
    ~DownloadService() override;
    /** 返回任务卡片数据和正在下载的任务数量。 */
    QVariantMap snapshot() const;
    /** 创建下载任务；无效指定目录按默认目录及系统目录顺序回退。 */
    QVariantMap createTask(const QString& url, const QString& directory, const QString& defaultDirectory);
    /** 暂停任务并保留已写入的部分内容。 */
    QVariantMap pauseTask(const QString& id);
    /** 继续暂停或失败的任务，并校验服务器断点响应。 */
    QVariantMap resumeTask(const QString& id);
    /** 取消任务，仅删除该任务拥有的部分文件。 */
    QVariantMap cancelTask(const QString& id);
    /** 移除终态任务记录，保留已经下载完成的文件。 */
    QVariantMap removeTask(const QString& id);
    /** 通过系统文件管理器打开任务所在目录，不执行下载文件。 */
    QVariantMap openDirectory(const QString& id);
    /** 是否存在尚在传输的任务，供关闭窗口时判断。 */
    bool hasActiveTasks() const;

signals:
    /** 状态变化立即通知，进度变化按固定频率合并通知。 */
    void changed();

private:
    /** 单个下载任务的持久化信息与当前传输资源。 */
    struct Task {
        QString id;                 ///< 随机生成且不含路径字符的任务标识。
        QUrl url;                   ///< 用户提交的原始 HTTP 或 HTTPS 地址。
        QString fileName;           ///< 已清理且不会覆盖现有文件的目标名称。
        QString directory;          ///< 已验证的绝对下载目录。
        QString status;             ///< 下载中、暂停、完成、取消或失败状态。
        qint64 bytesReceived = 0;    ///< 实际成功写入部分文件的字节数。
        qint64 totalBytes = -1;      ///< 服务器提供的总大小，未知时为负一。
        qint64 speed = 0;           ///< 近期平均写入速度，单位字节每秒。
        QString error;              ///< 最近一次失败的可读说明。
        QString warning;            ///< 目录回退或重新下载的可读说明。
        QString createdAt;          ///< 创建时间，使用 ISO 8601 格式。
        QByteArray etag;            ///< 可用于 If-Range 的强实体标识。
        QByteArray lastModified;    ///< 无强实体标识时使用的修改时间。
        QPointer<QNetworkAccessManager> network; ///< 当前传输独立的连接池，恢复时不会复用已失效连接。
        QPointer<QNetworkReply> reply; ///< 当前请求，过期回调不会影响新请求。
        QPointer<QTimer> retryTimer; ///< 暂态网络故障后的等待计时器，暂停和取消时清理。
        int retryAttempts = 0;      ///< 本轮人工启动以来的自动重试次数，收到数据也不重置。
        std::unique_ptr<QFile> file; ///< 仅属于本任务的部分文件句柄。
        QElapsedTimer elapsed;      ///< 当前速度采样区间的计时器。
        qint64 speedStartBytes = 0; ///< 速度采样开始时的已写入字节数。
        qint64 requestOffset = 0;   ///< 本次 Range 请求的起始偏移。
        qint64 responseBytes = 0;   ///< 当前响应实际写入的字节数。
        qint64 expectedBytes = -1;  ///< 当前响应声明的内容长度。
        bool headersAccepted = false; ///< 响应状态和续传范围是否已经校验。
        int redirects = 0;         ///< 当前请求链已经跟随的重定向次数。
    };
    /** 共享任务对象使异步回调期间记录生命周期稳定。 */
    using TaskPtr = std::shared_ptr<Task>;

    /** 检查地址仅使用明确的 HTTP 或 HTTPS 协议。 */
    static bool validUrl(const QUrl& url);
    /** 去除路径、控制字符和 Windows 特殊文件名。 */
    static QString safeFileName(const QString& name);
    /** 计算任务专属部分文件路径，始终由目录和标识生成。 */
    static QString partPath(const Task& task);
    /** 把任务转换成网页可序列化的卡片数据。 */
    static QVariantMap taskData(const Task& task);
    /** 选择尚未存在且未被其他任务预留的目标名称。 */
    QString availableName(const QString& directory, const QString& name, const QString& exceptId = {}) const;
    /** 验证并探测目录可写性，按明确的默认规则进行回退。 */
    QString chooseDirectory(const QString& requested, const QString& preferred, QString& warning) const;
    /** 校验索引目录不通过链接或联接点越过数据根目录。 */
    bool safeIndexPath() const;
    /** 读取并严格校验任务索引，不信任存储的完整文件路径。 */
    void load();
    /** 原子保存全部任务状态，错误时不覆盖原索引。 */
    bool persist(QString* error = nullptr);
    /** 保存状态并立即刷新界面，失败时保留真实任务状态并报告警告。 */
    void publishState();
    /** 合并高频进度通知和磁盘检查点。 */
    void publishProgress();
    /** 打开安全的部分文件并开始一次新的下载或恢复请求。 */
    bool start(const TaskPtr& task, QString& error);
    /** 发起已验证地址的请求并连接当前请求专属回调。 */
    void request(const TaskPtr& task, const QUrl& url);
    /** 验证 HTTP 状态、内容编码、长度及 Content-Range。 */
    bool acceptHeaders(const TaskPtr& task);
    /** 以有限缓冲区将网络数据写入部分文件。 */
    void consume(const TaskPtr& task);
    /** 请求完成时处理重定向、失败或原子发布最终文件。 */
    void finish(const TaskPtr& task);
    /** 为暂态网络故障安排有限次数的续传，等待期间仍允许暂停和取消。 */
    bool scheduleRetry(const TaskPtr& task);
    /** 停止当前请求、关闭文件，不删除部分内容。 */
    void stop(const TaskPtr& task);
    /** 标记失败并保留可以人工重试的部分文件。 */
    void fail(const TaskPtr& task, const QString& error);
    /** 只删除由本任务 ID 生成的普通部分文件，拒绝链接。 */
    bool removePartial(const TaskPtr& task, QString& error);

    QString m_dataRoot;            ///< 存放任务索引的受控数据目录。
    QString m_indexPath;           ///< 任务历史的版本化 JSON 路径。
    QString m_loadError;           ///< 损坏或不安全索引的保护性错误。
    QHash<QString, TaskPtr> m_tasks; ///< 按标识检索的任务集合。
    QStringList m_order;           ///< 任务卡片的创建顺序。
    QTimer* m_notifyTimer;         ///< 高频进度通知的合并计时器。
    QTimer* m_checkpointTimer;     ///< 下载中索引检查点的合并计时器。
};
