#include "DownloadService.h"
#include "ServiceResult.h"

#include <QDateTime>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>
#include <QUuid>
#include <limits>

namespace {
/** 单次读写缓冲区上限，下载大文件时保持内存使用稳定。 */
constexpr qint64 TransferBufferSize = 128 * 1024;
/** 任务索引的读取大小上限，拒绝异常巨大或损坏的历史文件。 */
constexpr qint64 MaximumIndexBytes = 16 * 1024 * 1024;
/** 单轮下载最多自动恢复三次，防止反复断开时无限发起请求。 */
constexpr int MaximumRetryAttempts = 3;

/** 只有连接超时或暂时中断可以自动重试，协议与文件校验错误保持失败。 */
bool isTransientNetworkError(QNetworkReply::NetworkError error)
{
    return error == QNetworkReply::TimeoutError || error == QNetworkReply::RemoteHostClosedError
        || error == QNetworkReply::TemporaryNetworkFailureError || error == QNetworkReply::NetworkSessionFailedError
        || error == QNetworkReply::ProxyConnectionClosedError || error == QNetworkReply::ProxyTimeoutError;
}

/** 使用平台合适的大小写规则比较本地路径。 */
bool samePath(const QString& first, const QString& second)
{
#ifdef Q_OS_WIN
    return first.compare(second, Qt::CaseInsensitive) == 0;
#else
    return first == second;
#endif
}

/** 验证普通目录实际可写，探测文件随函数退出自动移除。 */
QString writableDirectory(const QString& path, bool create)
{
    if (path.trimmed().isEmpty())
        return {};
    const QString absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (create && !QDir().mkpath(absolute))
        return {};
    const QFileInfo info(absolute);
    if (!info.isDir() || !info.isWritable())
        return {};
    QTemporaryFile probe(QDir(absolute).filePath(QStringLiteral(".desktoptool-write-XXXXXX")));
    if (!probe.open())
        return {};
    return info.canonicalFilePath();
}

/** 解析 HTTP 非负十进制字节数，不接受符号、溢出或小数。 */
bool parseByteCount(const QByteArray& raw, qint64& value)
{
    static const QRegularExpression digits(QStringLiteral("^[0-9]+$"));
    const QByteArray trimmed = raw.trimmed();
    if (!digits.match(QString::fromLatin1(trimmed)).hasMatch())
        return false;
    bool valid = false;
    value = trimmed.toLongLong(&valid);
    return valid && value >= 0;
}

/** 提取响应建议文件名，优先处理 UTF-8 扩展字段。 */
QString responseFileName(const QByteArray& disposition)
{
    const QString value = QString::fromLatin1(disposition);
    static const QRegularExpression extended(QStringLiteral("(?:^|;)\\s*filename\\*\\s*=\\s*(?:\")?UTF-8'[^']*'([^;\"]+)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression ordinary(QStringLiteral("(?:^|;)\\s*filename\\s*=\\s*(?:\"([^\"]*)\"|([^;]+))"), QRegularExpression::CaseInsensitiveOption);
    const auto encoded = extended.match(value);
    if (encoded.hasMatch())
        return QUrl::fromPercentEncoding(encoded.captured(1).trimmed().toLatin1());
    const auto plain = ordinary.match(value);
    if (plain.hasMatch())
        return plain.captured(1).isNull() ? plain.captured(2).trimmed() : plain.captured(1);
    return {};
}
}

DownloadService::DownloadService(const QString& dataRoot, QObject* parent)
    : QObject(parent)
    , m_dataRoot(QDir::cleanPath(QFileInfo(dataRoot).absoluteFilePath()))
    , m_indexPath(QDir(m_dataRoot).filePath(QStringLiteral("download-tasks.v1.json")))
    , m_notifyTimer(new QTimer(this))
    , m_checkpointTimer(new QTimer(this))
{
    m_notifyTimer->setSingleShot(true);
    m_notifyTimer->setInterval(200);
    m_checkpointTimer->setSingleShot(true);
    m_checkpointTimer->setInterval(1500);
    connect(m_notifyTimer, &QTimer::timeout, this, &DownloadService::changed);
    connect(m_checkpointTimer, &QTimer::timeout, this, [this] {
        QString error;
        if (!persist(&error)) {
            for (const auto& task : m_tasks)
                task->warning = QStringLiteral("下载状态暂未保存：") + error;
            emit changed();
        }
    });
    load();
}

DownloadService::~DownloadService()
{
    m_notifyTimer->stop();
    m_checkpointTimer->stop();
    for (const auto& task : m_tasks) {
        const bool active = task->status == QStringLiteral("downloading");
        stop(task);
        if (active)
            task->status = QStringLiteral("paused");
    }
    if (m_loadError.isEmpty() && (!m_tasks.isEmpty() || QFileInfo::exists(m_indexPath)))
        persist();
}

bool DownloadService::validUrl(const QUrl& url)
{
    const QString scheme = url.scheme().toLower();
    return url.isValid() && !url.host().isEmpty() && (scheme == QStringLiteral("http") || scheme == QStringLiteral("https"))
        && url.userInfo().isEmpty() && !url.toString().contains(QChar::Null);
}

QString DownloadService::safeFileName(const QString& value)
{
    QString name = value;
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    name = name.section(QLatin1Char('/'), -1).trimmed();
    name.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f\\x7f]")), QStringLiteral("_"));
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))
        name.chop(1);
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
        name = QStringLiteral("download");
    static const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)"), QRegularExpression::CaseInsensitiveOption);
    if (reserved.match(name).hasMatch())
        name.prepend(QLatin1Char('_'));
    if (name.startsWith(QStringLiteral(".desktoptool-"), Qt::CaseInsensitive))
        name.prepend(QLatin1Char('_'));
    if (name.size() > 180) {
        const QString suffix = QFileInfo(name).suffix().left(24);
        name = name.left(150) + (suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix);
    }
    return name;
}

QString DownloadService::partPath(const Task& task)
{
    return QDir(task.directory).filePath(QStringLiteral(".desktoptool-") + task.id + QStringLiteral(".part"));
}

QVariantMap DownloadService::taskData(const Task& task)
{
    return {{QStringLiteral("id"), task.id}, {QStringLiteral("url"), task.url.toString(QUrl::FullyEncoded)},
        {QStringLiteral("fileName"), task.fileName}, {QStringLiteral("directory"), task.directory},
        {QStringLiteral("filePath"), QDir(task.directory).filePath(task.fileName)},
        {QStringLiteral("status"), task.status}, {QStringLiteral("bytesReceived"), task.bytesReceived},
        {QStringLiteral("totalBytes"), task.totalBytes}, {QStringLiteral("speed"), task.speed},
        {QStringLiteral("error"), task.error}, {QStringLiteral("warning"), task.warning},
        {QStringLiteral("createdAt"), task.createdAt}};
}

QVariantMap DownloadService::snapshot() const
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    QVariantList items;
    int activeCount = 0;
    for (const QString& id : m_order) {
        const auto task = m_tasks.value(id);
        items.append(taskData(*task));
        if (task->status == QStringLiteral("downloading"))
            ++activeCount;
    }
    return ServiceResult::success(QVariantMap{{QStringLiteral("items"), items}, {QStringLiteral("activeCount"), activeCount}});
}

bool DownloadService::hasActiveTasks() const
{
    for (const auto& task : m_tasks) {
        if (task->status == QStringLiteral("downloading"))
            return true;
    }
    return false;
}

QString DownloadService::availableName(const QString& directory, const QString& suggested, const QString& exceptId) const
{
    const QString safe = safeFileName(suggested);
    const QString suffix = QFileInfo(safe).suffix();
    const QString stem = suffix.isEmpty() ? safe : safe.left(safe.size() - suffix.size() - 1);
    QString candidate = safe;
    for (int serial = 1; ; ++serial) {
        const QString path = QDir(directory).filePath(candidate);
        bool reserved = QFileInfo::exists(path) || QFileInfo(path).isSymbolicLink();
        for (const auto& task : m_tasks) {
            if (task->id != exceptId && task->status != QStringLiteral("cancelled")
                && samePath(QDir(task->directory).filePath(task->fileName), path)) {
                reserved = true;
                break;
            }
        }
        if (!reserved)
            return candidate;
        candidate = stem + QStringLiteral(" (%1)").arg(serial)
            + (suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix);
    }
}

QString DownloadService::chooseDirectory(const QString& requested, const QString& preferred, QString& warning) const
{
    QString result;
    if (!requested.trimmed().isEmpty()) {
        result = writableDirectory(requested.trimmed(), false);
        if (!result.isEmpty())
            return result;
        warning = QStringLiteral("指定目录不存在或不可写，已改用默认下载目录。");
    }
    if (!preferred.trimmed().isEmpty()) {
        result = writableDirectory(preferred.trimmed(), false);
        if (!result.isEmpty())
            return result;
        warning += QStringLiteral("默认目录不可用，已尝试系统下载目录。");
    }
    result = writableDirectory(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation), true);
    if (!result.isEmpty())
        return result;
    result = writableDirectory(QDir(m_dataRoot).filePath(QStringLiteral("downloads")), true);
    if (!result.isEmpty())
        warning += QStringLiteral("系统下载目录不可用，已使用应用数据目录下的 downloads。");
    return result;
}

bool DownloadService::safeIndexPath() const
{
    // 数据根由调用方提供，根目录和索引文件都不能是重解析链接。
    const QFileInfo root(m_dataRoot);
    const QFileInfo index(m_indexPath);
    return !root.isSymbolicLink() && !root.isJunction() && !index.isSymbolicLink() && !index.isJunction()
        && (!root.exists() || root.isDir()) && (!index.exists() || index.isFile());
}

void DownloadService::load()
{
    if (!safeIndexPath()) {
        m_loadError = QStringLiteral("下载索引路径不安全或不是普通文件，已禁止覆盖。");
        return;
    }
    if (!QFileInfo::exists(m_indexPath))
        return;
    QFile file(m_indexPath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaximumIndexBytes) {
        m_loadError = QStringLiteral("下载任务索引无法读取，已保护原文件。");
        return;
    }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    const auto object = document.object();
    if (parse.error != QJsonParseError::NoError || !document.isObject()
        || object.value(QStringLiteral("version")).toInt() != 1 || !object.value(QStringLiteral("items")).isArray()) {
        m_loadError = QStringLiteral("下载任务索引损坏或版本不支持，已禁止覆盖。");
        return;
    }
    static const QRegularExpression identifier(QStringLiteral("^[0-9a-f]{32}$"));
    const QSet<QString> statuses{QStringLiteral("downloading"), QStringLiteral("paused"), QStringLiteral("completed"), QStringLiteral("cancelled"), QStringLiteral("failed")};
    for (const auto& value : object.value(QStringLiteral("items")).toArray()) {
        const auto item = value.toObject();
        auto task = std::make_shared<Task>();
        task->id = item.value(QStringLiteral("id")).toString();
        task->url = QUrl(item.value(QStringLiteral("url")).toString(), QUrl::StrictMode);
        task->directory = item.value(QStringLiteral("directory")).toString();
        task->fileName = item.value(QStringLiteral("fileName")).toString();
        task->status = item.value(QStringLiteral("status")).toString();
        task->bytesReceived = item.value(QStringLiteral("bytesReceived")).toInteger(-1);
        task->totalBytes = item.value(QStringLiteral("totalBytes")).toInteger(-2);
        task->createdAt = item.value(QStringLiteral("createdAt")).toString();
        task->error = item.value(QStringLiteral("error")).toString();
        task->warning = item.value(QStringLiteral("warning")).toString();
        task->etag = item.value(QStringLiteral("etag")).toString().toLatin1();
        task->lastModified = item.value(QStringLiteral("lastModified")).toString().toLatin1();
        if (!value.isObject() || !identifier.match(task->id).hasMatch() || m_tasks.contains(task->id)
            || !validUrl(task->url) || !QDir::isAbsolutePath(task->directory)
            || QDir::cleanPath(task->directory) != task->directory || safeFileName(task->fileName) != task->fileName
            || !statuses.contains(task->status) || task->bytesReceived < 0 || task->totalBytes < -1
            || task->etag.size() > 1024 || task->lastModified.size() > 1024
            || task->etag.contains('\r') || task->etag.contains('\n')
            || task->lastModified.contains('\r') || task->lastModified.contains('\n')
            || (!task->etag.isEmpty() && (task->etag.size() < 2 || !task->etag.startsWith('"') || !task->etag.endsWith('"')))
            || (!task->lastModified.isEmpty() && !QDateTime::fromString(QString::fromLatin1(task->lastModified), Qt::RFC2822Date).isValid())) {
            m_loadError = QStringLiteral("下载索引包含无效任务或不安全路径，已禁止覆盖和文件操作。");
            m_tasks.clear();
            m_order.clear();
            return;
        }
        if (task->status == QStringLiteral("downloading"))
            task->status = QStringLiteral("paused");
        if (task->status == QStringLiteral("paused") || task->status == QStringLiteral("failed")) {
            const QFileInfo partial(partPath(*task));
            if (partial.isSymbolicLink() || partial.isJunction() || (partial.exists() && !partial.isFile())) {
                task->status = QStringLiteral("failed");
                task->error = QStringLiteral("部分文件不是安全的普通文件，无法自动继续。");
            } else {
                task->bytesReceived = partial.exists() ? partial.size() : 0;
            }
        }
        m_order.append(task->id);
        m_tasks.insert(task->id, task);
    }
}

bool DownloadService::persist(QString* error)
{
    if (!m_loadError.isEmpty() || !safeIndexPath() || !QDir().mkpath(m_dataRoot)) {
        if (error)
            *error = m_loadError.isEmpty() ? QStringLiteral("下载索引路径不可写或不安全。") : m_loadError;
        return false;
    }
    QJsonArray items;
    for (const QString& id : m_order) {
        const auto& task = *m_tasks.value(id);
        auto item = QJsonObject::fromVariantMap(taskData(task));
        item.remove(QStringLiteral("filePath"));
        item.remove(QStringLiteral("speed"));
        item.insert(QStringLiteral("etag"), QString::fromLatin1(task.etag));
        item.insert(QStringLiteral("lastModified"), QString::fromLatin1(task.lastModified));
        items.append(item);
    }
    const QByteArray bytes = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("items"), items}}).toJson();
    if (bytes.size() > MaximumIndexBytes) {
        if (error)
            *error = QStringLiteral("下载任务历史过大，请先移除不再需要的历史记录。");
        return false;
    }
    QSaveFile file(m_indexPath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error)
            *error = QStringLiteral("无法保存下载任务：") + file.errorString();
        return false;
    }
    return true;
}

void DownloadService::publishState()
{
    m_checkpointTimer->stop();
    m_notifyTimer->stop();
    QString error;
    if (!persist(&error)) {
        for (const auto& task : m_tasks)
            task->warning = QStringLiteral("下载状态暂未保存：") + error;
    }
    emit changed();
}

void DownloadService::publishProgress()
{
    if (!m_notifyTimer->isActive())
        m_notifyTimer->start();
    if (!m_checkpointTimer->isActive())
        m_checkpointTimer->start();
}

QVariantMap DownloadService::createTask(const QString& urlText, const QString& directory, const QString& defaultDirectory)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    QUrl url(urlText.trimmed(), QUrl::StrictMode);
    if (!validUrl(url))
        return ServiceResult::failure(QStringLiteral("请输入有效的 http:// 或 https:// 下载链接；链接不能包含用户名和密码。"));
    url.setFragment(QString());
    auto task = std::make_shared<Task>();
    task->id = QUuid::createUuid().toString(QUuid::Id128);
    task->url = url;
    task->directory = chooseDirectory(directory, defaultDirectory, task->warning);
    if (task->directory.isEmpty())
        return ServiceResult::failure(QStringLiteral("指定目录、默认目录和备用下载目录均不可写，请重新选择目录。"));
    task->fileName = availableName(task->directory, url.fileName(QUrl::FullyDecoded));
    task->createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    task->status = QStringLiteral("paused");
    m_tasks.insert(task->id, task);
    m_order.append(task->id);
    QString error;
    if (!persist(&error)) {
        m_tasks.remove(task->id);
        m_order.removeAll(task->id);
        return ServiceResult::failure(error);
    }
    if (!start(task, error)) {
        fail(task, error);
        return ServiceResult::failure(error);
    }
    return ServiceResult::success(taskData(*task));
}

bool DownloadService::start(const TaskPtr& task, QString& error)
{
    const QFileInfo directory(task->directory);
    const QFileInfo partial(partPath(*task));
    if (!directory.isDir() || !directory.isWritable() || partial.isSymbolicLink() || partial.isJunction()
        || (partial.exists() && !partial.isFile())) {
        error = QStringLiteral("下载目录不可写，或部分文件不是安全的普通文件。");
        return false;
    }
    task->file = std::make_unique<QFile>(partPath(*task));
    const auto mode = QIODevice::ReadWrite | (partial.exists() ? QIODevice::OpenMode() : QIODevice::NewOnly);
    if (!task->file->open(mode)) {
        error = QStringLiteral("无法打开下载部分文件：") + task->file->errorString();
        task->file.reset();
        return false;
    }
    task->bytesReceived = task->file->size();
    task->requestOffset = task->bytesReceived;
    if (task->requestOffset > 0 && task->etag.isEmpty() && task->lastModified.isEmpty()) {
        task->requestOffset = 0;
        task->warning = QStringLiteral("服务器未提供断点校验标识，为避免拼接不同版本文件，将重新下载。");
    }
    task->status = QStringLiteral("downloading");
    task->error.clear();
    task->speed = 0;
    task->speedStartBytes = task->bytesReceived;
    task->elapsed.start();
    task->redirects = 0;
    if (!persist(&error)) {
        stop(task);
        task->status = QStringLiteral("paused");
        return false;
    }
    // 每轮传输使用独立连接池；重试不会继续排队在已经超时的共享连接上。
    task->network = new QNetworkAccessManager(this);
    request(task, task->url);
    emit changed();
    return true;
}

void DownloadService::request(const TaskPtr& task, const QUrl& url)
{
    task->headersAccepted = false;
    task->responseBytes = 0;
    task->expectedBytes = -1;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setTransferTimeout(30000);
    request.setRawHeader("Accept-Encoding", "identity");
    const QString version = QCoreApplication::applicationVersion();
    request.setRawHeader("User-Agent", "DesktopTool/" + (version.isEmpty() ? QByteArray("unknown") : version.toUtf8()));
    if (task->requestOffset > 0) {
        request.setRawHeader("Range", "bytes=" + QByteArray::number(task->requestOffset) + '-');
        request.setRawHeader("If-Range", task->etag.isEmpty() ? task->lastModified : task->etag);
    }
    QNetworkReply* reply = task->network->get(request);
    reply->setReadBufferSize(TransferBufferSize * 2);
    task->reply = reply;
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, task, reply] {
        if (task->reply == reply)
            acceptHeaders(task);
    });
    connect(reply, &QIODevice::readyRead, this, [this, task, reply] {
        if (task->reply == reply)
            consume(task);
    });
    connect(reply, &QNetworkReply::finished, this, [this, task, reply] {
        if (task->reply == reply)
            finish(task);
    });
}

bool DownloadService::acceptHeaders(const TaskPtr& task)
{
    if (task->headersAccepted)
        return true;
    QNetworkReply* reply = task->reply;
    if (!reply)
        return false;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 0 || (status >= 300 && status < 400))
        return false;
    if (status == 416 && task->requestOffset > 0) {
        // 断点恰好位于文件末尾或服务器改变范围时，只重试一次完整下载。
        task->requestOffset = 0;
        task->warning = QStringLiteral("服务器拒绝当前断点，已改为完整下载；确认新响应前保留原部分文件。");
        task->reply.clear();
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
        request(task, task->url);
        return false;
    }
    if (status != 200 && status != 206) {
        fail(task, QStringLiteral("服务器返回 HTTP %1，下载未完成。").arg(status));
        return false;
    }
    const QByteArray encoding = reply->rawHeader("Content-Encoding").trimmed().toLower();
    if (!encoding.isEmpty() && encoding != "identity") {
        fail(task, QStringLiteral("服务器忽略了原始字节传输要求，返回了压缩编码；为防止断点文件损坏，已停止下载。"));
        return false;
    }
    qint64 contentLength = -1;
    if (reply->hasRawHeader("Content-Length") && !parseByteCount(reply->rawHeader("Content-Length"), contentLength)) {
        fail(task, QStringLiteral("服务器返回了无效的内容长度。"));
        return false;
    }
    qint64 total = contentLength;
    if (status == 206) {
        static const QRegularExpression rangeExpression(QStringLiteral("^bytes ([0-9]+)-([0-9]+)/([0-9]+|\\*)$"));
        const auto match = rangeExpression.match(QString::fromLatin1(reply->rawHeader("Content-Range").trimmed()));
        qint64 first = -1, last = -1;
        total = -1;
        const bool valid = match.hasMatch() && parseByteCount(match.captured(1).toLatin1(), first)
            && parseByteCount(match.captured(2).toLatin1(), last)
            && (match.captured(3) == QStringLiteral("*") || parseByteCount(match.captured(3).toLatin1(), total));
        if (!valid || first != task->requestOffset || last < first || last == std::numeric_limits<qint64>::max()
            || total < 0 || last >= total || (contentLength >= 0 && contentLength != last - first + 1)
            || (task->requestOffset > 0 && task->file->size() != task->requestOffset)) {
            fail(task, QStringLiteral("服务器返回的断点范围不匹配，已保留原部分文件并停止下载。"));
            return false;
        }
        const QByteArray etag = reply->rawHeader("ETag");
        const QByteArray modified = reply->rawHeader("Last-Modified");
        if (task->requestOffset > 0 && ((!task->etag.isEmpty() && !etag.isEmpty() && etag != task->etag)
            || (task->etag.isEmpty() && !task->lastModified.isEmpty() && !modified.isEmpty() && modified != task->lastModified))) {
            fail(task, QStringLiteral("服务器文件已改变，但仍返回了旧断点响应，已停止拼接。"));
            return false;
        }
        task->expectedBytes = last - first + 1;
        if (first == 0) {
            if (!task->etag.isEmpty() || !task->lastModified.isEmpty()) {
                task->etag.clear();
                task->lastModified.clear();
                QString error;
                if (!persist(&error)) {
                    fail(task, QStringLiteral("重新下载前无法保存校验状态：") + error);
                    return false;
                }
            }
            if (!task->file->resize(0)) {
                fail(task, QStringLiteral("无法重置下载部分文件。"));
                return false;
            }
            task->bytesReceived = 0;
            task->speedStartBytes = 0;
            task->elapsed.restart();
        }
        if (!task->file->seek(first)) {
            fail(task, QStringLiteral("无法定位下载部分文件。"));
            return false;
        }
    } else {
        if (task->requestOffset > 0)
            task->warning = QStringLiteral("服务器不支持当前断点或文件已更新，已安全地从头下载。");
        // 先持久化清除旧版本标识，避免崩溃后将新文件前缀与旧版本断点拼接。
        if (!task->etag.isEmpty() || !task->lastModified.isEmpty()) {
            task->etag.clear();
            task->lastModified.clear();
            QString error;
            if (!persist(&error)) {
                fail(task, QStringLiteral("重新下载前无法保存校验状态：") + error);
                return false;
            }
        }
        if (!task->file->resize(0) || !task->file->seek(0)) {
            fail(task, QStringLiteral("无法重置下载部分文件。"));
            return false;
        }
        task->bytesReceived = 0;
        task->speedStartBytes = 0;
        task->elapsed.restart();
        task->expectedBytes = contentLength;
    }
    task->totalBytes = total;
    const QByteArray etag = reply->rawHeader("ETag").trimmed();
    task->etag = etag.size() >= 2 && etag.startsWith('"') && etag.endsWith('"') && etag.size() <= 1024 ? etag : QByteArray();
    const QByteArray modified = reply->rawHeader("Last-Modified").trimmed();
    task->lastModified = modified.size() <= 1024 && QDateTime::fromString(QString::fromLatin1(modified), Qt::RFC2822Date).isValid()
        ? modified : QByteArray();
    const QString suggested = responseFileName(reply->rawHeader("Content-Disposition"));
    if (!suggested.isEmpty())
        task->fileName = availableName(task->directory, suggested, task->id);
    task->headersAccepted = true;
    publishProgress();
    return true;
}

void DownloadService::consume(const TaskPtr& task)
{
    if (!task->reply)
        return;
    const int status = task->reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 300 && status < 400) {
        while (task->reply && task->reply->bytesAvailable() > 0)
            task->reply->read(TransferBufferSize);
        return;
    }
    if (!acceptHeaders(task))
        return;
    while (task->reply && task->reply->bytesAvailable() > 0) {
        const QByteArray bytes = task->reply->read(TransferBufferSize);
        if (bytes.isEmpty())
            break;
        if (task->expectedBytes >= 0 && bytes.size() > task->expectedBytes - task->responseBytes) {
            fail(task, QStringLiteral("实际响应超过服务器声明的长度，已停止写入。"));
            return;
        }
        qint64 offset = 0;
        while (offset < bytes.size()) {
            const qint64 written = task->file->write(bytes.constData() + offset, bytes.size() - offset);
            if (written <= 0) {
                fail(task, QStringLiteral("写入下载文件失败：") + task->file->errorString());
                return;
            }
            offset += written;
            task->bytesReceived += written;
            task->responseBytes += written;
        }
        if (task->elapsed.elapsed() >= 250) {
            task->speed = (task->bytesReceived - task->speedStartBytes) * 1000 / qMax<qint64>(1, task->elapsed.elapsed());
            task->speedStartBytes = task->bytesReceived;
            task->elapsed.restart();
        }
    }
    publishProgress();
}

void DownloadService::finish(const TaskPtr& task)
{
    QNetworkReply* reply = task->reply;
    if (!reply)
        return;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 300 && status < 400) {
        const QUrl target = reply->url().resolved(reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());
        if (!reply->hasRawHeader("Location") || !validUrl(target)
            || (reply->url().scheme() == QStringLiteral("https") && target.scheme() != QStringLiteral("https"))
            || ++task->redirects > 10) {
            fail(task, QStringLiteral("重定向地址不安全、缺失或次数过多，已停止下载。"));
            return;
        }
        task->reply.clear();
        reply->disconnect(this);
        reply->deleteLater();
        request(task, target);
        return;
    }
    consume(task);
    if (task->reply != reply)
        return;
    if (reply->error() != QNetworkReply::NoError) {
        if (isTransientNetworkError(reply->error()) && scheduleRetry(task))
            return;
        const QString attempts = task->retryAttempts > 0
            ? QStringLiteral("（已自动重试 %1 次）").arg(task->retryAttempts) : QString();
        fail(task, QStringLiteral("网络下载失败") + attempts + QStringLiteral("：") + reply->errorString());
        return;
    }
    if (!task->headersAccepted || (task->expectedBytes >= 0 && task->responseBytes != task->expectedBytes)
        || (task->totalBytes >= 0 && task->bytesReceived != task->totalBytes)) {
        fail(task, QStringLiteral("下载数据不完整，已保留部分文件，可稍后继续。"));
        return;
    }
    if (!task->file->flush()) {
        fail(task, QStringLiteral("无法将下载内容刷新到磁盘：") + task->file->errorString());
        return;
    }
    stop(task);
    task->fileName = availableName(task->directory, task->fileName, task->id);
    QFile completed(partPath(*task));
    if (!completed.rename(QDir(task->directory).filePath(task->fileName))) {
        fail(task, QStringLiteral("下载已传输完，但无法生成最终文件：") + completed.errorString());
        return;
    }
    task->status = QStringLiteral("completed");
    task->totalBytes = task->bytesReceived;
    task->error.clear();
    publishState();
}

bool DownloadService::scheduleRetry(const TaskPtr& task)
{
    if (task->retryAttempts >= MaximumRetryAttempts)
        return false;
    // 关闭旧请求并刷新部分文件，后续统一经 start 校验目录和断点标识。
    stop(task);
    const int delaySeconds = 1 << task->retryAttempts;
    ++task->retryAttempts;
    task->warning = QStringLiteral("网络连接暂时中断，%1 秒后自动重试（%2/%3），已保留下载进度。")
        .arg(delaySeconds).arg(task->retryAttempts).arg(MaximumRetryAttempts);
    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    task->retryTimer = timer;
    connect(timer, &QTimer::timeout, this, [this, task, timer] {
        // 暂停、取消、移除和销毁均会清除此指针，旧回调不得重新启动任务。
        if (task->retryTimer != timer || task->status != QStringLiteral("downloading"))
            return;
        task->retryTimer.clear();
        timer->deleteLater();
        task->warning = QStringLiteral("网络中断后已自动重试 %1 次。").arg(task->retryAttempts);
        QString error;
        if (!start(task, error))
            fail(task, error);
    });
    timer->start(delaySeconds * 1000);
    publishState();
    return true;
}

void DownloadService::stop(const TaskPtr& task)
{
    if (task->retryTimer) {
        task->retryTimer->stop();
        task->retryTimer->deleteLater();
        task->retryTimer.clear();
    }
    QNetworkReply* reply = task->reply;
    task->reply.clear();
    if (reply) {
        reply->disconnect(this);
        if (!reply->isFinished())
            reply->abort();
        reply->deleteLater();
    }
    if (task->network) {
        task->network->deleteLater();
        task->network.clear();
    }
    if (task->file) {
        task->file->flush();
        task->file->close();
        task->file.reset();
    }
    task->speed = 0;
}

void DownloadService::fail(const TaskPtr& task, const QString& error)
{
    stop(task);
    task->status = QStringLiteral("failed");
    task->error = error;
    publishState();
}

QVariantMap DownloadService::pauseTask(const QString& id)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const auto task = m_tasks.value(id);
    if (!task || task->status != QStringLiteral("downloading"))
        return ServiceResult::failure(QStringLiteral("只有下载中的任务可以暂停。"));
    consume(task);
    if (task->status != QStringLiteral("downloading"))
        return ServiceResult::failure(task->error);
    if (task->file && !task->file->flush()) {
        fail(task, QStringLiteral("暂停时无法刷新已下载内容到磁盘。"));
        return ServiceResult::failure(task->error);
    }
    stop(task);
    task->status = QStringLiteral("paused");
    publishState();
    return ServiceResult::success(taskData(*task));
}

QVariantMap DownloadService::resumeTask(const QString& id)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const auto task = m_tasks.value(id);
    if (!task || (task->status != QStringLiteral("paused") && task->status != QStringLiteral("failed")))
        return ServiceResult::failure(QStringLiteral("只有暂停或失败的任务可以继续。"));
    task->retryAttempts = 0;
    QString error;
    if (!start(task, error)) {
        fail(task, error);
        return ServiceResult::failure(error);
    }
    return ServiceResult::success(taskData(*task));
}

bool DownloadService::removePartial(const TaskPtr& task, QString& error)
{
    const QString path = partPath(*task);
    const QFileInfo partial(path);
    if (partial.isSymbolicLink() || partial.isJunction() || (partial.exists() && !partial.isFile())) {
        error = QStringLiteral("部分文件不是安全的普通文件，已拒绝删除。");
        return false;
    }
    if (partial.exists() && !QFile::remove(path)) {
        error = QStringLiteral("无法清理该任务的部分文件，请检查目录权限或文件占用。");
        return false;
    }
    return true;
}

QVariantMap DownloadService::cancelTask(const QString& id)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const auto task = m_tasks.value(id);
    if (!task || task->status == QStringLiteral("completed"))
        return ServiceResult::failure(QStringLiteral("任务不存在或已经完成，无法取消。"));
    const QString previousStatus = task->status == QStringLiteral("downloading") ? QStringLiteral("paused") : task->status;
    const QString previousError = task->error;
    stop(task);
    QString error;
    task->status = QStringLiteral("cancelled");
    task->error.clear();
    if (!persist(&error)) {
        task->status = previousStatus;
        task->error = previousError;
        task->warning = error;
        emit changed();
        return ServiceResult::failure(error);
    }
    if (!removePartial(task, error)) {
        fail(task, error);
        return ServiceResult::failure(error);
    }
    emit changed();
    return ServiceResult::success(taskData(*task));
}

QVariantMap DownloadService::removeTask(const QString& id)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const auto task = m_tasks.value(id);
    if (!task || task->status == QStringLiteral("downloading") || task->status == QStringLiteral("paused"))
        return ServiceResult::failure(QStringLiteral("请先取消任务，再移除下载记录。"));
    QString error;
    const qsizetype position = m_order.indexOf(id);
    m_tasks.remove(id);
    m_order.removeAt(position);
    if (!persist(&error)) {
        m_tasks.insert(id, task);
        m_order.insert(position, id);
        return ServiceResult::failure(error);
    }
    if (task->status != QStringLiteral("completed") && !removePartial(task, error)) {
        m_tasks.insert(id, task);
        m_order.insert(position, id);
        QString rollbackError;
        if (!persist(&rollbackError))
            error += QStringLiteral("；恢复任务索引失败：") + rollbackError;
        emit changed();
        return ServiceResult::failure(error);
    }
    emit changed();
    return ServiceResult::success();
}

QVariantMap DownloadService::openDirectory(const QString& id)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const auto task = m_tasks.value(id);
    if (!task || !QFileInfo(task->directory).isDir())
        return ServiceResult::failure(QStringLiteral("任务目录不存在。"));
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(task->directory)))
        return ServiceResult::failure(QStringLiteral("无法打开下载目录。"));
    return ServiceResult::success(taskData(*task));
}
