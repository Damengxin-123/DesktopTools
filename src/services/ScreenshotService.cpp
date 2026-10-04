#include "ScreenshotService.h"
#include "ServiceResult.h"

#include <QBuffer>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
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
// 单张 PNG 与索引文件的大小上限。
constexpr qint64 MaximumFileBytes = 48 * 1024 * 1024;
constexpr qint64 MaximumIndexBytes = 32 * 1024 * 1024;
// 图像像素上限，与剪贴板服务的防护边界一致。
constexpr qint64 MaximumPixels = 20000000;
// 缩略图按等比缩放到该宽度内，随索引内联保存。
constexpr int ThumbnailWidth = 320;

const QString imagePrefix = QStringLiteral("data:image/png;base64,");

// 把图片编码为 PNG 数据地址；失败或超限时返回空。
QString imageDataUrl(const QImage& image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG") || bytes.size() > MaximumFileBytes)
        return {};
    return imagePrefix + QString::fromLatin1(bytes.toBase64());
}
}

ScreenshotService::ScreenshotService(const QString& dataRoot, QObject* parent)
    : QObject(parent)
    , m_dataRoot(QDir::cleanPath(QFileInfo(dataRoot).absoluteFilePath()))
    , m_root(QDir(m_dataRoot).filePath(QStringLiteral("screenshots")))
    , m_indexPath(QDir(m_root).filePath(QStringLiteral("index.v1.json")))
{
    load();
}

bool ScreenshotService::safePath(const QString& path) const
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

void ScreenshotService::load()
{
    if (!safePath(m_indexPath)) {
        m_loadError = QStringLiteral("截图数据路径包含不安全的目录链接，已停止读取和保存。");
        return;
    }
    if (!QFileInfo::exists(m_indexPath))
        return;
    QFile file(m_indexPath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaximumIndexBytes) {
        m_loadError = QStringLiteral("无法读取截图索引，已保护原文件，禁止覆盖。");
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    const QJsonObject root = document.object();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || root.value(QStringLiteral("version")).toInt() != 1
        || !root.value(QStringLiteral("items")).isArray()) {
        m_loadError = QStringLiteral("截图索引损坏或版本不支持，已保护原文件，禁止覆盖。");
        return;
    }
    QSet<QString> ids;
    for (const QJsonValue& value : root.value(QStringLiteral("items")).toArray()) {
        const QJsonObject object = value.toObject();
        Item item;
        item.id = object.value(QStringLiteral("id")).toString();
        item.capturedAt = object.value(QStringLiteral("capturedAt")).toString();
        item.file = object.value(QStringLiteral("file")).toString();
        item.bytes = object.value(QStringLiteral("bytes")).toInteger();
        item.width = object.value(QStringLiteral("width")).toInt();
        item.height = object.value(QStringLiteral("height")).toInt();
        item.thumbnail = object.value(QStringLiteral("thumbnail")).toString();
        if (!value.isObject() || ids.contains(item.id)
            || !QDateTime::fromString(item.capturedAt, Qt::ISODateWithMs).isValid()
            || item.file.isEmpty() || item.file.contains(QLatin1Char('/'))
            || item.file.contains(QLatin1Char('\\')) || item.file.contains(QLatin1Char(':'))
            || item.bytes < 0 || item.bytes > MaximumFileBytes
            || item.width <= 0 || item.height <= 0
            || !item.thumbnail.startsWith(imagePrefix)
            || item.thumbnail.size() > 1024 * 1024) {
            m_loadError = QStringLiteral("截图索引包含无效记录，已禁止覆盖。");
            return;
        }
        ids.insert(item.id);
        m_items.append(item);
    }
}

QVariantMap ScreenshotService::summary(const Item& item)
{
    return {{QStringLiteral("id"), item.id}, {QStringLiteral("capturedAt"), item.capturedAt},
        {QStringLiteral("bytes"), item.bytes}, {QStringLiteral("width"), item.width},
        {QStringLiteral("height"), item.height}, {QStringLiteral("thumbnail"), item.thumbnail}};
}

QVariantMap ScreenshotService::snapshot() const
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    QVariantList items;
    for (const Item& item : m_items)
        items.append(summary(item));
    return ServiceResult::success(QVariantMap{{QStringLiteral("items"), items},
        {QStringLiteral("total"), m_items.size()}});
}

int ScreenshotService::position(const QVector<Item>& items, const QString& id) const
{
    for (int index = 0; index < items.size(); ++index) {
        if (items.at(index).id == id)
            return index;
    }
    return -1;
}

QVariantMap ScreenshotService::commit(QVector<Item> items)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    if (!safePath(m_indexPath) || !QDir().mkpath(m_root))
        return ServiceResult::failure(QStringLiteral("截图索引路径不安全或目录无法创建。"));
    QJsonArray itemArray;
    for (const Item& item : items) {
        itemArray.append(QJsonObject{{QStringLiteral("id"), item.id},
            {QStringLiteral("capturedAt"), item.capturedAt}, {QStringLiteral("file"), item.file},
            {QStringLiteral("bytes"), item.bytes}, {QStringLiteral("width"), item.width},
            {QStringLiteral("height"), item.height}, {QStringLiteral("thumbnail"), item.thumbnail}});
    }
    const QByteArray bytes = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
        {QStringLiteral("items"), itemArray}}).toJson();
    QSaveFile file(m_indexPath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return ServiceResult::failure(QStringLiteral("无法保存截图索引：") + file.errorString());
    m_items = items;
    emit changed();
    return ServiceResult::success();
}

QVariantMap ScreenshotService::add(const QImage& image, const QString& capturedAtIso)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    if (image.isNull() || image.width() <= 0 || image.height() <= 0
        || qint64(image.width()) * image.height() > MaximumPixels)
        return ServiceResult::failure(QStringLiteral("截图图像无效或超过 2000 万像素，无法保存。"));
    if (!QDateTime::fromString(capturedAtIso, Qt::ISODateWithMs).isValid())
        return ServiceResult::failure(QStringLiteral("截图时间无效，无法保存。"));

    const QString id = QUuid::createUuid().toString(QUuid::Id128);
    const QString fileName = id + QStringLiteral(".png");
    const QString path = QDir(m_root).filePath(fileName);
    if (!safePath(path) || !QDir().mkpath(m_root))
        return ServiceResult::failure(QStringLiteral("截图保存路径不安全。"));
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || !image.save(&file, "PNG") || !file.commit())
        return ServiceResult::failure(QStringLiteral("无法保存截图文件：") + file.errorString());
    const qint64 bytes = QFileInfo(path).size();
    if (bytes > MaximumFileBytes) {
        QFile::remove(path);
        return ServiceResult::failure(QStringLiteral("截图文件超过 48 MB，无法保存到历史。"));
    }

    Item item;
    item.id = id;
    item.capturedAt = capturedAtIso;
    item.file = fileName;
    item.bytes = bytes;
    item.width = image.width();
    item.height = image.height();
    item.thumbnail = imageDataUrl(image.scaledToWidth(ThumbnailWidth, Qt::SmoothTransformation));
    if (item.thumbnail.isEmpty()) {
        QFile::remove(path);
        return ServiceResult::failure(QStringLiteral("缩略图编码失败，无法保存到历史。"));
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
    if (!committed.value(QStringLiteral("ok")).toBool()) {
        QFile::remove(path);
        return committed;
    }
    removeFiles(removed);
    return ServiceResult::success(summary(item));
}

void ScreenshotService::removeFiles(const QVector<Item>& items)
{
    // 个别文件删除失败只留下无害的孤立文件，不影响索引。
    for (const Item& item : items) {
        const QString path = QDir(m_root).filePath(item.file);
        if (safePath(path))
            QFile::remove(path);
    }
}

QVariantMap ScreenshotService::read(const QString& id) const
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const int index = position(m_items, id);
    if (index < 0)
        return ServiceResult::failure(QStringLiteral("该截图记录已不存在。"));
    const Item item = m_items.at(index);
    const QString path = QDir(m_root).filePath(item.file);
    if (!safePath(path))
        return ServiceResult::failure(QStringLiteral("截图文件路径不安全，已拒绝读取。"));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaximumFileBytes)
        return ServiceResult::failure(QStringLiteral("无法读取截图文件，文件可能缺失、过大或不可访问。"));
    QImage image;
    if (!image.load(&file, "PNG"))
        return ServiceResult::failure(QStringLiteral("截图文件损坏，无法读取。"));
    const QString encoded = imageDataUrl(image);
    if (encoded.isEmpty())
        return ServiceResult::failure(QStringLiteral("截图文件过大，无法预览。"));
    QVariantMap record = summary(item);
    record.insert(QStringLiteral("image"), encoded);
    record.insert(QStringLiteral("path"), QDir::toNativeSeparators(path));
    return ServiceResult::success(record);
}

QVariantMap ScreenshotService::remove(const QStringList& ids)
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

QVariantMap ScreenshotService::clear()
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

QVariantMap ScreenshotService::openDirectory(const QString& id) const
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const int index = position(m_items, id);
    if (index < 0)
        return ServiceResult::failure(QStringLiteral("该截图记录已不存在。"));
    const QString path = QDir(m_root).filePath(m_items.at(index).file);
    if (!safePath(path))
        return ServiceResult::failure(QStringLiteral("截图文件路径不安全，已拒绝打开。"));
    const QFileInfo file(path);
    if (!file.exists())
        return ServiceResult::failure(QStringLiteral("截图文件已不存在，仍可在记录中查看缩略图。"));
    // 资源管理器选中文件需要反斜杠路径。
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(file.absolutePath())))
        return ServiceResult::failure(QStringLiteral("无法打开所在目录。"));
    return ServiceResult::success();
}
