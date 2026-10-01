#include "EmojiService.h"
#include "ClipboardService.h"
#include "ServiceResult.h"

#include <QBuffer>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QUrl>
#include <QUuid>

#include <algorithm>

namespace
{
// 限制表情数量与索引体积；缩略图随索引一起保存，防止无限占用磁盘。
constexpr qsizetype MaximumItems = 2000;
constexpr qint64 MaximumIndexBytes = 128 * 1024 * 1024;
constexpr qint64 MaximumImagePixels = 20000000;
constexpr qint64 MaximumThumbnailBytes = 256 * 1024;
// 列表缩略图的最长边；表情多为小图，保持清晰即可。
constexpr int ThumbnailSize = 200;
// 关键字数量与长度上限，保持索引轻量和搜索可用。
constexpr int MaximumKeywords = 20;
constexpr int MaximumKeywordLength = 50;
// 缩略图固定为 Qt 编码的 PNG 数据地址，与网页 CSP 一致。
const QString ThumbnailPrefix = QStringLiteral("data:image/png;base64,");

// 生成新增分类或表情的稳定标识。
QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// 将图片编码为 PNG 数据地址，仅由 Qt 编码的图片进入网页。
QString imageDataUrl(const QImage& image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG") || bytes.size() > MaximumThumbnailBytes)
        return {};
    return ThumbnailPrefix + QString::fromLatin1(bytes.toBase64());
}

// 判断文件是否为图像；兼容没有常见图片扩展名的缓存文件。
bool isImageFile(const QString& path)
{
    const QFileInfo file(path);
    if (file.isDir())
        return false;
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    if (reader.canRead())
        return true;
    const auto suffix = file.suffix().toLower().toLatin1();
    return !suffix.isEmpty() && QImageReader::supportedImageFormats().contains(suffix);
}

// 在读取像素前检查图像头部尺寸，避免超大文件解码后占用过多内存。
QImage readBoundedImage(QImageReader& reader)
{
    const QSize size = reader.size();
    if (!size.isValid() || qint64(size.width()) * size.height() > MaximumImagePixels)
        return {};
    reader.setAutoTransform(true);
    return reader.read();
}

// 解析并校验关键字列表，失败时返回错误说明。
QString parseKeywords(const QVariant& value, QStringList* keywords)
{
    if (!QJsonValue::fromVariant(value).isArray())
        return QStringLiteral("表情关键字必须是列表。");
    for (const auto& entry : value.toList()) {
        if (!QJsonValue::fromVariant(entry).isString())
            return QStringLiteral("表情关键字必须是文本。");
        const QString keyword = entry.toString().trimmed();
        if (keyword.isEmpty())
            continue;
        if (keyword.size() > MaximumKeywordLength)
            return QStringLiteral("单个关键字不能超过 %1 个字符。").arg(MaximumKeywordLength);
        if (keywords->size() >= MaximumKeywords)
            return QStringLiteral("关键字最多 %1 个。").arg(MaximumKeywords);
        keywords->append(keyword);
    }
    keywords->removeDuplicates();
    return {};
}
}

EmojiService::EmojiService(const QString& dataRoot, ClipboardService* clipboard, QObject* parent)
    : QObject(parent), m_path(QDir(dataRoot).absoluteFilePath(QStringLiteral("emoji/emoji-library.v1.json"))),
      m_categories({{QStringLiteral("default"), QStringLiteral("默认分类")}}), m_clipboard(clipboard)
{
    load();
}

void EmojiService::load()
{
    if (!QFileInfo::exists(m_path))
        return;
    QFile file(m_path);
    m_loadError = QStringLiteral("表情库无法读取或格式损坏，已停止写入以保护原文件。请检查数据目录中的 emoji/emoji-library.v1.json。");
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaximumIndexBytes)
        return;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    const auto root = document.object();
    if (error.error != QJsonParseError::NoError || root.value("version").toInt() != 1
        || !root.value("items").isArray() || !root.value("categories").isArray()
        || root.value("items").toArray().size() > MaximumItems)
        return;
    QVector<Category> categories;
    for (const auto& value : root.value("categories").toArray()) {
        const auto category = value.toObject();
        if (!value.isObject() || !category.value("id").isString() || !category.value("name").isString())
            return;
        categories.append({category.value("id").toString(), category.value("name").toString()});
    }
    QVector<Emoji> items;
    for (const auto& value : root.value("items").toArray()) {
        const auto item = value.toObject();
        const auto keywordsValue = item.value("keywords");
        if (!value.isObject() || !item.value("id").isString() || !item.value("categoryId").isString()
            || !item.value("name").isString() || !item.value("path").isString()
            || !keywordsValue.isArray() || !item.value("addedAt").isString() || !item.value("thumbnail").isString())
            return;
        const auto width = item.value("width");
        const auto height = item.value("height");
        if (!width.isDouble() || !height.isDouble())
            return;
        Emoji entry;
        entry.id = item.value("id").toString();
        entry.categoryId = item.value("categoryId").toString();
        entry.name = item.value("name").toString();
        entry.path = item.value("path").toString();
        entry.addedAt = item.value("addedAt").toString();
        entry.width = width.toInt();
        entry.height = height.toInt();
        entry.thumbnail = item.value("thumbnail").toString();
        for (const auto& keyword : keywordsValue.toArray()) {
            if (!keyword.isString())
                return;
            entry.keywords.append(keyword.toString());
        }
        items.append(entry);
    }
    const QString validationError = validate(categories, items);
    if (!validationError.isEmpty())
        return;
    m_categories = categories;
    m_items = items;
    m_loadError.clear();
}

QString EmojiService::validate(const QVector<Category>& categories, const QVector<Emoji>& items) const
{
    QSet<QString> categoryIds;
    QSet<QString> categoryNames;
    for (const Category& category : categories) {
        if (category.id.trimmed().isEmpty() || category.name.trimmed().isEmpty())
            return QStringLiteral("分类标识和名称不能为空。");
        if (categoryIds.contains(category.id) || categoryNames.contains(category.name.toCaseFolded()))
            return QStringLiteral("分类标识或名称重复：%1").arg(category.name);
        if (category.id == QStringLiteral("default") && category.name != QStringLiteral("默认分类"))
            return QStringLiteral("默认分类名称必须为“默认分类”。");
        categoryIds.insert(category.id);
        categoryNames.insert(category.name.toCaseFolded());
    }
    if (!categoryIds.contains(QStringLiteral("default")))
        return QStringLiteral("缺少默认分类。");
    QSet<QString> ids;
    for (const Emoji& item : items) {
        if (item.id.trimmed().isEmpty() || item.categoryId.isEmpty() || item.name.isEmpty() || item.path.isEmpty())
            return QStringLiteral("表情标识、名称、路径和分类不能为空。");
        if (!categoryIds.contains(item.categoryId))
            return QStringLiteral("表情所属分类不存在：%1").arg(item.name);
        if (ids.contains(item.id))
            return QStringLiteral("表情标识重复。");
        if (item.keywords.size() > MaximumKeywords)
            return QStringLiteral("表情关键字数量超过上限：%1").arg(item.name);
        for (const QString& keyword : item.keywords) {
            if (keyword.isEmpty() || keyword.size() > MaximumKeywordLength)
                return QStringLiteral("表情关键字长度无效：%1").arg(item.name);
        }
        if (!item.thumbnail.startsWith(ThumbnailPrefix)
            || item.thumbnail.size() > MaximumThumbnailBytes * 4 / 3 + 100)
            return QStringLiteral("表情缩略图数据无效：%1").arg(item.name);
        if (item.width <= 0 || item.height <= 0 || qint64(item.width) * item.height > MaximumImagePixels)
            return QStringLiteral("表情尺寸信息无效：%1").arg(item.name);
        if (!QDateTime::fromString(item.addedAt, Qt::ISODateWithMs).isValid())
            return QStringLiteral("表情添加时间无效：%1").arg(item.name);
        ids.insert(item.id);
    }
    return {};
}

QVariantMap EmojiService::commit(const QVector<Category>& categories, const QVector<Emoji>& items)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const QString error = validate(categories, items);
    if (!error.isEmpty())
        return ServiceResult::failure(error);
    QJsonArray categoryArray;
    for (const Category& category : categories)
        categoryArray.append(QJsonObject{{"id", category.id}, {"name", category.name}});
    QJsonArray itemArray;
    for (const Emoji& item : items)
        itemArray.append(QJsonObject::fromVariantMap(emojiMap(item)));
    const QByteArray bytes = QJsonDocument(QJsonObject{{"version", 1}, {"categories", categoryArray},
        {"items", itemArray}}).toJson(QJsonDocument::Indented);
    if (bytes.size() > MaximumIndexBytes)
        return ServiceResult::failure(QStringLiteral("表情库容量已满，请删除部分表情后重试。"));
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath()))
        return ServiceResult::failure(QStringLiteral("无法创建表情数据目录。"));
    QSaveFile file(m_path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return ServiceResult::failure(QStringLiteral("无法保存表情库：") + file.errorString());
    m_categories = categories;
    m_items = items;
    emit changed();
    return ServiceResult::success();
}

QVariantMap EmojiService::emojiMap(const Emoji& item)
{
    return QVariantMap{{"id", item.id}, {"categoryId", item.categoryId}, {"name", item.name},
        {"path", item.path}, {"keywords", item.keywords}, {"addedAt", item.addedAt},
        {"width", item.width}, {"height", item.height}, {"thumbnail", item.thumbnail}};
}

QVariantMap EmojiService::data(const QString& query) const
{
    QVariantList categoryList;
    for (const Category& category : m_categories)
        categoryList.append(QVariantMap{{"id", category.id}, {"name", category.name}});
    const QString needle = query.trimmed();
    QVariantList items;
    for (const Emoji& item : m_items) {
        if (!needle.isEmpty() && !item.name.contains(needle, Qt::CaseInsensitive)
            && !std::any_of(item.keywords.cbegin(), item.keywords.cend(),
                   [&needle](const QString& keyword) { return keyword.contains(needle, Qt::CaseInsensitive); }))
            continue;
        items.append(emojiMap(item));
    }
    return QVariantMap{{"categories", categoryList}, {"items", items}, {"total", m_items.size()}};
}

QVariantMap EmojiService::snapshot(const QString& query) const
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    return ServiceResult::success(data(query));
}

qsizetype EmojiService::position(const QString& id) const
{
    for (qsizetype index = 0; index < m_items.size(); ++index) {
        if (m_items.at(index).id == id)
            return index;
    }
    return -1;
}

QString EmojiService::buildThumbnail(const QString& path, int* width, int* height)
{
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    const QImage image = readBoundedImage(reader);
    if (image.isNull())
        return {};
    const QString encoded = imageDataUrl(image.scaled(ThumbnailSize, ThumbnailSize,
        Qt::KeepAspectRatio, Qt::SmoothTransformation));
    if (encoded.isEmpty())
        return {};
    *width = image.width();
    *height = image.height();
    return encoded;
}

QVariantMap EmojiService::add(const QVariantMap& item)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    for (const QString& key : {QStringLiteral("path"), QStringLiteral("categoryId")}) {
        if (!QJsonValue::fromVariant(item.value(key)).isString())
            return ServiceResult::failure(QStringLiteral("表情字段格式错误：") + key);
    }
    const QString rawPath = item.value("path").toString().trimmed();
    const QFileInfo file(QDir::cleanPath(rawPath));
    if (rawPath.isEmpty() || !file.isFile() || !file.exists())
        return ServiceResult::failure(QStringLiteral("表情文件不存在：") + QDir::toNativeSeparators(rawPath));
    if (!isImageFile(file.absoluteFilePath()))
        return ServiceResult::failure(QStringLiteral("所选文件不是支持的图片格式。"));
    const QString categoryId = item.value("categoryId").toString();
    if (std::none_of(m_categories.cbegin(), m_categories.cend(),
            [&categoryId](const Category& category) { return category.id == categoryId; }))
        return ServiceResult::failure(QStringLiteral("目标分类不存在。"));
    QStringList keywords;
    const QString keywordError = parseKeywords(item.value(QStringLiteral("keywords")), &keywords);
    if (!keywordError.isEmpty())
        return ServiceResult::failure(keywordError);
    // 同一分类内不重复添加同一文件；跨分类允许同一表情复用。
    const QString path = QDir::toNativeSeparators(file.absoluteFilePath());
    for (const Emoji& existing : m_items) {
        if (existing.categoryId == categoryId && existing.path.compare(path, Qt::CaseInsensitive) == 0)
            return ServiceResult::failure(QStringLiteral("该分类已存在相同的表情文件。"));
    }
    int width = 0;
    int height = 0;
    const QString thumbnail = buildThumbnail(file.absoluteFilePath(), &width, &height);
    if (thumbnail.isEmpty())
        return ServiceResult::failure(QStringLiteral("无法读取表情图片，或图片超过 2000 万像素。"));
    if (m_items.size() >= MaximumItems)
        return ServiceResult::failure(QStringLiteral("表情数量已达上限，请删除部分表情后重试。"));
    Emoji value{newId(), categoryId, file.fileName(), path, keywords,
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs), width, height, thumbnail};
    QVector<Emoji> items = m_items;
    items.prepend(value);
    const auto result = commit(m_categories, items);
    return result.value("ok").toBool() ? ServiceResult::success(emojiMap(value)) : result;
}

QVariantMap EmojiService::save(const QVariantMap& item)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    for (const QString& key : {QStringLiteral("id"), QStringLiteral("categoryId")}) {
        if (!QJsonValue::fromVariant(item.value(key)).isString())
            return ServiceResult::failure(QStringLiteral("表情字段格式错误：") + key);
    }
    const qsizetype index = position(item.value("id").toString());
    if (index < 0)
        return ServiceResult::failure(QStringLiteral("要修改的表情不存在。"));
    const QString categoryId = item.value("categoryId").toString();
    if (std::none_of(m_categories.cbegin(), m_categories.cend(),
            [&categoryId](const Category& category) { return category.id == categoryId; }))
        return ServiceResult::failure(QStringLiteral("目标分类不存在。"));
    QStringList keywords;
    const QString keywordError = parseKeywords(item.value(QStringLiteral("keywords")), &keywords);
    if (!keywordError.isEmpty())
        return ServiceResult::failure(keywordError);
    QVector<Emoji> items = m_items;
    items[index].categoryId = categoryId;
    items[index].keywords = keywords;
    const auto result = commit(m_categories, items);
    return result.value("ok").toBool() ? ServiceResult::success(emojiMap(items.at(index))) : result;
}

QVariantMap EmojiService::remove(const QStringList& ids)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    QSet<QString> requested;
    for (const QString& id : ids) {
        if (position(id) < 0)
            return ServiceResult::failure(QStringLiteral("要删除的表情不存在，未删除任何条目。"));
        requested.insert(id);
    }
    QVector<Emoji> items;
    for (const Emoji& item : m_items) {
        if (!requested.contains(item.id))
            items.append(item);
    }
    return commit(m_categories, items);
}

QVariantMap EmojiService::saveCategory(const QString& id, const QString& name)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    QVector<Category> categories = m_categories;
    const QString cleanName = name.trimmed();
    QString savedId = id;
    if (id.isEmpty()) {
        savedId = newId();
        categories.append({savedId, cleanName});
    } else {
        auto category = std::find_if(categories.begin(), categories.end(),
            [&id](const Category& value) { return value.id == id; });
        if (category == categories.end())
            return ServiceResult::failure(QStringLiteral("要修改的分类不存在。"));
        category->name = cleanName;
    }
    const auto result = commit(categories, m_items);
    return result.value("ok").toBool()
        ? ServiceResult::success(QVariantMap{{"id", savedId}, {"name", cleanName}}) : result;
}

QVariantMap EmojiService::removeCategory(const QString& id)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    if (id == QStringLiteral("default"))
        return ServiceResult::failure(QStringLiteral("默认分类不能删除。"));
    auto category = std::find_if(m_categories.begin(), m_categories.end(),
        [&id](const Category& value) { return value.id == id; });
    if (category == m_categories.end())
        return ServiceResult::failure(QStringLiteral("要删除的分类不存在。"));
    QVector<Category> categories = m_categories;
    categories.erase(category);
    QVector<Emoji> items = m_items;
    for (Emoji& item : items) {
        if (item.categoryId == id)
            item.categoryId = QStringLiteral("default");
    }
    return commit(categories, items);
}

QVariantMap EmojiService::openDirectory(const QString& id)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const qsizetype index = position(id);
    if (index < 0)
        return ServiceResult::failure(QStringLiteral("该表情已不存在。"));
    const QFileInfo file(m_items.at(index).path);
    if (!file.exists())
        return ServiceResult::failure(QStringLiteral("原文件已不存在，仍可在卡片上查看原路径。"));
#ifdef Q_OS_WIN
    // 资源管理器 /select 参数打开目录并选中该文件。
    if (!QProcess::startDetached(QStringLiteral("explorer.exe"),
            {QStringLiteral("/select,") + QDir::toNativeSeparators(file.absoluteFilePath())}))
        return ServiceResult::failure(QStringLiteral("无法打开文件所在目录。"));
#else
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(file.absolutePath())))
        return ServiceResult::failure(QStringLiteral("无法打开文件所在目录。"));
#endif
    return ServiceResult::success();
}

QVariantMap EmojiService::copyFile(const QString& id)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const qsizetype index = position(id);
    if (index < 0)
        return ServiceResult::failure(QStringLiteral("该表情已不存在。"));
    if (!m_clipboard)
        return ServiceResult::failure(QStringLiteral("剪贴板功能不可用，无法复制文件。"));
    return m_clipboard->copyExternalFile(m_items.at(index).path);
}
