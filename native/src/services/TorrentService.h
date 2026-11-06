#pragma once

#include <QObject>
#include <QVariantMap>
#include <memory>

/** 管理先选择文件再下载的原生磁力任务，网络会话只在用户启动任务时创建。 */
class TorrentService final : public QObject
{
    Q_OBJECT
public:
    /** 读取独立磁力索引，恢复历史时不建立网络连接。 */
    explicit TorrentService(const QString& dataRoot, QObject* parent = nullptr);
    /** 停止网络传输并保留可校验恢复的已下载内容。 */
    ~TorrentService() override;
    /** 返回统一的任务列表与正在解析或下载的任务数量。 */
    QVariantMap snapshot() const;
    /** 在有效目录下创建专属任务目录，先获取元数据而不下载文件内容。 */
    QVariantMap createTask(const QString& magnet, const QString& directory, const QString& warning = {});
    /** 返回经过安全校验的文件列表及当前选择状态。 */
    QVariantMap files(const QString& id) const;
    /** 确认非空的合法文件索引列表后，开始下载选择的文件。 */
    QVariantMap confirmFiles(const QString& id, const QVariantList& indices);
    /** 暂停元数据解析或文件下载，保留已获得的内容。 */
    QVariantMap pauseTask(const QString& id);
    /** 继续任务；尚未确认文件的任务绝不会自动选择或下载文件。 */
    QVariantMap resumeTask(const QString& id);
    /** 取消任务，只清理该任务拥有的未完成内容。 */
    QVariantMap cancelTask(const QString& id);
    /** 移除终态记录，完成文件保留在任务目录内。 */
    QVariantMap removeTask(const QString& id);
    /** 打开专属下载目录，不执行其中的文件。 */
    QVariantMap openDirectory(const QString& id);
    /** 是否仍有元数据解析或文件传输任务。 */
    bool hasActiveTasks() const;

signals:
    /** 文件列表、任务状态或合并后的下载进度已改变。 */
    void changed();

private:
    /** 隐藏 libtorrent 类型、缓存的元数据和独立持久化状态。 */
    struct State;
    std::unique_ptr<State> m_state; ///< 服务拥有的实现对象，网络会话延迟初始化。
};
