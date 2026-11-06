#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class FileSearchWorker;
class QThread;

// 文件检索服务：在工作线程维护全盘文件名索引并提供即时检索。
// NTFS 使用主文件表枚举和 USN 增量日志，普通用户通过独立服务读取。
// 不具备服务权限时提示启用；仅非 NTFS 卷使用目录扫描与变更监视。
// 环境变量 DESKTOPTOOL_FILESEARCH_ROOTS 指定分号分隔的目录列表时，
// 只索引这些目录（测试注入），且强制使用扫描模式。
class FileSearchService final : public QObject
{
    Q_OBJECT
public:
    // enabled 为 false 时不启动任何线程，仅返回未启用状态。
    explicit FileSearchService(const QString& dataRoot, bool enabled, QObject* parent = nullptr);
    ~FileSearchService() override;

    // 最近一次状态快照（阶段、模式、各卷状态与文件计数）。
    QVariantMap statusSnapshot() const { return m_status; }
    // 提交一次检索；结果（含 requestId）通过 searchFinished 信号异步返回。
    QVariantMap search(const QString& query, int offset, int limit, bool searchPath);
    // 打断当前构建并丢弃索引重建。
    QVariantMap rebuild();
    // 启用受保护的 NTFS 读取服务，仅此操作请求 Windows 管理员授权。
    QVariantMap enableJournalService();

signals:
    // 索引状态或构建进度已更新。
    void statusChanged();
    // 一次检索完成；result 含 requestId、rows、total 等。
    void searchFinished(const QVariantMap& result);

private:
    // 工作线程状态经队列信号送达，仅在本线程更新缓存。
    void setStatus(QVariantMap status);

    QString m_dataRoot;
    bool m_enabled = false;
    QThread* m_thread = nullptr;
    FileSearchWorker* m_worker = nullptr;
    QVariantMap m_status;
    int m_nextRequestId = 0;
    // 防止用户重复启动服务安装器。
    bool m_installing = false;
};
