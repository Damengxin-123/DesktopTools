#include "TorrentService.h"
#include "ServiceResult.h"

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/bdecode.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/write_resume_data.hpp>

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>

namespace lt = libtorrent;

namespace {
/** 索引及单个元数据文件的大小上限。 */
constexpr qint64 MaximumBytes = 16 * 1024 * 1024;
/** 文件选择界面允许处理的最大文件数量。 */
constexpr int MaximumFiles = 10000;
/** 独立目录内用于确认任务所有权的标记文件。 */
const QString OwnerFile = QStringLiteral(".desktoptool-magnet-owner");
/** 使用引擎文档列出的多个入口，单个域名解析异常时仍能进入 DHT 网络。 */
constexpr const char* BootstrapNodes = "dht.libtorrent.org:25401,dht.transmissionbt.com:6881,router.bittorrent.com:6881,router.bt.ouinet.work:6881";

/** 只有显式回环节点且没有 tracker 的任务使用封闭的本机发现流程。 */
bool localDiscovery(const lt::add_torrent_params& params)
{
    return params.trackers.empty() && !params.peers.empty()
        && std::all_of(params.peers.begin(), params.peers.end(), [](const auto& peer) { return peer.address().is_loopback(); });
}

/** 将 libtorrent 使用的 UTF-8 路径转换为 Qt 字符串。 */
QString fromUtf8(const std::string& text) { return QString::fromUtf8(text.data(), qsizetype(text.size())); }
/** 转换本地路径；Windows 下 libtorrent 要求使用本机分隔符。 */
std::string nativePath(const QString& path) { return QDir::toNativeSeparators(path).toUtf8().toStdString(); }
/** 检查已有路径不为符号链接或目录联接。 */
bool plainPath(const QString& path)
{
    const QFileInfo info(path);
    return !info.isSymbolicLink() && !info.isJunction();
}
/** 原子写入普通文件，写入失败不会覆盖原文件。 */
bool atomicWrite(const QString& path, const QByteArray& bytes, QString& error)
{
    if (!plainPath(path) || bytes.size() > MaximumBytes) {
        error = QStringLiteral("磁力数据路径不安全或文件过大，已禁止覆盖。");
        return false;
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error = QStringLiteral("无法保存磁力任务：") + file.errorString();
        return false;
    }
    return true;
}
/** 拒绝 Windows 保留名、目录穿越和不稳定的路径分量。 */
bool validComponent(const QString& value)
{
    static const QRegularExpression unsafe(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f\\x7f]"));
    static const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)"), QRegularExpression::CaseInsensitiveOption);
    return !value.isEmpty() && value != QStringLiteral(".") && value != QStringLiteral("..")
        && !value.endsWith(QLatin1Char('.')) && !value.endsWith(QLatin1Char(' '))
        && !unsafe.match(value).hasMatch() && !reserved.match(value).hasMatch()
        && !value.startsWith(QStringLiteral(".desktoptool-"), Qt::CaseInsensitive);
}
/** 校验原始 bencode 路径分量，避免接受库内部已替换的恶意路径。 */
bool validRawName(lt::string_view text)
{
    const QByteArray bytes(text.data(), qsizetype(text.size()));
    const QString value = QString::fromUtf8(bytes);
    return value.toUtf8() == bytes && validComponent(value);
}
/** 递归检查 v2 文件树及其符号链接属性。 */
bool validFileTree(const lt::bdecode_node& node, int depth = 0)
{
    if (depth > 64 || node.type() != lt::bdecode_node::dict_t)
        return false;
    for (int index = 0; index < node.dict_size(); ++index) {
        const auto item = node.dict_at(index);
        if (item.first.empty()) {
            if (item.second.type() != lt::bdecode_node::dict_t
                || item.second.dict_find_string_value("attr").find('l') != lt::string_view::npos
                || item.second.dict_find("symlink path"))
                return false;
        } else if (!validRawName(item.first) || !validFileTree(item.second, depth + 1)) {
            return false;
        }
    }
    return true;
}
/** 校验元数据原始名称、v1 路径及 v2 文件树。 */
bool validRawMetadata(const lt::torrent_info& info)
{
    lt::error_code error;
    const auto raw = lt::bdecode(info.info_section(), error);
    if (error || raw.type() != lt::bdecode_node::dict_t || !validRawName(raw.dict_find_string_value("name")))
        return false;
    const auto utf8Name = raw.dict_find_string("name.utf-8");
    if (utf8Name && !validRawName(utf8Name.string_value()))
        return false;
    const auto files = raw.dict_find_list("files");
    for (int index = 0; files && index < files.list_size(); ++index) {
        const auto file = files.list_at(index);
        if (file.type() != lt::bdecode_node::dict_t || file.dict_find_string_value("attr").find('l') != lt::string_view::npos
            || file.dict_find("symlink path"))
            return false;
        for (const char* key : {"path", "path.utf-8"}) {
            const auto path = file.dict_find_list(key);
            if (!path) {
                if (QByteArray(key) == "path")
                    return false;
                continue;
            }
            if (path.list_size() == 0)
                return false;
            for (int component = 0; component < path.list_size(); ++component) {
                if (path.list_at(component).type() != lt::bdecode_node::string_t || !validRawName(path.list_string_value_at(component)))
                    return false;
            }
        }
    }
    const auto tree = raw.dict_find("file tree");
    return !tree || validFileTree(tree);
}
/** 解析磁力链接并将内容哈希规范化，同时保留用户指定的 tracker 和直接节点。 */
bool parseMagnet(const QString& text, lt::add_torrent_params& params, QString& normalized, QString& error)
{
    const QUrl url(text.trimmed(), QUrl::StrictMode);
    if (text.size() > 16384 || !url.isValid() || url.scheme().compare(QStringLiteral("magnet"), Qt::CaseInsensitive) != 0
        || text.contains(QChar::Null)) {
        error = QStringLiteral("请输入有效的 magnet:?xt= 磁力链接。");
        return false;
    }
    lt::error_code ec;
    params = lt::parse_magnet_uri(url.toString(QUrl::FullyEncoded).toUtf8().toStdString(), ec);
    if (ec || (!params.info_hashes.has_v1() && !params.info_hashes.has_v2())) {
        error = QStringLiteral("磁力链接缺少有效的 BT 内容哈希：") + fromUtf8(ec.message());
        return false;
    }
    QUrl result(url);
    result.setScheme(QStringLiteral("magnet"));
    result.setFragment({});
    QUrlQuery query(result);
    query.removeAllQueryItems(QStringLiteral("xt"));
    if (params.info_hashes.has_v1())
        query.addQueryItem(QStringLiteral("xt"), QStringLiteral("urn:btih:") + QString::fromLatin1(QByteArray(params.info_hashes.v1.data(), 20).toHex()));
    if (params.info_hashes.has_v2())
        query.addQueryItem(QStringLiteral("xt"), QStringLiteral("urn:btmh:1220") + QString::fromLatin1(QByteArray(params.info_hashes.v2.data(), 32).toHex()));
    result.setQuery(query);
    normalized = result.toString(QUrl::FullyEncoded);
    return true;
}
/** 判断两个磁力或元数据是否共享同一个受支持的内容哈希。 */
bool sameHash(const lt::info_hash_t& first, const lt::info_hash_t& second)
{
    return (first.has_v1() && second.has_v1() && first.v1 == second.v1)
        || (first.has_v2() && second.has_v2() && first.v2 == second.v2);
}
}

/** 持有元数据缓存、任务句柄和延迟创建的 BitTorrent 网络会话。 */
struct TorrentService::State {
    /** 单项任务；元数据缓存和句柄不直接序列化到 JSON。 */
    struct Task {
        QString id;                   ///< 稳定的磁力任务标识。
        QString magnet;               ///< 保留发现节点的规范化磁力链接。
        QString directory;            ///< 只属于本任务的保存目录。
        QString name;                 ///< 元数据提供的显示名称。
        QString status;               ///< 当前解析、选择、下载或终态状态。
        QString error;                ///< 最近一次失败说明。
        QString warning;              ///< 目录回退或存盘失败提示。
        QString createdAt;            ///< UTC 创建时间。
        qint64 received = 0;           ///< 已验证的已选文件字节数。
        qint64 total = -1;             ///< 当前文件选择对应的总字节数。
        qint64 speed = 0;              ///< 当前文件内容下载速度。
        bool confirmed = false;       ///< 用户是否明确确认过文件选择。
        bool cleanupPending = false;  ///< 已取消任务是否仍在等待安全清理。
        bool finishing = false;       ///< 完成下载后是否正在关闭文件句柄。
        bool publicTask = false;      ///< 本次启动是否需要公网节点发现。
        int knownPeers = 0;           ///< 引擎已发现的文件来源数量，不代表来源仍在线。
        int connectedPeers = 0;       ///< 当前完成连接的文件来源数量。
        qint64 metadataElapsedSeconds = 0; ///< 本次解析持续的秒数，暂停后停止更新。
        qint64 lastDhtRequest = -30000; ///< 最近一次主动查询的单调时钟毫秒值。
        bool pendingDhtQuery = false; ///< 网络就绪事件要求补查，限频期间保留到下次轮询再发送。
        QElapsedTimer metadataClock;  ///< 仅统计当前解析尝试，重新开始时清零。
        QSet<int> selected;            ///< 用户明确选择的文件索引。
        QVariantList fileList;        ///< 已安全校验且不包含填充文件的缓存列表。
        lt::info_hash_t hashes;        ///< 从磁力链接解析的原始内容哈希。
        lt::add_torrent_params metadata; ///< 不携带可信文件路径的元数据缓存。
        lt::torrent_handle handle;    ///< 当前传输句柄，替换后旧事件无效。
    };
    /** 任务共享指针使异步事件处理期间对象保持有效。 */
    using TaskPtr = std::shared_ptr<Task>;

    TorrentService* owner;             ///< 对外发送状态信号的服务对象。
    QString root;                     ///< 磁力索引所在数据根目录。
    QString indexPath;                ///< 独立版本化磁力索引。
    QString metadataRoot;             ///< 经过校验的 .torrent 缓存目录。
    QString loadError;                ///< 损坏索引的写保护错误。
    QHash<QString, TaskPtr> tasks;     ///< 按标识定位的任务集合。
    QStringList order;                ///< 界面显示顺序。
    std::unique_ptr<lt::session> session; ///< 用户开始磁力任务后才创建的网络会话。
    QTimer* timer;                    ///< 合并状态与进度更新的轮询计时器。
    QElapsedTimer checkpoint;         ///< 限制下载进度写盘频率。
    bool publicDiscovery = false;     ///< 是否已经显式启用公网 DHT 发现。
    QHash<QString, int> dhtNodes;      ///< 分监听端点统计路由节点，避免 IPv6 空表覆盖 IPv4 结果。
    QElapsedTimer discoveryClock;     ///< 限制会话统计与主动查询的频率。
    qint64 lastDhtStats = -1000;       ///< 上次请求路由统计的毫秒值。
    QString networkIssue;             ///< 最近一次 DHT 或监听错误，供解析阶段说明原因。

    /** 建立纯磁盘状态；构造和历史读取均不创建网络会话。 */
    State(TorrentService* service, const QString& dataRoot)
        : owner(service), root(QDir::cleanPath(QFileInfo(dataRoot).absoluteFilePath()))
        , indexPath(QDir(root).filePath(QStringLiteral("magnet-tasks.v1.json")))
        , metadataRoot(QDir(root).filePath(QStringLiteral("magnet-metadata")))
        , timer(new QTimer(service))
    {
        timer->setInterval(250);
        QObject::connect(timer, &QTimer::timeout, owner, [this] { poll(); });
        checkpoint.start();
        discoveryClock.start();
        load();
    }
    /** 停止事件源并等待网络库刷新其拥有的文件。 */
    ~State()
    {
        timer->stop();
        for (const auto& task : tasks) {
            if (task->handle.is_valid())
                task->handle.pause();
            if (active(*task) || (task->confirmed && task->status == QStringLiteral("awaiting_selection")))
                task->status = QStringLiteral("paused");
        }
        session.reset();
        QString error;
        if (loadError.isEmpty() && (!tasks.isEmpty() || QFileInfo::exists(indexPath)))
            persist(error);
    }
    /** 统一判断任务是否正在使用网络获取元数据或文件内容。 */
    static bool active(const Task& task)
    {
        return task.status == QStringLiteral("resolving") || task.status == QStringLiteral("downloading");
    }
    /** 元数据缓存路径始终从已验证的任务标识推导。 */
    QString metadataPath(const Task& task) const { return QDir(metadataRoot).filePath(task.id + QStringLiteral(".torrent")); }
    /** 汇总不同监听端点的有效路由节点。 */
    int dhtNodeCount() const
    {
        int count = 0;
        for (int nodes : dhtNodes)
            count += nodes;
        return count;
    }
    /** 解释当前解析停留的阶段，不把缺少来源误报成文件下载超时。 */
    QString discoveryMessage(const Task& task) const
    {
        if (task.status != QStringLiteral("resolving"))
            return {};
        if (task.connectedPeers > 0)
            return QStringLiteral("已连接 %1 个文件来源，正在获取文件信息…").arg(task.connectedPeers);
        if (task.knownPeers > 0)
            return QStringLiteral("已发现 %1 个文件来源，正在尝试连接…").arg(task.knownPeers);
        if (task.publicTask && dhtNodeCount() > 0)
            return task.metadataElapsedSeconds < 60
                ? QStringLiteral("已接入资源网络，正在寻找在线文件来源…")
                : QStringLiteral("已接入资源网络，暂未找到在线文件来源，可继续等待或暂停后重试。");
        if (!task.publicTask)
            return QStringLiteral("正在连接指定文件来源并获取文件信息…");
        if (task.metadataElapsedSeconds >= 15 && !networkIssue.isEmpty())
            return QStringLiteral("资源网络连接遇到问题：") + networkIssue;
        return task.metadataElapsedSeconds < 30
            ? QStringLiteral("正在通过多个入口连接资源网络…")
            : QStringLiteral("尚未连接到资源网络，正在等待备用入口回应；请检查网络是否允许 UDP 通信。");
    }
    /** 生成网页和持久化共享的基础字段。 */
    QVariantMap taskData(const Task& task) const
    {
        return {{QStringLiteral("id"), task.id}, {QStringLiteral("url"), task.magnet}, {QStringLiteral("kind"), QStringLiteral("magnet")},
            {QStringLiteral("fileName"), task.name}, {QStringLiteral("directory"), task.directory}, {QStringLiteral("filePath"), task.directory},
            {QStringLiteral("status"), task.status}, {QStringLiteral("bytesReceived"), task.received}, {QStringLiteral("totalBytes"), task.total},
            {QStringLiteral("speed"), task.speed}, {QStringLiteral("error"), task.error}, {QStringLiteral("warning"), task.warning},
            {QStringLiteral("createdAt"), task.createdAt}, {QStringLiteral("fileCount"), task.fileList.size()},
            {QStringLiteral("selectedCount"), task.selected.size()}, {QStringLiteral("selectionConfirmed"), task.confirmed},
            {QStringLiteral("discoveryMessage"), discoveryMessage(task)}, {QStringLiteral("metadataElapsedSeconds"), task.metadataElapsedSeconds},
            {QStringLiteral("dhtNodes"), task.publicTask ? dhtNodeCount() : 0},
            {QStringLiteral("knownPeers"), task.knownPeers}, {QStringLiteral("connectedPeers"), task.connectedPeers}};
    }
    /** 检查根目录、缓存目录和索引本身的链接边界。 */
    bool safeStore() const
    {
        return plainPath(root) && plainPath(metadataRoot) && plainPath(indexPath)
            && (!QFileInfo::exists(root) || QFileInfo(root).isDir())
            && (!QFileInfo::exists(metadataRoot) || QFileInfo(metadataRoot).isDir())
            && (!QFileInfo::exists(indexPath) || QFileInfo(indexPath).isFile());
    }
    /** 原子保存记录；元数据单独存储，因此轮询不会反复读取 .torrent。 */
    bool persist(QString& error)
    {
        if (!loadError.isEmpty() || !safeStore() || !QDir().mkpath(root)) {
            error = loadError.isEmpty() ? QStringLiteral("磁力索引目录不可写或不安全。") : loadError;
            return false;
        }
        QJsonArray items;
        for (const auto& id : order) {
            const auto& task = *tasks.value(id);
            auto object = QJsonObject::fromVariantMap(taskData(task));
            QJsonArray selected;
            auto indices = task.selected.values();
            std::sort(indices.begin(), indices.end());
            for (int index : indices)
                selected.append(index);
            object.insert(QStringLiteral("selected"), selected);
            object.insert(QStringLiteral("hasMetadata"), bool(task.metadata.ti));
            object.insert(QStringLiteral("cleanupPending"), task.cleanupPending);
            items.append(object);
        }
        return atomicWrite(indexPath, QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("items"), items}}).toJson(), error);
    }
    /** 落盘并通知界面，失败时保留真实状态和可见警告。 */
    void publish()
    {
        QString error;
        if (!persist(error)) {
            for (const auto& task : tasks)
                task->warning = error;
        }
        checkpoint.restart();
        emit owner->changed();
    }
    /** 验证目录所有权及每层父目录都未被替换成链接。 */
    bool ownsDirectory(const Task& task, QString& error) const
    {
        QFileInfo info(task.directory);
        if (!info.isDir() || info.fileName() != task.id || !plainPath(task.directory)) {
            error = QStringLiteral("磁力任务目录不存在或已被替换，已停止文件操作。");
            return false;
        }
        QString parent = task.directory;
        while (!parent.isEmpty()) {
            if (!plainPath(parent)) {
                error = QStringLiteral("磁力任务目录包含链接，已停止文件操作。");
                return false;
            }
            const QString next = QFileInfo(parent).dir().absolutePath();
            if (next == parent)
                break;
            parent = next;
        }
        const QString path = QDir(task.directory).filePath(OwnerFile);
        QFile marker(path);
        if (!plainPath(path) || !marker.open(QIODevice::ReadOnly) || marker.read(256) != task.id.toUtf8()) {
            error = QStringLiteral("磁力任务目录缺少有效的所有权标记，已保留目录内容。");
            return false;
        }
        return true;
    }
    /** 检查元数据映射路径，拒绝链接、越界和首次下载时已有的同名文件。 */
    bool safeFiles(const Task& task, bool requireAbsent, QString& error) const
    {
        if (!ownsDirectory(task, error))
            return false;
        for (const auto& entry : task.fileList) {
            const auto item = entry.toMap();
            const auto components = item.value(QStringLiteral("path")).toString().split(QLatin1Char('/'));
            QString path = task.directory;
            for (qsizetype index = 0; index < components.size(); ++index) {
                path = QDir(path).filePath(components.at(index));
                const QFileInfo info(path);
                if (!plainPath(path) || (info.exists() && index + 1 < components.size() && !info.isDir())
                    || (info.exists() && index + 1 == components.size() && (!info.isFile() || requireAbsent))) {
                    error = QStringLiteral("磁力目标路径含链接、类型冲突或已有同名文件，已拒绝覆盖。");
                    return false;
                }
            }
        }
        return true;
    }
    /** 校验原始元数据及所有文件路径，缓存供界面使用的不可执行清单。 */
    bool inspectMetadata(Task& task, QString& error)
    {
        if (!task.metadata.ti || !sameHash(task.hashes, task.metadata.ti->info_hashes()) || !validRawMetadata(*task.metadata.ti)) {
            error = QStringLiteral("磁力元数据哈希不匹配，或包含不安全的路径与符号链接。");
            return false;
        }
        const auto& layout = task.metadata.ti->layout();
        if (layout.num_files() <= 0 || layout.num_files() > MaximumFiles) {
            error = QStringLiteral("磁力文件数量无效或超过可处理上限。");
            return false;
        }
        QSet<QString> paths;
        QVariantList list;
        qint64 total = 0;
        for (int index = 0; index < layout.num_files(); ++index) {
            const lt::file_index_t fileIndex(index);
            if (bool(layout.file_flags(fileIndex) & lt::file_storage::flag_symlink)) {
                error = QStringLiteral("磁力元数据包含符号链接，已拒绝下载。");
                return false;
            }
            const QString path = QDir::fromNativeSeparators(fromUtf8(layout.file_path(fileIndex)));
            const auto parts = path.split(QLatin1Char('/'));
            if (QDir::isAbsolutePath(path) || QDir::cleanPath(path) != path
                || std::any_of(parts.begin(), parts.end(), [](const QString& part) { return !validComponent(part); })
                || paths.contains(path.toCaseFolded())) {
                error = QStringLiteral("磁力文件路径越界、重复或包含非法文件名。");
                return false;
            }
            paths.insert(path.toCaseFolded());
            if (bool(layout.file_flags(fileIndex) & lt::file_storage::flag_pad_file))
                continue;
            const qint64 size = layout.file_size(fileIndex);
            if (size < 0 || size > std::numeric_limits<qint64>::max() - total) {
                error = QStringLiteral("磁力文件大小无效。");
                return false;
            }
            total += size;
            list.append(QVariantMap{{QStringLiteral("index"), index}, {QStringLiteral("path"), path},
                {QStringLiteral("size"), size}, {QStringLiteral("selected"), task.selected.contains(index)}});
        }
        if (list.isEmpty()) {
            error = QStringLiteral("磁力任务没有可下载的普通文件。");
            return false;
        }
        task.name = fromUtf8(task.metadata.ti->name());
        task.fileList = list;
        task.total = total;
        if (task.confirmed) {
            task.total = 0;
            QSet<int> valid;
            for (const auto& item : list) {
                const auto file = item.toMap();
                const int index = file.value(QStringLiteral("index")).toInt();
                if (task.selected.contains(index)) {
                    valid.insert(index);
                    task.total += file.value(QStringLiteral("size")).toLongLong();
                }
            }
            if (valid != task.selected || valid.isEmpty()) {
                error = QStringLiteral("保存的磁力文件选择无效，已禁止自动下载。");
                return false;
            }
        } else if (!task.selected.isEmpty()) {
            error = QStringLiteral("未确认的磁力任务不能包含自动选择的文件。");
            return false;
        }
        return true;
    }
    /** 加载历史并严格保护损坏索引；任何任务都不会在读取时联网。 */
    void load();
    /** 创建需要的网络会话，本地测试节点不会启用公网发现。 */
    void ensureSession(const lt::add_torrent_params& params);
    /** 记录网络就绪事件并发送待处理查询，限频不能丢弃尚未执行的补查。 */
    void requestMetadataPeers(bool networkEvent = false);
    /** 根据明确确认过的文件选择启动任务。 */
    bool start(const TaskPtr& task, QString& error);
    /** 标记失败并停用当前句柄，保留可恢复的文件。 */
    void fail(const TaskPtr& task, const QString& error);
    /** 保存安全元数据并转入等待选择，迟到事件不能复活暂停或取消任务。 */
    void receiveMetadata(const TaskPtr& task);
    /** 只清理用户已取消任务的已选文件与确定归属的分片缓存。 */
    bool cleanup(const TaskPtr& task, QString& error);
    /** 处理句柄身份匹配的网络事件及合并进度更新。 */
    void poll();
};

void TorrentService::State::load()
{
    if (!safeStore()) {
        loadError = QStringLiteral("磁力索引或缓存路径不安全，已禁止覆盖。");
        return;
    }
    if (!QFileInfo::exists(indexPath))
        return;
    QFile file(indexPath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaximumBytes) {
        loadError = QStringLiteral("磁力索引无法读取或过大，已保护原文件。");
        return;
    }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    const auto object = document.object();
    if (parse.error != QJsonParseError::NoError || !document.isObject()
        || object.value(QStringLiteral("version")).toInt() != 1 || !object.value(QStringLiteral("items")).isArray()) {
        loadError = QStringLiteral("磁力任务索引损坏或版本不支持，已禁止覆盖。");
        return;
    }
    static const QRegularExpression identifier(QStringLiteral("^magnet-[0-9a-f]{32}$"));
    const QSet<QString> statuses{QStringLiteral("resolving"), QStringLiteral("awaiting_selection"), QStringLiteral("downloading"),
        QStringLiteral("paused"), QStringLiteral("completed"), QStringLiteral("cancelled"), QStringLiteral("failed")};
    try {
        for (const auto& value : object.value(QStringLiteral("items")).toArray()) {
            const auto item = value.toObject();
            auto task = std::make_shared<Task>();
            task->id = item.value(QStringLiteral("id")).toString();
            task->directory = item.value(QStringLiteral("directory")).toString();
            task->name = item.value(QStringLiteral("fileName")).toString();
            task->status = item.value(QStringLiteral("status")).toString();
            task->error = item.value(QStringLiteral("error")).toString();
            task->warning = item.value(QStringLiteral("warning")).toString();
            task->createdAt = item.value(QStringLiteral("createdAt")).toString();
            task->received = item.value(QStringLiteral("bytesReceived")).toInteger(-1);
            task->total = item.value(QStringLiteral("totalBytes")).toInteger(-2);
            task->confirmed = item.value(QStringLiteral("selectionConfirmed")).toBool();
            task->cleanupPending = item.value(QStringLiteral("cleanupPending")).toBool();
            lt::add_torrent_params parsed;
            QString error;
            if (!value.isObject() || !identifier.match(task->id).hasMatch() || tasks.contains(task->id)
                || !QDir::isAbsolutePath(task->directory) || task->directory.contains(QChar::Null)
                || QDir::cleanPath(task->directory) != task->directory || QFileInfo(task->directory).fileName() != task->id
                || !statuses.contains(task->status) || task->received < 0 || task->total < -1
                || !item.value(QStringLiteral("selected")).isArray() || !item.value(QStringLiteral("selectionConfirmed")).isBool()
                || !item.value(QStringLiteral("hasMetadata")).isBool()
                || !parseMagnet(item.value(QStringLiteral("url")).toString(), parsed, task->magnet, error)) {
                loadError = QStringLiteral("磁力索引包含无效记录或不安全路径，已禁止文件操作。");
                break;
            }
            task->hashes = parsed.info_hashes;
            for (const auto& index : item.value(QStringLiteral("selected")).toArray()) {
                const int number = index.toInt(-1);
                if (!index.isDouble() || index.toDouble() != number || number < 0 || task->selected.contains(number)) {
                    loadError = QStringLiteral("磁力索引的文件选择无效，已禁止覆盖。");
                    break;
                }
                task->selected.insert(number);
            }
            if (!loadError.isEmpty())
                break;
            if (item.value(QStringLiteral("hasMetadata")).toBool()) {
                const QString path = metadataPath(*task);
                QFile metadataFile(path);
                if (!plainPath(path) || !metadataFile.open(QIODevice::ReadOnly) || metadataFile.size() > MaximumBytes) {
                    loadError = QStringLiteral("磁力元数据缓存缺失或不可读取，已保护任务索引。");
                    break;
                }
                const QByteArray bytes = metadataFile.readAll();
                lt::error_code ec;
                task->metadata = lt::load_torrent_buffer(lt::span<char const>(bytes.constData(), bytes.size()), ec, {});
                if (ec || !inspectMetadata(*task, error)) {
                    loadError = QStringLiteral("磁力元数据缓存无效：") + (ec ? fromUtf8(ec.message()) : error);
                    break;
                }
            } else if (task->confirmed || !task->selected.isEmpty() || task->status == QStringLiteral("awaiting_selection")
                || task->status == QStringLiteral("completed")) {
                loadError = QStringLiteral("磁力索引缺少必要的文件元数据，已禁止覆盖。");
                break;
            }
            if (active(*task) || (task->confirmed && task->status == QStringLiteral("awaiting_selection")))
                task->status = QStringLiteral("paused");
            if (task->cleanupPending)
                task->warning = QStringLiteral("上次取消的清理尚未完成，可再次取消以清理专属文件。");
            order.append(task->id);
            tasks.insert(task->id, task);
        }
    } catch (const std::exception& error) {
        loadError = QStringLiteral("读取磁力元数据失败：") + QString::fromUtf8(error.what());
    }
    if (!loadError.isEmpty()) {
        tasks.clear();
        order.clear();
    }
}

void TorrentService::State::ensureSession(const lt::add_torrent_params& params)
{
    const bool localOnly = localDiscovery(params);
    if (!session) {
        lt::settings_pack settings;
        settings.set_str(lt::settings_pack::listen_interfaces, "0.0.0.0:0,[::]:0");
        settings.set_bool(lt::settings_pack::enable_upnp, false);
        settings.set_bool(lt::settings_pack::enable_natpmp, false);
        settings.set_bool(lt::settings_pack::enable_lsd, false);
        settings.set_bool(lt::settings_pack::enable_dht, !localOnly);
        settings.set_str(lt::settings_pack::dht_bootstrap_nodes, localOnly ? "" : BootstrapNodes);
        settings.set_int(lt::settings_pack::alert_mask, lt::alert_category::error | lt::alert_category::status | lt::alert_category::storage | lt::alert_category::dht);
        session = std::make_unique<lt::session>(settings);
        publicDiscovery = !localOnly;
        timer->start();
    } else if (!localOnly && !publicDiscovery) {
        lt::settings_pack settings;
        settings.set_str(lt::settings_pack::dht_bootstrap_nodes, BootstrapNodes);
        settings.set_bool(lt::settings_pack::enable_dht, true);
        session->apply_settings(settings);
        publicDiscovery = true;
        dhtNodes.clear();
        networkIssue.clear();
        lastDhtStats = -1000;
    }
}

void TorrentService::State::requestMetadataPeers(bool networkEvent)
{
    const qint64 now = discoveryClock.elapsed();
    for (const auto& task : tasks) {
        if (!task->publicTask || task->status != QStringLiteral("resolving") || !task->handle.is_valid())
            continue;
        task->pendingDhtQuery = task->pendingDhtQuery || networkEvent;
        if (task->pendingDhtQuery && dhtNodeCount() > 0 && now - task->lastDhtRequest >= 30000) {
            task->handle.force_dht_announce();
            task->lastDhtRequest = now;
            task->pendingDhtQuery = false;
        }
    }
}

bool TorrentService::State::start(const TaskPtr& task, QString& error)
{
    if (!safeFiles(*task, false, error))
        return false;
    if (task->metadata.ti && !task->confirmed) {
        task->status = QStringLiteral("awaiting_selection");
        task->error.clear();
        return persist(error);
    }
    try {
        lt::add_torrent_params original;
        QString normalized;
        if (!parseMagnet(task->magnet, original, normalized, error))
            return false;
        task->publicTask = !localDiscovery(original);
        ensureSession(original);
        task->status = task->confirmed ? QStringLiteral("downloading") : QStringLiteral("resolving");
        task->error.clear();
        task->speed = 0;
        if (!task->confirmed) {
            task->metadataClock.restart();
            task->metadataElapsedSeconds = 0;
            task->lastDhtRequest = -30000;
            task->pendingDhtQuery = task->publicTask;
        }
        if (!persist(error)) {
            task->status = QStringLiteral("paused");
            return false;
        }
        if (task->handle.is_valid()) {
            task->handle.clear_error();
            task->handle.resume();
            return true;
        }
        auto params = task->metadata.ti ? task->metadata : original;
        params.trackers = original.trackers;
        params.tracker_tiers = original.tracker_tiers;
        params.peers = original.peers;
        params.url_seeds.clear();
        params.renamed_files.clear();
        params.save_path = nativePath(task->directory);
        params.flags = lt::torrent_flags::default_flags;
        params.flags &= ~(lt::torrent_flags::auto_managed | lt::torrent_flags::paused | lt::torrent_flags::seed_mode);
        params.flags |= lt::torrent_flags::duplicate_is_error | lt::torrent_flags::default_dont_download;
        params.file_priorities.clear();
        // 元数据阶段的全零分片优先级不能覆盖用户确认的文件优先级；续传由磁盘校验恢复。
        params.piece_priorities.clear();
        params.have_pieces.clear();
        params.verified_pieces.clear();
        params.unfinished_pieces.clear();
        if (!task->confirmed) {
            params.flags |= lt::torrent_flags::upload_mode;
        } else {
            params.file_priorities.assign(task->metadata.ti->layout().num_files(), lt::dont_download);
            for (int index : task->selected)
                params.file_priorities.at(std::size_t(index)) = lt::default_priority;
        }
        lt::error_code ec;
        task->handle = session->add_torrent(std::move(params), ec);
        if (ec) {
            error = QStringLiteral("无法启动磁力任务：") + fromUtf8(ec.message());
            task->handle = {};
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        error = QStringLiteral("启动磁力任务失败：") + QString::fromUtf8(exception.what());
        return false;
    }
}

void TorrentService::State::fail(const TaskPtr& task, const QString& error)
{
    if (task->handle.is_valid() && session) {
        task->handle.pause();
        session->remove_torrent(task->handle);
    }
    task->handle = {};
    task->finishing = false;
    task->status = QStringLiteral("failed");
    task->speed = 0;
    task->error = error;
    publish();
}

void TorrentService::State::receiveMetadata(const TaskPtr& task)
{
    // 暂停期间迟到的元数据可以缓存，但绝不改变暂停、取消或失败状态。
    if (!task->handle.is_valid() || task->confirmed || task->status == QStringLiteral("cancelled") || task->status == QStringLiteral("failed"))
        return;
    const bool wasResolving = task->status == QStringLiteral("resolving");
    task->handle.pause();
    try {
        task->metadata = task->handle.get_resume_data(lt::torrent_handle::save_info_dict);
        QString error;
        if (!inspectMetadata(*task, error) || !safeFiles(*task, true, error) || !safeStore() || !QDir().mkpath(metadataRoot)) {
            task->metadata = {};
            task->fileList.clear();
            task->total = -1;
            fail(task, error.isEmpty() ? QStringLiteral("无法创建安全的磁力元数据缓存目录。") : error);
            return;
        }
        const auto buffer = lt::write_torrent_file_buf(task->metadata, lt::write_flags::allow_missing_piece_layer | lt::write_flags::no_http_seeds);
        if (!atomicWrite(metadataPath(*task), QByteArray(buffer.data(), qsizetype(buffer.size())), error)) {
            task->metadata = {};
            task->fileList.clear();
            task->total = -1;
            fail(task, error);
            return;
        }
        session->remove_torrent(task->handle);
        task->handle = {};
        task->status = wasResolving ? QStringLiteral("awaiting_selection") : QStringLiteral("paused");
        task->received = 0;
        task->speed = 0;
        publish();
    } catch (const std::exception& exception) {
        task->metadata = {};
        task->fileList.clear();
        task->total = -1;
        fail(task, QStringLiteral("磁力元数据处理失败：") + QString::fromUtf8(exception.what()));
    }
}

bool TorrentService::State::cleanup(const TaskPtr& task, QString& error)
{
    if (!safeFiles(*task, false, error))
        return false;
    QStringList ownedPaths;
    for (const auto& value : task->fileList) {
        const auto file = value.toMap();
        if (task->selected.contains(file.value(QStringLiteral("index")).toInt()))
            ownedPaths.append(QDir(task->directory).filePath(file.value(QStringLiteral("path")).toString()));
    }
    // 引擎以完整元数据的优选哈希（v2 截断为 20 字节）命名分片缓存，混合种子也遵循此规则。
    const auto storageHash = (task->metadata.ti ? task->metadata.ti->info_hashes() : task->hashes).get_best();
    ownedPaths.append(QDir(task->directory).filePath(QLatin1Char('.') + QString::fromLatin1(QByteArray(storageHash.data(), 20).toHex()) + QStringLiteral(".parts")));
    // 先校验所有候选路径，再删除明确归属文件；任何未知文件和目录均予以保留。
    for (const auto& path : ownedPaths) {
        if (!plainPath(path) || (QFileInfo::exists(path) && !QFileInfo(path).isFile())) {
            error = QStringLiteral("任务文件已被替换为链接或目录，已拒绝清理。");
            return false;
        }
    }
    for (const auto& path : ownedPaths) {
        if (QFileInfo::exists(path) && !QFile::remove(path)) {
            error = QStringLiteral("无法清理磁力部分文件，请检查目录权限或文件占用。");
            return false;
        }
    }
    task->cleanupPending = false;
    task->received = 0;
    task->speed = 0;
    return true;
}

void TorrentService::State::poll()
{
    if (!session)
        return;
    std::vector<lt::alert*> alerts;
    session->pop_alerts(&alerts);
    for (const auto* alert : alerts) {
        // 引导和路由统计属于会话事件，必须在筛选单项任务事件之前处理。
        if (const auto* stats = lt::alert_cast<lt::dht_stats_alert>(alert)) {
            if (!publicDiscovery)
                continue;
            const int previous = dhtNodeCount();
            int nodes = 0;
            for (const auto& bucket : stats->routing_table)
                nodes += bucket.num_nodes;
            const QString endpoint = fromUtf8(stats->local_endpoint.address().to_string())
                + QLatin1Char(':') + QString::number(stats->local_endpoint.port());
            dhtNodes.insert(endpoint, nodes);
            if (previous == 0 && dhtNodeCount() > 0) {
                networkIssue.clear();
                requestMetadataPeers(true);
            }
            continue;
        }
        if (lt::alert_cast<lt::dht_bootstrap_alert>(alert)) {
            if (publicDiscovery)
                requestMetadataPeers(true);
            continue;
        }
        if (const auto* listenError = lt::alert_cast<lt::listen_failed_alert>(alert)) {
            networkIssue = QStringLiteral("无法建立监听：") + fromUtf8(listenError->error.message());
            continue;
        }
        if (const auto* dhtError = lt::alert_cast<lt::dht_error_alert>(alert)) {
            networkIssue = QStringLiteral("节点发现失败：") + fromUtf8(dhtError->error.message());
            continue;
        }
        const auto* torrentAlert = dynamic_cast<const lt::torrent_alert*>(alert);
        if (!torrentAlert)
            continue;
        TaskPtr task;
        for (const auto& candidate : tasks) {
            if (candidate->handle.is_valid() && candidate->handle == torrentAlert->handle) {
                task = candidate;
                break;
            }
        }
        if (!task)
            continue;
        try {
            if (lt::alert_cast<lt::cache_flushed_alert>(alert)) {
                if (task->cleanupPending || task->finishing) {
                    session->remove_torrent(task->handle);
                    task->handle = {};
                    QString error;
                    if (task->cleanupPending && !cleanup(task, error))
                        task->warning = error;
                    if (task->finishing) {
                        task->status = QStringLiteral("completed");
                        task->received = task->total;
                        task->finishing = false;
                    }
                    publish();
                }
            } else if (task->status != QStringLiteral("cancelled")) {
                if (lt::alert_cast<lt::metadata_received_alert>(alert))
                    receiveMetadata(task);
                else if (const auto* error = lt::alert_cast<lt::torrent_error_alert>(alert))
                    fail(task, QStringLiteral("磁力任务失败：") + fromUtf8(error->error.message()));
                else if (const auto* fileError = lt::alert_cast<lt::file_error_alert>(alert))
                    fail(task, QStringLiteral("磁力文件读写失败：") + fromUtf8(fileError->error.message()));
            }
        } catch (const std::exception& exception) {
            fail(task, QStringLiteral("磁力事件处理失败：") + QString::fromUtf8(exception.what()));
        }
    }
    bool changed = false;
    bool activeTask = false;
    for (const auto& task : tasks) {
        activeTask = activeTask || (active(*task) && task->publicTask);
        if (!task->handle.is_valid() || !active(*task) || task->finishing)
            continue;
        try {
            const auto status = task->handle.status();
            task->knownPeers = status.list_peers;
            task->connectedPeers = status.num_peers;
            if (task->status == QStringLiteral("resolving") && status.has_metadata) {
                receiveMetadata(task);
                continue;
            }
            if (task->status == QStringLiteral("resolving")) {
                task->metadataElapsedSeconds = task->metadataClock.isValid() ? task->metadataClock.elapsed() / 1000 : 0;
                changed = true;
            }
            if (!task->confirmed)
                continue;
            task->received = qBound<qint64>(0, status.total_wanted_done, task->total);
            task->speed = status.download_payload_rate;
            changed = true;
            if (status.is_finished && task->received == task->total) {
                task->handle.pause();
                task->finishing = true;
                task->speed = 0;
                task->handle.flush_cache();
            }
        } catch (const std::exception& exception) {
            fail(task, QStringLiteral("读取磁力下载状态失败：") + QString::fromUtf8(exception.what()));
        }
    }
    if (!activeTask && publicDiscovery) {
        lt::settings_pack settings;
        settings.set_bool(lt::settings_pack::enable_dht, false);
        session->apply_settings(settings);
        publicDiscovery = false;
        dhtNodes.clear();
    } else if (publicDiscovery && discoveryClock.elapsed() - lastDhtStats >= 1000) {
        session->post_dht_stats();
        lastDhtStats = discoveryClock.elapsed();
        requestMetadataPeers();
    }
    if (changed) {
        if (checkpoint.elapsed() >= 3000)
            publish();
        else
            emit owner->changed();
    }
}

TorrentService::TorrentService(const QString& dataRoot, QObject* parent)
    : QObject(parent), m_state(std::make_unique<State>(this, dataRoot))
{
}

TorrentService::~TorrentService() = default;

QVariantMap TorrentService::snapshot() const
{
    if (!m_state->loadError.isEmpty())
        return ServiceResult::failure(m_state->loadError);
    QVariantList items;
    int activeCount = 0;
    for (const auto& id : m_state->order) {
        const auto& task = *m_state->tasks.value(id);
        items.append(m_state->taskData(task));
        if (State::active(task))
            ++activeCount;
    }
    return ServiceResult::success(QVariantMap{{QStringLiteral("items"), items}, {QStringLiteral("activeCount"), activeCount}});
}

bool TorrentService::hasActiveTasks() const
{
    return std::any_of(m_state->tasks.begin(), m_state->tasks.end(), [](const auto& task) { return State::active(*task); });
}

QVariantMap TorrentService::createTask(const QString& magnet, const QString& directory, const QString& warning)
{
    if (!m_state->loadError.isEmpty())
        return ServiceResult::failure(m_state->loadError);
    QString normalized, error;
    lt::add_torrent_params params;
    if (!parseMagnet(magnet, params, normalized, error))
        return ServiceResult::failure(error);
    for (const auto& existing : m_state->tasks) {
        if (existing->status != QStringLiteral("cancelled") && sameHash(existing->hashes, params.info_hashes))
            return ServiceResult::failure(QStringLiteral("该磁力内容已有任务，请继续原任务或移除原记录后再创建。"));
    }
    const QFileInfo parent(directory);
    if (!QDir::isAbsolutePath(directory) || !parent.isDir() || parent.canonicalFilePath().isEmpty())
        return ServiceResult::failure(QStringLiteral("磁力下载目录不存在或不是有效的绝对目录。"));
    auto task = std::make_shared<State::Task>();
    task->id = QStringLiteral("magnet-") + QUuid::createUuid().toString(QUuid::Id128);
    task->directory = QDir(parent.canonicalFilePath()).filePath(task->id);
    task->magnet = normalized;
    task->hashes = params.info_hashes;
    task->name = params.name.empty() ? QStringLiteral("正在解析磁力链接") : fromUtf8(params.name);
    task->warning = warning;
    task->status = QStringLiteral("paused");
    task->createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    if (!QDir(parent.canonicalFilePath()).mkdir(task->id))
        return ServiceResult::failure(QStringLiteral("无法创建独立磁力任务目录，已保留已有文件。"));
    const QString marker = QDir(task->directory).filePath(OwnerFile);
    if (!atomicWrite(marker, task->id.toUtf8(), error))
        return ServiceResult::failure(error);
    m_state->tasks.insert(task->id, task);
    m_state->order.append(task->id);
    if (!m_state->persist(error)) {
        m_state->tasks.remove(task->id);
        m_state->order.removeAll(task->id);
        QFile::remove(marker);
        QDir().rmdir(task->directory);
        return ServiceResult::failure(error);
    }
    if (!m_state->start(task, error)) {
        m_state->fail(task, error);
        return ServiceResult::failure(error);
    }
    emit changed();
    return ServiceResult::success(m_state->taskData(*task));
}

QVariantMap TorrentService::files(const QString& id) const
{
    if (!m_state->loadError.isEmpty())
        return ServiceResult::failure(m_state->loadError);
    const auto task = m_state->tasks.value(id);
    if (!task || !task->metadata.ti)
        return ServiceResult::failure(QStringLiteral("磁力文件列表尚未解析完成。"));
    QVariantList files;
    qint64 total = 0;
    for (const auto& value : task->fileList) {
        auto file = value.toMap();
        file.insert(QStringLiteral("selected"), task->selected.contains(file.value(QStringLiteral("index")).toInt()));
        total += file.value(QStringLiteral("size")).toLongLong();
        files.append(file);
    }
    return ServiceResult::success(QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("fileName"), task->name},
        {QStringLiteral("files"), files}, {QStringLiteral("totalBytes"), total}});
}

QVariantMap TorrentService::confirmFiles(const QString& id, const QVariantList& indices)
{
    if (!m_state->loadError.isEmpty())
        return ServiceResult::failure(m_state->loadError);
    const auto task = m_state->tasks.value(id);
    if (!task || !task->metadata.ti || task->confirmed
        || (task->status != QStringLiteral("awaiting_selection") && task->status != QStringLiteral("paused")))
        return ServiceResult::failure(QStringLiteral("该任务当前不能确认文件选择。"));
    QSet<int> available, selected;
    for (const auto& file : task->fileList)
        available.insert(file.toMap().value(QStringLiteral("index")).toInt());
    for (const auto& value : indices) {
        bool valid = false;
        const double number = value.toDouble(&valid);
        if (!valid || value.metaType().id() == QMetaType::QString || value.metaType().id() == QMetaType::Bool
            || !std::isfinite(number) || number < 0 || number > std::numeric_limits<int>::max() || std::floor(number) != number
            || !available.contains(int(number)) || selected.contains(int(number)))
            return ServiceResult::failure(QStringLiteral("文件选择包含无效或重复的索引。"));
        selected.insert(int(number));
    }
    if (selected.isEmpty())
        return ServiceResult::failure(QStringLiteral("请至少选择一个文件后开始下载。"));
    QString error;
    if (!m_state->safeFiles(*task, true, error))
        return ServiceResult::failure(error);
    const qint64 previousTotal = task->total;
    const QString previousStatus = task->status; // 确认事务中断时，已保存的选择应显示为可继续任务。
    task->selected = selected;
    task->confirmed = true;
    task->status = QStringLiteral("paused");
    task->total = 0;
    for (const auto& value : task->fileList) {
        const auto file = value.toMap();
        if (selected.contains(file.value(QStringLiteral("index")).toInt()))
            task->total += file.value(QStringLiteral("size")).toLongLong();
    }
    if (!m_state->persist(error)) {
        task->selected.clear();
        task->confirmed = false;
        task->total = previousTotal;
        task->status = previousStatus;
        return ServiceResult::failure(error);
    }
    if (!m_state->start(task, error)) {
        m_state->fail(task, error);
        return ServiceResult::failure(error);
    }
    emit changed();
    return ServiceResult::success(m_state->taskData(*task));
}

QVariantMap TorrentService::pauseTask(const QString& id)
{
    if (!m_state->loadError.isEmpty())
        return ServiceResult::failure(m_state->loadError);
    const auto task = m_state->tasks.value(id);
    if (!task || !State::active(*task))
        return ServiceResult::failure(QStringLiteral("只有解析中或下载中的磁力任务可以暂停。"));
    if (task->handle.is_valid())
        task->handle.pause();
    task->finishing = false;
    task->status = QStringLiteral("paused");
    task->speed = 0;
    m_state->publish();
    return ServiceResult::success(m_state->taskData(*task));
}

QVariantMap TorrentService::resumeTask(const QString& id)
{
    if (!m_state->loadError.isEmpty())
        return ServiceResult::failure(m_state->loadError);
    const auto task = m_state->tasks.value(id);
    if (!task || task->cleanupPending || (task->status != QStringLiteral("paused")
        && task->status != QStringLiteral("failed") && task->status != QStringLiteral("awaiting_selection")))
        return ServiceResult::failure(QStringLiteral("只有暂停、失败或等待文件选择的磁力任务可以继续。"));
    QString error;
    if (!m_state->start(task, error)) {
        m_state->fail(task, error);
        return ServiceResult::failure(error);
    }
    emit changed();
    return ServiceResult::success(m_state->taskData(*task));
}

QVariantMap TorrentService::cancelTask(const QString& id)
{
    if (!m_state->loadError.isEmpty())
        return ServiceResult::failure(m_state->loadError);
    const auto task = m_state->tasks.value(id);
    if (!task || task->status == QStringLiteral("completed"))
        return ServiceResult::failure(QStringLiteral("磁力任务不存在或已经完成，无法取消。"));
    QString error;
    if (!m_state->safeFiles(*task, false, error))
        return ServiceResult::failure(error);
    const QString previous = State::active(*task) ? QStringLiteral("paused") : task->status;
    if (task->handle.is_valid())
        task->handle.pause();
    task->status = QStringLiteral("cancelled");
    task->finishing = false;
    task->cleanupPending = true;
    task->speed = 0;
    if (!m_state->persist(error)) {
        task->status = previous;
        task->cleanupPending = false;
        emit changed();
        return ServiceResult::failure(error);
    }
    if (task->handle.is_valid() && task->confirmed) {
        task->handle.flush_cache();
    } else {
        if (task->handle.is_valid() && m_state->session)
            m_state->session->remove_torrent(task->handle);
        task->handle = {};
        if (!m_state->cleanup(task, error)) {
            task->warning = error;
            m_state->publish();
            return ServiceResult::failure(error);
        }
        m_state->publish();
    }
    emit changed();
    return ServiceResult::success(m_state->taskData(*task));
}

QVariantMap TorrentService::removeTask(const QString& id)
{
    if (!m_state->loadError.isEmpty())
        return ServiceResult::failure(m_state->loadError);
    const auto task = m_state->tasks.value(id);
    if (!task || (task->status != QStringLiteral("completed") && task->status != QStringLiteral("cancelled")
        && task->status != QStringLiteral("failed")))
        return ServiceResult::failure(QStringLiteral("请先取消磁力任务，再移除记录。"));
    if (task->cleanupPending || task->handle.is_valid())
        return ServiceResult::failure(QStringLiteral("磁力文件仍在关闭或清理，请稍后再移除记录。"));
    const qsizetype position = m_state->order.indexOf(id);
    m_state->tasks.remove(id);
    m_state->order.removeAt(position);
    QString error;
    if (!m_state->persist(error)) {
        m_state->tasks.insert(id, task);
        m_state->order.insert(position, id);
        return ServiceResult::failure(error);
    }
    const QString metadata = m_state->metadataPath(*task);
    if (m_state->safeStore() && plainPath(metadata))
        QFile::remove(metadata);
    emit changed();
    return ServiceResult::success();
}

QVariantMap TorrentService::openDirectory(const QString& id)
{
    if (!m_state->loadError.isEmpty())
        return ServiceResult::failure(m_state->loadError);
    const auto task = m_state->tasks.value(id);
    QString error;
    if (!task || !m_state->ownsDirectory(*task, error))
        return ServiceResult::failure(error.isEmpty() ? QStringLiteral("磁力任务不存在。") : error);
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(task->directory)))
        return ServiceResult::failure(QStringLiteral("无法打开磁力下载目录。"));
    return ServiceResult::success(m_state->taskData(*task));
}
