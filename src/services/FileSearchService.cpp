#include "services/FileSearchService.h"

#include "services/FileSearchIndex.h"
#include "services/ServiceResult.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSet>
#include <QThread>
#include <QCoreApplication>
#include <QTimer>
#include <mutex>
#include <limits>
#include <QtEndian>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#include <winioctl.h>
#include <shellapi.h>
#include "FileJournalClient.h"
#endif

namespace
{
// 变更轮询与扫描事件回收周期。
constexpr int HeartbeatIntervalMs = 2000;
// USN 单次心跳最多读取的批次数，避免持续写入的磁盘饿死其他队列任务。
constexpr int MaximumUsnBatchesPerPoll = 16;
// 扫描构建期间每隔多少条记录刷新一次状态。
constexpr qint64 ScanProgressBatch = 20000;
// 目录变更监视缓冲区大小。
constexpr DWORD WatchBufferSize = 64 * 1024;

#ifdef Q_OS_WIN
// 当前进程是否具有管理员权限（决定能否直接枚举 NTFS 主文件表）。
bool isProcessElevated()
{
    BOOL elevated = FALSE;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation{};
        DWORD returned = 0;
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned))
            elevated = elevation.TokenIsElevated;
        CloseHandle(token);
    }
    return elevated != FALSE;
}

// 返回卷设备路径（\\.\C:），用于打开卷句柄。
QString volumeDevicePath(int driveIndex)
{
    return QStringLiteral("\\\\.\\%1:").arg(QChar(ushort(u'A' + driveIndex - 1)));
}

// 把 Win32 错误码转成可读的中文说明。
QString win32ErrorText(DWORD error)
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    QString text = length > 0 && buffer ? QString::fromWCharArray(buffer, int(length)).trimmed()
                                        : QStringLiteral("未知错误 %1").arg(error);
    if (buffer)
        LocalFree(buffer);
    return text;
}
#endif
}

// 工作线程对象：持有索引、卷信息、USN 轮询定时器与目录变更监视线程。
class FileSearchWorker final : public QObject
{
    Q_OBJECT
public:
    explicit FileSearchWorker(QString dataRoot, QStringList rootOverrides)
        : m_dataRoot(std::move(dataRoot)), m_rootOverrides(std::move(rootOverrides))
    {
    }

    ~FileSearchWorker() override
    {
        stopHeartbeat();
        stopMonitors();
    }

    // 线程安全：请求尽快打断长时间构建（重建索引或关闭窗口时调用）。
    void requestAbort() { m_abort.store(true); }
    // 永久停止标记防止排队中的初始化或重建清除关闭请求。
    void requestStop() { m_stopping.store(true); m_abort.store(true); }
    // 新输入立即使旧检索失效，正在运行的查询会定期检查。
    void selectRequest(int id) { m_latestRequest.store(id); }

public slots:
    // 启动入口：优先加载持久化索引，否则全量构建。
    void initialize();
    // 丢弃当前索引并重建全部卷。
    void rebuild();
    // 执行一次检索并回发结果。
    void executeSearch(int requestId, const QString& raw, int offset, int limit, bool searchPath);
    // 周期任务：USN 增量读取与目录变更回收。
    void heartbeat();
    // 关闭前停止全部监视。
    void shutdown()
    {
        stopHeartbeat();
        stopMonitors();
        checkpoint();
    }
    // 监视线程回投一个发生变更的目录（原生大小写路径）。
    void enqueueScanEvent(int driveIndex, const QString& directory);

signals:
    void statusChanged(const QVariantMap& status);
    void searchFinished(const QVariantMap& result);

private:
    // 一个目录变更监视线程的句柄集合。
    struct ScanWatch
    {
        int driveIndex = 0;
        // 由监视线程发布、主控线程读取，用原子量避免句柄关闭竞态。
        std::atomic<void*> directory{nullptr}; // HANDLE
        std::unique_ptr<std::atomic_bool> stop;
        std::thread thread;
    };

    void startHeartbeat();
    void stopHeartbeat();
    void discoverVolumes();
    void buildAll();
    bool buildVolume(FileSearchVolume& volume);
#ifdef Q_OS_WIN
    bool buildVolumeMft(FileSearchVolume& volume, FileJournalVolume* handle);
    void pollUsnJournals();
    void applyUsnBatch(FileSearchVolume& volume, const char* data, int bytes, bool* changed);
    FileJournalVolume* openVolumeHandle(const FileSearchVolume& volume);
#endif
    bool buildVolumeScan(const FileSearchVolume& volume);
    void scanDirectory(const QString& directory, quint64 parentKey, int driveIndex);
    void flushScanEvents();
    void startMonitors();
    void stopMonitors();
    // 把索引和断点作为一个原子快照保存，限制写盘频率。
    void checkpoint();
    void emitStatus(bool force = false);
    QVariantMap statusSnapshot() const;
    QString persistPath() const;

    QString m_dataRoot;
    QStringList m_rootOverrides;
    FileSearchIndex m_index;
    QVector<FileSearchVolume> m_volumes;
    // 运行期卷状态：pending/building/ready/failed。
    QHash<int, QString> m_volumeStatus;
    QHash<int, QString> m_volumeErrors;
#ifdef Q_OS_WIN
    QHash<int, FileJournalVolume*> m_handles; // mft 卷设备句柄
#endif
    // 最新查询编号用于取消过期搜索。
    std::atomic_int m_latestRequest{0};
    // 快照仅在内容或日志断点改变后写入，最多每五分钟一次。
    bool m_dirty = false;
    qint64 m_checkpointAt = 0;
    // 未安装权限代理时明确提示启用服务，不静默扫描 NTFS。
    bool m_serviceRequired = false;
    // 分时轮询的下一卷，繁忙磁盘不能长期占用查询线程。
    int m_nextPollVolume = 0;
    std::vector<std::unique_ptr<ScanWatch>> m_watches;
    QVector<QPair<int, QString>> m_pendingDirs;
    // 独立节拍线程只允许保留一个待处理心跳，避免忙碌期间队列增长。
    std::thread m_heartbeatThread;
    std::unique_ptr<std::atomic_bool> m_heartbeatStop;
    // 尚未处理的节拍由原子标记合并。
    std::atomic_bool m_heartbeatPending{false};
    // 监视线程直接合并目录事件，避免向 Qt 队列无限投递。
    std::mutex m_pendingMutex;
    bool m_building = false;
    bool m_ready = false;
    bool m_persisted = false;
    bool m_elevated = false;
    QString m_lastError;
    QString m_lastBuildAt;
    qint64 m_buildElapsedMs = 0;
    std::atomic_bool m_abort{false};
    // 服务销毁后不允许再次启动构建。
    std::atomic_bool m_stopping{false};
    qint64 m_lastStatusEmitMs = 0;
};

void FileSearchWorker::startHeartbeat()
{
    stopHeartbeat();
    m_heartbeatStop = std::make_unique<std::atomic_bool>(false);
    std::atomic_bool* stop = m_heartbeatStop.get();
    m_heartbeatThread = std::thread([this, stop]() {
        for (;;) {
            for (int waited = 0; waited < HeartbeatIntervalMs; waited += 100) {
                if (stop->load())
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (stop->load())
                return;
            if (!m_heartbeatPending.exchange(true)) {
                QMetaObject::invokeMethod(this, [this]() {
                    heartbeat();
                    m_heartbeatPending.store(false);
                }, Qt::QueuedConnection);
            }
        }
    });
}

void FileSearchWorker::stopHeartbeat()
{
    if (m_heartbeatStop)
        m_heartbeatStop->store(true);
    if (m_heartbeatThread.joinable())
        m_heartbeatThread.join();
    m_heartbeatStop.reset();
}

void FileSearchWorker::initialize()
{
    startHeartbeat();
#ifdef Q_OS_WIN
    m_elevated = isProcessElevated();
#endif
    discoverVolumes();
    if (!m_rootOverrides.isEmpty()) {
        // 测试注入：只索引指定目录，跳过持久化。
        buildAll();
        return;
    }
    QVector<FileSearchVolume> stored;
    FileSearchIndex loaded;
    QString error;
    if (!FileSearchIndex::load(persistPath(), &stored, &loaded, &error)) {
        buildAll();
        return;
    }
    m_index = std::move(loaded);
    m_dirty = false;
    m_building = true;
    // 卷身份、日志身份和断点仍有效才复用；其他卷不受单卷失效影响。
    for (const auto& old : stored) {
        const bool present = std::any_of(m_volumes.cbegin(), m_volumes.cend(),
            [&old](const auto& current) { return old.driveIndex == current.driveIndex; });
        if (!present) { m_index.remove(m_index.volumeKeys(old.driveIndex)); m_dirty = true; }
    }
    for (auto& volume : m_volumes) {
        if (m_stopping.load()) return;
        bool reusable = false;
#ifdef Q_OS_WIN
        for (const auto& old : stored) {
            if (old.driveIndex != volume.driveIndex || !volume.mftMode || !old.mftMode
                || old.serialNumber != volume.serialNumber || old.displayRoot != volume.displayRoot) continue;
            auto* handle = openVolumeHandle(volume);
            USN_JOURNAL_DATA_V0 journal{};
            DWORD bytes = 0;
            if (handle && readFileJournal(handle, FSCTL_QUERY_USN_JOURNAL, nullptr, 0,
                    &journal, sizeof(journal), &bytes) && bytes >= sizeof(journal)
                && old.journalId == journal.UsnJournalID && old.nextUsn >= quint64(journal.FirstUsn)
                && old.nextUsn >= quint64(journal.LowestValidUsn) && old.nextUsn <= quint64(journal.NextUsn)) {
                volume.journalId = old.journalId;
                volume.nextUsn = old.nextUsn;
                m_handles.insert(volume.driveIndex, handle);
                reusable = true;
            } else closeFileJournalVolume(handle);
            break;
        }
#endif
        if (!reusable) {
            m_dirty = true;
            m_index.remove(m_index.volumeKeys(volume.driveIndex));
            m_volumeStatus[volume.driveIndex] = QStringLiteral("building");
            emitStatus(true);
            reusable = buildVolume(volume);
            if (!reusable) {
                m_index.remove(m_index.volumeKeys(volume.driveIndex));
                volume.journalId = 0;
                m_lastError = m_volumeErrors.value(volume.driveIndex);
            }
        }
        m_volumeStatus[volume.driveIndex] = reusable ? QStringLiteral("ready") : QStringLiteral("failed");
    }
    m_building = false;
    m_ready = m_volumeStatus.values().contains(QStringLiteral("ready"));
    m_persisted = true;
    m_checkpointAt = QDateTime::currentMSecsSinceEpoch();
    startMonitors();
    heartbeat();
    emitStatus(true);
}

void FileSearchWorker::rebuild()
{
    // 手动重建或首次安装服务后重新检测卷身份与读取能力。
    stopMonitors();
    m_abort.store(false);
    if (!m_stopping.load()) { discoverVolumes(); buildAll(); }
}

void FileSearchWorker::discoverVolumes()
{
    m_volumes.clear();
    m_volumeStatus.clear();
    m_volumeErrors.clear();
    m_serviceRequired = false;
    if (!m_rootOverrides.isEmpty()) {
        int index = 1;
        for (const QString& raw : std::as_const(m_rootOverrides)) {
            const QDir directory(raw.trimmed());
            FileSearchVolume volume;
            volume.driveIndex = index++;
            volume.displayRoot = QDir::toNativeSeparators(directory.absolutePath());
            volume.fileSystem = QStringLiteral("DIR");
            volume.mftMode = false;
            if (!directory.exists())
                m_volumeErrors[volume.driveIndex] = QStringLiteral("目录不存在：%1").arg(volume.displayRoot);
            m_volumes.append(volume);
            m_volumeStatus[volume.driveIndex] = QStringLiteral("pending");
        }
        return;
    }
#ifdef Q_OS_WIN
    const quint32 drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i)))
            continue;
        const QString letter = QString(QChar(ushort(u'A' + i)));
        const QString rootPath = letter + QStringLiteral(":\\");
        if (GetDriveTypeW(reinterpret_cast<const wchar_t*>(rootPath.utf16())) != DRIVE_FIXED)
            continue;
        wchar_t fileSystem[MAX_PATH + 1] = {};
        DWORD serial = 0;
        if (!GetVolumeInformationW(reinterpret_cast<const wchar_t*>(rootPath.utf16()),
                nullptr, 0, &serial, nullptr, nullptr, fileSystem, MAX_PATH))
            continue;
        FileSearchVolume volume;
        volume.driveIndex = i + 1;
        volume.displayRoot = letter + QStringLiteral(":");
        volume.fileSystem = QString::fromWCharArray(fileSystem);
        volume.serialNumber = serial;
        // NTFS 始终使用日志模式；普通用户通过已安装的服务读取。
        volume.mftMode = volume.fileSystem.compare(QStringLiteral("NTFS"), Qt::CaseInsensitive) == 0;
        if (volume.mftMode && !m_elevated && !fileJournalServiceRunning())
            m_serviceRequired = true;
        m_volumes.append(volume);
        m_volumeStatus[volume.driveIndex] = QStringLiteral("pending");
    }
#endif
}

void FileSearchWorker::buildAll()
{
    m_abort.store(false);
    if (m_stopping.load()) {
        m_abort.store(true);
        return;
    }
    stopMonitors();
    m_index.clear();
    m_building = true;
    m_ready = false;
    m_persisted = false;
    m_lastError.clear();
    QElapsedTimer timer;
    timer.start();
    emitStatus(true);
    for (FileSearchVolume& volume : m_volumes) {
        if (m_abort.load())
            break;
        m_volumeStatus[volume.driveIndex] = QStringLiteral("building");
        emitStatus(true);
        const bool ok = buildVolume(volume);
        m_volumeStatus[volume.driveIndex] = ok ? QStringLiteral("ready") : QStringLiteral("failed");
        if (!ok) {
            m_index.remove(m_index.volumeKeys(volume.driveIndex));
            volume.journalId = 0;
            const QString error = m_volumeErrors.value(volume.driveIndex);
            if (!error.isEmpty() && m_lastError.isEmpty())
                m_lastError = error;
        }
        emitStatus(true);
    }
    if (!m_abort.load())
        m_index.dropOrphans();
    m_building = false;
    if (m_abort.load()) {
        m_ready = false;
        emitStatus(true);
        return;
    }
    m_ready = m_volumeStatus.values().contains(QStringLiteral("ready"));
    m_buildElapsedMs = timer.elapsed();
    m_lastBuildAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    m_dirty = true;
    checkpoint();
    startMonitors();
    emitStatus(true);
}

bool FileSearchWorker::buildVolume(FileSearchVolume& volume)
{
#ifdef Q_OS_WIN
    if (volume.mftMode) {
        auto* handle = m_handles.value(volume.driveIndex, nullptr);
        if (!handle) {
            handle = openVolumeHandle(volume);
            if (handle)
                m_handles.insert(volume.driveIndex, handle);
        }
        if (!handle) {
            m_volumeErrors[volume.driveIndex] = QStringLiteral(
                "无法打开卷 %1（请先启用 NTFS 索引服务）。").arg(volume.displayRoot);
            return false;
        }
        const bool ok = buildVolumeMft(volume, handle);
        if (!ok && m_volumeErrors.value(volume.driveIndex).isEmpty())
            m_volumeErrors[volume.driveIndex] = QStringLiteral("枚举卷 %1 失败。").arg(volume.displayRoot);
        return ok;
    }
    if (!m_volumeErrors.value(volume.driveIndex).isEmpty())
        return false; // 发现阶段已确认目录无效。
#endif
    return buildVolumeScan(volume);
}

#ifdef Q_OS_WIN
FileJournalVolume* FileSearchWorker::openVolumeHandle(const FileSearchVolume& volume)
{
    return openFileJournalVolume(DWORD(volume.driveIndex));
}

bool FileSearchWorker::buildVolumeMft(FileSearchVolume& volume, FileJournalVolume* handle)
{
    USN_JOURNAL_DATA_V0 journal{};
    DWORD bytes = 0;
    if (!readFileJournal(handle, FSCTL_QUERY_USN_JOURNAL, nullptr, 0,
            &journal, sizeof(journal), &bytes) || bytes < sizeof(journal)) {
        m_volumeErrors[volume.driveIndex] =
            QStringLiteral("读取 USN 日志失败：") + win32ErrorText(GetLastError());
        return false;
    }
    // 先取断点再枚举：枚举期间发生的变更会由心跳增量读取补齐。
    volume.journalId = journal.UsnJournalID;
    volume.nextUsn = journal.NextUsn;
    m_index.upsert(FileSearchIndex::key(volume.driveIndex, 0), 0, volume.displayRoot, true);
    QByteArray buffer(65536, Qt::Uninitialized);
    quint64 startFrn = 0;
    for (;;) {
        if (m_abort.load())
            return false; // 未完成枚举不能保存为可续接的完整索引。
        MFT_ENUM_DATA_V0 request{};
        request.StartFileReferenceNumber = startFrn;
        request.LowUsn = 0;
        // 枚举期间变更的数据记录也必须收集，增量读取从枚举前的断点补齐。
        request.HighUsn = std::numeric_limits<qint64>::max();
        DWORD returned = 0;
        if (!readFileJournal(handle, FSCTL_ENUM_USN_DATA, &request, sizeof(request),
                buffer.data(), buffer.size(), &returned)) {
            const DWORD error = GetLastError();
            if (error == ERROR_HANDLE_EOF)
                break;
            m_volumeErrors[volume.driveIndex] =
                QStringLiteral("枚举主文件表失败：") + win32ErrorText(error);
            return false;
        }
        if (returned <= sizeof(quint64))
            break;
        startFrn = qFromLittleEndian<quint64>(buffer.constData());
        QString error;
        if (!m_index.applyJournalRecords(volume.driveIndex, QByteArray(buffer.constData(), returned), true, &error)) {
            m_volumeErrors[volume.driveIndex] = error;
            return false;
        }
        emitStatus();
    }
    return true;
}

void FileSearchWorker::pollUsnJournals()
{
    QElapsedTimer budget;
    budget.start();
    const int start = m_nextPollVolume;
    for (int count = 0; count < m_volumes.size(); ++count) {
        const int position = (start + count) % m_volumes.size();
        auto& volume = m_volumes[position];
        m_nextPollVolume = (position + 1) % m_volumes.size();
        if (m_abort.load() || !volume.mftMode || m_volumeStatus.value(volume.driveIndex) != QStringLiteral("ready"))
            continue;
        auto* handle = m_handles.value(volume.driveIndex, nullptr);
        if (!handle) {
            handle = openVolumeHandle(volume);
            if (!handle)
                continue;
            m_handles.insert(volume.driveIndex, handle);
        }
        bool changed = false;
        int batches = 0;
        while (!m_abort.load() && budget.elapsed() < 50 && batches++ < MaximumUsnBatchesPerPoll) {
            READ_USN_JOURNAL_DATA_V0 request{};
            request.UsnJournalID = volume.journalId;
            request.StartUsn = volume.nextUsn;
            request.ReasonMask = USN_REASON_FILE_CREATE | USN_REASON_FILE_DELETE | USN_REASON_RENAME_NEW_NAME;
            request.BytesToWaitFor = 0;
            request.Timeout = 0;
            char buffer[WatchBufferSize];
            DWORD returned = 0;
            if (!readFileJournal(handle, FSCTL_READ_USN_JOURNAL, &request, sizeof(request),
                    buffer, sizeof(buffer), &returned)) {
                const DWORD error = GetLastError();
                if (error == ERROR_HANDLE_EOF)
                    break;
                // 暂时断开或权限错误只报告，不反复重扫；仅日志失效才重建该卷。
                if (error != ERROR_JOURNAL_ENTRY_DELETED && error != ERROR_JOURNAL_DELETE_IN_PROGRESS
                    && error != ERROR_INVALID_PARAMETER) {
                    m_volumeErrors[volume.driveIndex] = win32ErrorText(error);
                    m_lastError = m_volumeErrors.value(volume.driveIndex);
                    if (error == ERROR_SERVICE_NOT_ACTIVE) m_serviceRequired = true;
                    emitStatus(true);
                    break;
                }
                m_index.remove(m_index.volumeKeys(volume.driveIndex));
                m_volumeStatus[volume.driveIndex] = QStringLiteral("building");
                emitStatus(true);
                const bool ok = buildVolume(volume);
                m_volumeStatus[volume.driveIndex] =
                    ok ? QStringLiteral("ready") : QStringLiteral("failed");
                if (!ok) { m_index.remove(m_index.volumeKeys(volume.driveIndex)); volume.journalId = 0; }
                m_dirty = true;
                changed = true;
                break;
            }
            if (returned < sizeof(USN)) { m_volumeErrors[volume.driveIndex] = QStringLiteral("USN 响应不完整。"); break; }
            applyUsnBatch(volume, buffer, int(returned), &changed);
            if (m_volumeStatus.value(volume.driveIndex) == QStringLiteral("failed")) { emitStatus(true); break; }
            if (returned <= sizeof(USN))
                break;
        }
        if (changed) emitStatus();
        if (budget.elapsed() >= 50) break;
    }
}

void FileSearchWorker::applyUsnBatch(FileSearchVolume& volume, const char* data, int bytes,
                                     bool* changed)
{
    QString error;
    if (!m_index.applyJournalRecords(volume.driveIndex, QByteArray(data, bytes), false, &error)) {
        m_volumeErrors[volume.driveIndex] = error;
        m_lastError = error;
        m_volumeStatus[volume.driveIndex] = QStringLiteral("failed");
        return;
    }
    const quint64 next = qFromLittleEndian<quint64>(data);
    if (next != volume.nextUsn) m_dirty = true;
    volume.nextUsn = next;
    if (bytes > int(sizeof(USN))) *changed = true;
}
#endif

bool FileSearchWorker::buildVolumeScan(const FileSearchVolume& volume)
{
    // 根记录的名称即路径前缀；子记录沿父链拼接完整路径。
    const quint64 rootKey = FileSearchIndex::scanKey(volume.driveIndex, volume.displayRoot);
    m_index.upsert(rootKey, 0, volume.displayRoot, true);
    scanDirectory(volume.displayRoot, rootKey, volume.driveIndex);
    return true;
}

void FileSearchWorker::scanDirectory(const QString& directory, quint64 parentKey, int driveIndex)
{
#ifdef Q_OS_WIN
    // 显式栈遍历并跳过重解析点，避开 junction 造成的环；每项携带自身键。
    QVector<QPair<QString, quint64>> stack;
    stack.append({directory, parentKey});
    qint64 processed = 0;
    while (!stack.isEmpty()) {
        if (m_abort.load())
            return;
        const auto [current, currentKey] = stack.takeLast();
        const QString pattern = current + QStringLiteral("\\*");
        WIN32_FIND_DATAW data{};
        void* find = FindFirstFileExW(reinterpret_cast<const wchar_t*>(pattern.utf16()),
            FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE)
            continue;
        do {
            if (m_abort.load())
                break;
            const QString name = QString::fromWCharArray(data.cFileName);
            if (name == QLatin1String(".") || name == QLatin1String(".."))
                continue;
            const QString full = current + QStringLiteral("\\") + name;
            const bool isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const quint64 key = FileSearchIndex::scanKey(driveIndex, full);
            m_index.upsert(key, currentKey, name, isDirectory);
            if (isDirectory && (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                stack.append({full, key});
            }
            if (++processed % ScanProgressBatch == 0)
                emitStatus();
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
#else
    Q_UNUSED(directory);
    Q_UNUSED(parentKey);
    Q_UNUSED(driveIndex);
#endif
}

void FileSearchWorker::flushScanEvents()
{
    QVector<QPair<int, QString>> pending;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        pending.swap(m_pendingDirs);
    }
    if (pending.isEmpty() || m_abort.load())
        return;
    // 去重并只保留最上层目录：子目录的重扫会被父目录覆盖。
    QVector<QPair<int, QString>> dirty;
    QSet<QString> seen;
    for (const auto& event : std::as_const(pending)) {
        const QString key = QString::number(event.first) + QLatin1Char('|') + event.second.toLower();
        if (!seen.contains(key)) {
            seen.insert(key);
            dirty.append(event);
        }
    }
    std::sort(dirty.begin(), dirty.end(), [](const auto& left, const auto& right) {
        if (left.first != right.first)
            return left.first < right.first;
        return left.second.compare(right.second, Qt::CaseInsensitive) < 0;
    });
    bool changed = false;
    for (int i = 0; i < dirty.size(); ++i) {
        if (m_abort.load())
            return;
        bool covered = false;
        for (int j = 0; j < i && !covered; ++j) {
            covered = dirty[j].first == dirty[i].first
                && dirty[i].second.startsWith(dirty[j].second + QLatin1Char('\\'), Qt::CaseInsensitive);
        }
        if (covered)
            continue;
        FileSearchVolume* volume = nullptr;
        for (FileSearchVolume& candidate : m_volumes) {
            if (candidate.driveIndex == dirty[i].first && !candidate.mftMode) {
                volume = &candidate;
                break;
            }
        }
        if (!volume)
            continue;
        // 自该目录向上找到最近一个已知目录（新目录的变更按其父目录重扫）。
        const QString lookup = dirty[i].second.toLower();
        QString known = lookup;
        quint64 parentKey = 0;
        for (;;) {
            const quint64 knownKey = FileSearchIndex::scanKey(volume->driveIndex, known);
            if (m_index.contains(knownKey)) {
                parentKey = knownKey;
                break;
            }
            const int slash = known.lastIndexOf(QLatin1Char('\\'));
            if (slash <= 0) {
                parentKey = 0;
                break;
            }
            known = known.left(slash);
        }
        if (parentKey == 0) {
            // 连卷根都未知（如根目录被删除）：清除本卷后整卷重建。
            m_index.remove(m_index.volumeKeys(volume->driveIndex));
            buildVolume(*volume);
            changed = true;
            continue;
        }
        QString knownPath = m_index.pathOf(parentKey);
        if (knownPath.isEmpty())
            continue;
        const QVector<quint64> children = m_index.childKeys(parentKey);
        if (!children.isEmpty()) {
            m_index.remove(children);
            changed = true;
        }
        // 目录可能已被删除；扫描函数对缺失目录自动跳过。
        if (QFileInfo::exists(knownPath)) {
            scanDirectory(knownPath, parentKey, volume->driveIndex);
            // 空目录首次新增文件也需要通知界面更新索引数量。
            changed = true;
        }
    }
    if (changed) {
        m_dirty = true;
        emitStatus(true);
    }
}

void FileSearchWorker::startMonitors()
{
    stopMonitors();
#ifdef Q_OS_WIN
    for (const FileSearchVolume& volume : std::as_const(m_volumes)) {
        if (volume.mftMode) {
            // USN 心跳即可覆盖，无需额外线程。
            continue;
        }
        auto watch = std::make_unique<ScanWatch>();
        watch->driveIndex = volume.driveIndex;
        watch->stop = std::make_unique<std::atomic_bool>(false);
        const QString watchRoot = volume.displayRoot + QStringLiteral("\\");
        const QString rootDisplay = volume.displayRoot;
        ScanWatch* watchPointer = watch.get();
        FileSearchWorker* worker = this;
        watch->thread = std::thread([worker, watchPointer, watchRoot, rootDisplay]() {
            void* directory = CreateFileW(reinterpret_cast<const wchar_t*>(watchRoot.utf16()),
                FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
            if (directory == INVALID_HANDLE_VALUE)
                return;
            // 句柄必须先于任何阻塞等待发布，且停止请求在发布前到达时自行退出。
            if (watchPointer->stop->load()) {
                CloseHandle(directory);
                return;
            }
            watchPointer->directory.store(directory);
            // 使用可超时的重叠 I/O，使停止请求不会与下一次阻塞读取发生竞态。
            HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!event) {
                watchPointer->directory.store(nullptr);
                CloseHandle(directory);
                return;
            }
            std::vector<char> buffer(WatchBufferSize);
            for (;;) {
                DWORD bytes = 0;
                if (watchPointer->stop->load())
                    break;
                OVERLAPPED overlapped{};
                overlapped.hEvent = event;
                ResetEvent(event);
                if (!ReadDirectoryChangesW(directory, buffer.data(), WatchBufferSize, TRUE,
                        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
                        nullptr, &overlapped, nullptr))
                    break;
                DWORD waitResult = WAIT_TIMEOUT;
                while (!watchPointer->stop->load() && waitResult == WAIT_TIMEOUT)
                    waitResult = WaitForSingleObject(event, 100);
                if (watchPointer->stop->load() || waitResult != WAIT_OBJECT_0) {
                    CancelIoEx(directory, &overlapped);
                    GetOverlappedResult(directory, &overlapped, &bytes, TRUE);
                    break;
                }
                if (!GetOverlappedResult(directory, &overlapped, &bytes, FALSE))
                    break;
                // 字节为 0 说明缓冲区溢出，通知整卷重扫。
                if (bytes == 0) {
                    worker->enqueueScanEvent(watchPointer->driveIndex, rootDisplay);
                    continue;
                }
                const char* pointer = buffer.data();
                const char* end = buffer.data() + bytes;
                while (pointer + sizeof(FILE_NOTIFY_INFORMATION) <= end) {
                    const auto info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(pointer);
                    // 只关心名字变化；修改内容不影响文件名索引。
                    if (info->Action != FILE_ACTION_MODIFIED) {
                        const QString relative = QString::fromWCharArray(info->FileName,
                            int(info->FileNameLength / sizeof(WCHAR)));
                        const int slash = relative.lastIndexOf(QLatin1Char('\\'));
                        const QString changedDirectory =
                            slash >= 0 ? watchRoot + relative.left(slash) : watchRoot;
                        worker->enqueueScanEvent(watchPointer->driveIndex, changedDirectory);
                    }
                    if (info->NextEntryOffset == 0)
                        break;
                    pointer += info->NextEntryOffset;
                }
            }
            // 句柄仅由创建它的线程关闭，避免关闭后被其他线程复用。
            watchPointer->directory.store(nullptr);
            CloseHandle(event);
            CloseHandle(directory);
        });
        m_watches.push_back(std::move(watch));
    }
#endif
}

void FileSearchWorker::stopMonitors()
{
    for (auto& watch : m_watches) {
        if (watch->stop)
            watch->stop->store(true);
        if (watch->thread.joinable())
            watch->thread.join();
    }
    m_watches.clear();
#ifdef Q_OS_WIN
    for (auto* handle : std::as_const(m_handles)) {
        closeFileJournalVolume(handle);
    }
    m_handles.clear();
#endif
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingDirs.clear();
}

void FileSearchWorker::checkpoint()
{
    if (!m_dirty || !m_rootOverrides.isEmpty() || !m_ready) return;
    QDir().mkpath(QFileInfo(persistPath()).absolutePath());
    QString error;
    m_persisted = FileSearchIndex::save(persistPath(), m_volumes, m_index, &error);
    if (m_persisted) m_dirty = false;
    else m_lastError = QStringLiteral("索引保存失败：") + error;
    m_checkpointAt = QDateTime::currentMSecsSinceEpoch();
}

void FileSearchWorker::emitStatus(bool force)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!force && now - m_lastStatusEmitMs < 400)
        return;
    m_lastStatusEmitMs = now;
    emit statusChanged(statusSnapshot());
}

QVariantMap FileSearchWorker::statusSnapshot() const
{
    bool anyMft = false;
    for (const FileSearchVolume& volume : m_volumes)
        anyMft = anyMft || volume.mftMode;
    QVariantMap status;
    status.insert(QStringLiteral("phase"),
        m_ready ? QStringLiteral("ready")
                : (m_building ? QStringLiteral("building")
                              : (m_lastError.isEmpty() ? QStringLiteral("loading")
                                                       : QStringLiteral("error"))));
    status.insert(QStringLiteral("mode"),
        m_volumes.isEmpty() ? QStringLiteral("none") : (anyMft ? QStringLiteral("mft") : QStringLiteral("scan")));
    status.insert(QStringLiteral("fileCount"), m_index.fileCount());
    status.insert(QStringLiteral("directoryCount"), m_index.directoryCount());
    status.insert(QStringLiteral("total"), m_index.size());
    status.insert(QStringLiteral("persisted"), m_persisted);
    status.insert(QStringLiteral("elevated"), m_elevated);
    status.insert(QStringLiteral("serviceRequired"), m_serviceRequired);
    status.insert(QStringLiteral("lastError"), m_lastError);
    status.insert(QStringLiteral("lastBuildAt"), m_lastBuildAt);
    status.insert(QStringLiteral("buildElapsedMs"), m_buildElapsedMs);
    QVariantList volumes;
    for (const FileSearchVolume& volume : std::as_const(m_volumes)) {
        volumes.append(QVariantMap{
            {QStringLiteral("root"), volume.displayRoot},
            {QStringLiteral("fileSystem"), volume.fileSystem},
            {QStringLiteral("mode"), volume.mftMode ? QStringLiteral("mft") : QStringLiteral("scan")},
            {QStringLiteral("status"), m_volumeStatus.value(volume.driveIndex, QStringLiteral("pending"))},
            {QStringLiteral("error"), m_volumeErrors.value(volume.driveIndex)}});
    }
    status.insert(QStringLiteral("volumes"), volumes);
    return status;
}

QString FileSearchWorker::persistPath() const
{
    return QDir(m_dataRoot).absoluteFilePath(QStringLiteral("setting/file-search-index.bin"));
}

void FileSearchWorker::executeSearch(int requestId, const QString& raw, int offset, int limit,
                                     bool searchPath)
{
    const auto cancelled = [this, requestId]() { return m_stopping.load() || m_latestRequest.load() != requestId; };
    if (cancelled()) return;
    QElapsedTimer timer;
    timer.start();
    const FileSearchIndex::Query query = FileSearchIndex::Query::parse(raw, searchPath);
    int total = 0;
    int collected = 0;
    QVector<quint64> keys;
    if (!query.isEmpty())
        keys = m_index.search(query, qMax(0, offset), qBound(1, limit, 500), &total, &collected, cancelled);
    if (cancelled()) return;
    QVariantList rows;
    rows.reserve(keys.size());
    for (const quint64 key : std::as_const(keys)) {
        if (cancelled()) return;
        const FileSearchRecord* record = m_index.record(key);
        if (!record)
            continue;
        QString path = m_index.pathOf(key);
        if (path.isEmpty())
            path = record->name;
        QVariantMap row{{QStringLiteral("name"), record->name},
            {QStringLiteral("path"), QDir::toNativeSeparators(path)},
            {QStringLiteral("isDir"), record->isDirectory}};
        // 名称检索不再逐条访问磁盘获取大小和时间，避免慢盘阻塞结果。
        rows.append(row);
    }
    emit searchFinished(QVariantMap{{QStringLiteral("ok"), true},
        {QStringLiteral("requestId"), requestId}, {QStringLiteral("query"), raw},
        {QStringLiteral("offset"), offset}, {QStringLiteral("limit"), limit},
        {QStringLiteral("total"), total}, {QStringLiteral("collected"), collected},
        {QStringLiteral("elapsedMs"), timer.elapsed()},
        {QStringLiteral("building"), m_building}, {QStringLiteral("rows"), rows}});
}

void FileSearchWorker::heartbeat()
{
    if (m_abort.load() || m_building || !m_ready)
        return;
#ifdef Q_OS_WIN
    pollUsnJournals();
#endif
    flushScanEvents();
    if (QDateTime::currentMSecsSinceEpoch() - m_checkpointAt >= 300000) checkpoint();
}

void FileSearchWorker::enqueueScanEvent(int driveIndex, const QString& directory)
{
    if (m_abort.load())
        return;
    QString normalized = QDir::toNativeSeparators(QDir::cleanPath(directory)).toLower();
    while (normalized.endsWith(QLatin1Char('\\')))
        normalized.chop(1);
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    int count = 0;
    for (const auto& pending : std::as_const(m_pendingDirs)) {
        if (pending.first != driveIndex)
            continue;
        ++count;
        if (normalized == pending.second
            || normalized.startsWith(pending.second + QLatin1Char('\\')))
            return;
    }
    // 每卷最多保留 256 个目录；事件风暴降级为一次根目录刷新。
    if (count >= 256) {
        for (const auto& volume : std::as_const(m_volumes)) {
            if (volume.driveIndex == driveIndex) {
                normalized = volume.displayRoot.toLower();
                break;
            }
        }
    }
    for (qsizetype i = m_pendingDirs.size(); i > 0; --i) {
        const auto& pending = m_pendingDirs.at(i - 1);
        if (pending.first == driveIndex
            && (count >= 256 || pending.second.startsWith(normalized + QLatin1Char('\\'))))
            m_pendingDirs.removeAt(i - 1);
    }
    m_pendingDirs.append({driveIndex, normalized});
}

// ---- FileSearchService ----

FileSearchService::FileSearchService(const QString& dataRoot, bool enabled, QObject* parent)
    : QObject(parent), m_dataRoot(QDir(dataRoot).absolutePath()), m_enabled(enabled)
{
    // 测试注入：环境变量提供分号分隔的目录列表时，强制启用并只索引这些目录。
    QStringList rootOverrides;
    const QString overrides = qEnvironmentVariable("DESKTOPTOOL_FILESEARCH_ROOTS");
    if (!overrides.isEmpty()) {
        for (const QString& path : overrides.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
            if (!path.trimmed().isEmpty())
                rootOverrides.append(path.trimmed());
        }
        if (!rootOverrides.isEmpty())
            m_enabled = true;
    }
    if (!m_enabled) {
        m_status = QVariantMap{{QStringLiteral("phase"), QStringLiteral("disabled")},
            {QStringLiteral("mode"), QStringLiteral("none")}};
        return;
    }
    m_worker = new FileSearchWorker(m_dataRoot, rootOverrides);
    m_thread = new QThread(this);
    m_worker->moveToThread(m_thread);
    connect(m_worker, &FileSearchWorker::statusChanged, this,
        [this](const QVariantMap& status) { setStatus(status); });
    connect(m_worker, &FileSearchWorker::searchFinished, this, &FileSearchService::searchFinished);
    m_thread->start();
    QMetaObject::invokeMethod(m_worker, "initialize", Qt::QueuedConnection);
}

FileSearchService::~FileSearchService()
{
    if (!m_worker || !m_thread)
        return;
    // 永久打断构建，工作线程完成监视清理后再退出。
    m_worker->requestStop();
    QMetaObject::invokeMethod(m_worker, "shutdown", Qt::BlockingQueuedConnection);
    m_thread->quit();
    m_thread->wait();
    delete m_worker;
    m_worker = nullptr;
}

void FileSearchService::setStatus(QVariantMap status)
{
    m_status = std::move(status);
    emit statusChanged();
}

QVariantMap FileSearchService::search(const QString& query, int offset, int limit, bool searchPath)
{
    if (!m_worker)
        return ServiceResult::failure(QStringLiteral("文件检索功能未启用。"));
    const int requestId = ++m_nextRequestId;
    m_worker->selectRequest(requestId);
    QMetaObject::invokeMethod(m_worker, "executeSearch", Qt::QueuedConnection,
        Q_ARG(int, requestId), Q_ARG(QString, query), Q_ARG(int, offset), Q_ARG(int, limit),
        Q_ARG(bool, searchPath));
    return ServiceResult::success(QVariantMap{{QStringLiteral("requestId"), requestId},
        {QStringLiteral("query"), query}});
}

QVariantMap FileSearchService::rebuild()
{
    if (!m_worker)
        return ServiceResult::failure(QStringLiteral("文件检索功能未启用。"));
    // 先打断进行中的构建，再排队重建；两个请求按序到达工作线程。
    m_worker->requestAbort();
    QMetaObject::invokeMethod(m_worker, "rebuild", Qt::QueuedConnection);
    return ServiceResult::success();
}

QVariantMap FileSearchService::enableJournalService()
{
#ifdef Q_OS_WIN
    if (!m_enabled || !m_worker) return ServiceResult::failure(QStringLiteral("文件检索未启用。"));
    if (m_installing) return ServiceResult::failure(QStringLiteral("正在启用服务，请稍候。"));
    const QString executable = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("DesktopToolIndexService.exe"));
    if (!QFileInfo::exists(executable)) return ServiceResult::failure(QStringLiteral("缺少索引服务程序，请更新完整软件包。"));
    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    launch.lpVerb = L"runas";
    launch.lpFile = reinterpret_cast<const wchar_t*>(executable.utf16());
    launch.lpParameters = L"--install";
    launch.nShow = SW_HIDE;
    if (!ShellExecuteExW(&launch)) return ServiceResult::failure(GetLastError() == ERROR_CANCELLED
        ? QStringLiteral("已取消服务授权。") : QStringLiteral("无法启动服务安装器。"));
    m_installing = true;
    // 共享句柄保证窗口销毁后仍能释放句柄，不强制终止系统安装器。
    const auto process = std::shared_ptr<void>(launch.hProcess, [](void* handle) { if (handle) CloseHandle(handle); });
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this, timer, process]() {
        if (WaitForSingleObject(process.get(), 0) != WAIT_OBJECT_0) return;
        DWORD code = ERROR_GEN_FAILURE;
        GetExitCodeProcess(process.get(), &code);
        timer->stop(); timer->deleteLater(); m_installing = false;
        if (code == ERROR_SUCCESS && fileJournalServiceRunning()) rebuild();
        else {
            m_status.insert(QStringLiteral("lastError"), QStringLiteral("索引服务启用失败：") + win32ErrorText(code));
            m_status.insert(QStringLiteral("phase"), QStringLiteral("error"));
            emit statusChanged();
        }
    });
    timer->start(300);
    return ServiceResult::success();
#else
    return ServiceResult::failure(QStringLiteral("NTFS 索引服务仅支持 Windows。"));
#endif
}

#include "FileSearchService.moc"
