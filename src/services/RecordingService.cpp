#include "RecordingService.h"
#include "ServiceResult.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QUrl>
#include <QUuid>

namespace {
// 历史条数上限；达到后自动移除最早的记录及其文件。
constexpr int MaximumItems = 100;
// 索引文件大小上限；视频本体永不写入索引。
constexpr qint64 MaximumIndexBytes = 32 * 1024 * 1024;
// 单个视频文件的大小上限，超过视为异常拒绝登记。
constexpr qint64 MaximumVideoBytes = 64LL * 1024 * 1024 * 1024;
// 缩略图数据地址上限，与截图索引保持一致。
constexpr qint64 MaximumThumbnailChars = 1024 * 1024;

const QString imagePrefix = QStringLiteral("data:image/png;base64,");
}

RecordingService::RecordingService(const QString& dataRoot, QObject* parent)
    : QObject(parent)
    , m_dataRoot(QDir::cleanPath(QFileInfo(dataRoot).absoluteFilePath()))
    , m_root(QDir(m_dataRoot).filePath(QStringLiteral("recordings")))
    , m_indexPath(QDir(m_root).filePath(QStringLiteral("index.v1.json")))
{
    load();
}

bool RecordingService::safePath(const QString& path) const
{
    const QString absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    if (absolute.compare(m_dataRoot, sensitivity) != 0
        && !absolute.startsWith(m_dataRoot + QLatin1Char('/'), sensitivity))
        return false;
    // 逐段检查已有路径，不允许符号链接或 Windows 目录联接绕过根目录。
    const QString relative = QDir(m_dataRoot).relativeFilePath(absolute);
    const QString canonicalRoot = QFileInfo(m_dataRoot).canonicalFilePath();
    QString current = m_dataRoot;
    for (const QString& component : relative.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (component == QStringLiteral("."))
            continue;
        if (component.isEmpty() || component == QStringLiteral("..")
            || component.contains(QLatin1Char(':')) || component.endsWith(QLatin1Char('.')))
            return false;
        current = QDir(current).filePath(component);
        const QFileInfo info(current);
        if (info.isSymbolicLink() || info.isJunction())
            return false;
        if (info.exists() && !canonicalRoot.isEmpty()
            && !info.canonicalFilePath().startsWith(canonicalRoot, sensitivity))
            return false;
    }
    return true;
}

void RecordingService::load()
{
    if (!safePath(m_indexPath)) {
        m_loadError = QStringLiteral("录屏数据路径包含不安全的目录链接，已停止读取和保存。");
        return;
    }
    if (!QFileInfo::exists(m_indexPath))
        return;
    QFile file(m_indexPath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaximumIndexBytes) {
        m_loadError = QStringLiteral("无法读取录屏索引，已保护原文件，禁止覆盖。");
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    const QJsonObject root = document.object();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || root.value(QStringLiteral("version")).toInt() != 1
        || !root.value(QStringLiteral("items")).isArray()) {
        m_loadError = QStringLiteral("录屏索引损坏或版本不支持，已保护原文件，禁止覆盖。");
        return;
    }
    QSet<QString> ids;
    for (const QJsonValue& value : root.value(QStringLiteral("items")).toArray()) {
        const QJsonObject object = value.toObject();
        Item item;
        item.id = object.value(QStringLiteral("id")).toString();
        item.recordedAt = object.value(QStringLiteral("recordedAt")).toString();
        item.file = object.value(QStringLiteral("file")).toString();
        item.bytes = object.value(QStringLiteral("bytes")).toInteger();
        item.width = object.value(QStringLiteral("width")).toInt();
        item.height = object.value(QStringLiteral("height")).toInt();
        item.durationMs = object.value(QStringLiteral("durationMs")).toInteger();
        item.thumbnail = object.value(QStringLiteral("thumbnail")).toString();
        if (ids.contains(item.id)
            || !QDateTime::fromString(item.recordedAt, Qt::ISODateWithMs).isValid()
            || item.file.isEmpty() || item.file.contains(QLatin1Char('/'))
            || item.file.contains(QLatin1Char('\\')) || item.file.contains(QLatin1Char(':'))
            || !item.file.endsWith(QStringLiteral(".mp4"), Qt::CaseInsensitive)
            || item.bytes <= 0 || item.bytes > MaximumVideoBytes
            || item.width <= 0 || item.height <= 0 || item.durationMs < 0
            || (!item.thumbnail.isEmpty()
                && (!item.thumbnail.startsWith(imagePrefix)
                    || item.thumbnail.size() > MaximumThumbnailChars))) {
            m_loadError = QStringLiteral("录屏索引包含无效记录，已禁止覆盖。");
            return;
        }
        ids.insert(item.id);
        m_items.append(item);
    }
}

QVariantMap RecordingService::summary(const Item& item)
{
    return {{QStringLiteral("id"), item.id}, {QStringLiteral("recordedAt"), item.recordedAt},
        {QStringLiteral("bytes"), item.bytes}, {QStringLiteral("width"), item.width},
        {QStringLiteral("height"), item.height}, {QStringLiteral("durationMs"), item.durationMs},
        {QStringLiteral("thumbnail"), item.thumbnail}};
}

QVariantMap RecordingService::snapshot() const
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    QVariantList items;
    for (const Item& item : m_items)
        items.append(summary(item));
    return ServiceResult::success(QVariantMap{{QStringLiteral("items"), items},
        {QStringLiteral("total"), m_items.size()}});
}

int RecordingService::position(const QVector<Item>& items, const QString& id) const
{
    for (int index = 0; index < items.size(); ++index) {
        if (items.at(index).id == id)
            return index;
    }
    return -1;
}

QVariantMap RecordingService::commit(QVector<Item> items)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    if (!safePath(m_indexPath) || !QDir().mkpath(m_root))
        return ServiceResult::failure(QStringLiteral("录屏索引路径不安全或目录无法创建。"));
    QJsonArray itemArray;
    for (const Item& item : items) {
        itemArray.append(QJsonObject{{QStringLiteral("id"), item.id},
            {QStringLiteral("recordedAt"), item.recordedAt}, {QStringLiteral("file"), item.file},
            {QStringLiteral("bytes"), item.bytes}, {QStringLiteral("width"), item.width},
            {QStringLiteral("height"), item.height},
            {QStringLiteral("durationMs"), item.durationMs},
            {QStringLiteral("thumbnail"), item.thumbnail}});
    }
    const QByteArray bytes = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
        {QStringLiteral("items"), itemArray}}).toJson();
    QSaveFile file(m_indexPath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return ServiceResult::failure(QStringLiteral("无法保存录屏索引：") + file.errorString());
    m_items = items;
    emit changed();
    return ServiceResult::success();
}

QVariantMap RecordingService::reserve()
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    if (!safePath(m_root) || !QDir().mkpath(m_root))
        return ServiceResult::failure(QStringLiteral("录屏保存路径不安全或目录无法创建。"));
    const QString id = QUuid::createUuid().toString(QUuid::Id128);
    const QString path = QDir(m_root).filePath(id + QStringLiteral(".mp4"));
    if (!safePath(path))
        return ServiceResult::failure(QStringLiteral("录屏保存路径不安全。"));
    return ServiceResult::success(QVariantMap{{QStringLiteral("id"), id},
        {QStringLiteral("path"), QDir::toNativeSeparators(path)}});
}

QVariantMap RecordingService::add(const QString& path, const QVariantMap& meta)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    // 只接受本服务预约的路径：位于数据目录中、文件名是标识加 .mp4。
    const QString absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const QFileInfo info(absolute);
    if (!safePath(absolute) || info.absolutePath() != QDir(m_root).absolutePath()
        || !info.fileName().endsWith(QStringLiteral(".mp4"), Qt::CaseInsensitive)) {
        return ServiceResult::failure(QStringLiteral("录屏文件路径无效，无法登记。"));
    }
    const QString id = info.completeBaseName();
    if (!QFileInfo::exists(absolute))
        return ServiceResult::failure(QStringLiteral("录屏文件不存在，无法登记。"));
    const qint64 bytes = info.size();
    if (bytes <= 0 || bytes > MaximumVideoBytes)
        return ServiceResult::failure(QStringLiteral("录屏文件为空或超过大小上限，无法登记。"));
    const QString recordedAt = meta.value(QStringLiteral("recordedAt")).toString();
    if (!QDateTime::fromString(recordedAt, Qt::ISODateWithMs).isValid())
        return ServiceResult::failure(QStringLiteral("录制时间无效，无法登记。"));
    Item item;
    item.id = id;
    item.recordedAt = recordedAt;
    item.file = info.fileName();
    item.bytes = bytes;
    item.width = meta.value(QStringLiteral("width")).toInt();
    item.height = meta.value(QStringLiteral("height")).toInt();
    item.durationMs = meta.value(QStringLiteral("durationMs")).toLongLong();
    if (item.width <= 0 || item.height <= 0 || item.durationMs < 0)
        return ServiceResult::failure(QStringLiteral("录屏元数据无效，无法登记。"));
    item.thumbnail = meta.value(QStringLiteral("thumbnail")).toString();
    if ((!item.thumbnail.isEmpty()
            && (!item.thumbnail.startsWith(imagePrefix)
                || item.thumbnail.size() > MaximumThumbnailChars))) {
        return ServiceResult::failure(QStringLiteral("录屏缩略图无效，无法登记。"));
    }

    QVector<Item> items;
    items.append(item);
    QVector<Item> removed;
    for (const Item& previous : m_items) {
        if (items.size() < MaximumItems)
            items.append(previous);
        else
            removed.append(previous);
    }
    const QVariantMap committed = commit(items);
    if (!committed.value(QStringLiteral("ok")).toBool())
        return committed;
    removeFiles(removed);
    return ServiceResult::success(summary(item));
}

void RecordingService::removeFiles(const QVector<Item>& items)
{
    // 个别文件删除失败只留下无害的孤立文件，不影响索引。
    for (const Item& item : items) {
        const QString path = QDir(m_root).filePath(item.file);
        if (safePath(path))
            QFile::remove(path);
    }
}

QVariantMap RecordingService::read(const QString& id) const
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const int index = position(m_items, id);
    if (index < 0)
        return ServiceResult::failure(QStringLiteral("该录屏记录已不存在。"));
    const Item item = m_items.at(index);
    const QString path = QDir(m_root).filePath(item.file);
    if (!safePath(path))
        return ServiceResult::failure(QStringLiteral("录屏文件路径不安全，已拒绝读取。"));
    QVariantMap record = summary(item);
    record.insert(QStringLiteral("path"), QDir::toNativeSeparators(path));
    return ServiceResult::success(record);
}

QVariantMap RecordingService::remove(const QStringList& ids)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const QSet<QString> selected(ids.begin(), ids.end());
    QVector<Item> items;
    QVector<Item> removed;
    for (const Item& item : m_items) {
        if (selected.contains(item.id))
            removed.append(item);
        else
            items.append(item);
    }
    if (removed.isEmpty())
        return ServiceResult::success();
    const QVariantMap committed = commit(items);
    if (!committed.value(QStringLiteral("ok")).toBool())
        return committed;
    removeFiles(removed);
    return ServiceResult::success();
}

QVariantMap RecordingService::clear()
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const QVector<Item> removed = m_items;
    const QVariantMap committed = commit({});
    if (!committed.value(QStringLiteral("ok")).toBool())
        return committed;
    removeFiles(removed);
    return ServiceResult::success();
}

QVariantMap RecordingService::openDirectory(const QString& id) const
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const int index = position(m_items, id);
    if (index < 0)
        return ServiceResult::failure(QStringLiteral("该录屏记录已不存在。"));
    const QString path = QDir(m_root).filePath(m_items.at(index).file);
    if (!safePath(path))
        return ServiceResult::failure(QStringLiteral("录屏文件路径不安全，已拒绝打开。"));
    const QFileInfo file(path);
    if (!file.exists())
        return ServiceResult::failure(QStringLiteral("录屏文件已不存在，仍可在记录中查看缩略图。"));
    // 资源管理器选中文件需要反斜杠路径。
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(file.absolutePath())))
        return ServiceResult::failure(QStringLiteral("无法打开所在目录。"));
    return ServiceResult::success();
}
