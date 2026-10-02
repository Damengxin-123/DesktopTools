#include "GridMapService.h"
#include "ServiceResult.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <cmath>

namespace {
// 单个项目数据文件允许的最大字节数。
constexpr qint64 MaximumMapBytes = 32 * 1024 * 1024;
// 单个项目允许保存的最大格子数量。
constexpr int MaximumCells = 200000;
// 项目数量上限，避免索引无限膨胀。
constexpr int MaximumProjects = 500;
// 坐标的取值范围，画布中心为原点，向四周延伸。
constexpr qint64 MaximumCoordinate = 1000000;
// 十六进制颜色（#RGB 或 #RRGGBB）。
const QRegularExpression colorPattern(QStringLiteral("^#(?:[0-9a-f]{3}|[0-9a-f]{6})$"), QRegularExpression::CaseInsensitiveOption);
// 项目标识使用 32 位十六进制字符。
const QRegularExpression stableId(QStringLiteral("^[0-9a-f]{32}$"));

// 将 #RGB 颜色展开为小写 #RRGGBB，便于网页直接使用与比较。
QString normalizeColor(const QString& value)
{
    const QString trimmed = value.trimmed().toLower();
    if (!colorPattern.match(trimmed).hasMatch())
        return QString();
    if (trimmed.size() == 4) {
        QString expanded = QStringLiteral("#");
        for (int index = 1; index < 4; ++index)
            expanded.append(trimmed.at(index)).append(trimmed.at(index));
        return expanded;
    }
    return trimmed;
}

// 把 QVariant 值转换为整数；小数、溢出或超出范围时返回失败。
bool toCoordinate(const QVariant& value, qint64* coordinate)
{
    bool valid = false;
    const double number = value.toDouble(&valid);
    if (!valid || !qFuzzyIsNull(number - std::round(number))
        || std::abs(number) > MaximumCoordinate)
        return false;
    *coordinate = static_cast<qint64>(std::round(number));
    return true;
}
}

GridMapService::GridMapService(const QString& dataRoot, QObject* parent)
    : QObject(parent)
    , m_dataRoot(QDir::cleanPath(QFileInfo(dataRoot).absoluteFilePath()))
    , m_root(QDir(m_dataRoot).filePath(QStringLiteral("gridmap")))
    , m_indexPath(QDir(m_root).filePath(QStringLiteral("index.v1.json")))
{
    load();
}

bool GridMapService::safeSegment(const QString& name)
{
    return !name.isEmpty() && name != QStringLiteral(".") && name != QStringLiteral("..")
        && !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\'))
        && !name.contains(QLatin1Char(':')) && !name.contains(QChar::Null)
        && !name.endsWith(QLatin1Char('.')) && !name.endsWith(QLatin1Char(' '));
}

bool GridMapService::safePath(const QString& path) const
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
    QString current = m_dataRoot;
    const QString relative = QDir(m_dataRoot).relativeFilePath(absolute);
    const QString canonicalRoot = QFileInfo(m_dataRoot).canonicalFilePath();
    for (const QString& component : relative.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (component == QStringLiteral("."))
            continue;
        if (!safeSegment(component))
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

QString GridMapService::newId()
{
    return QUuid::createUuid().toString(QUuid::Id128);
}

void GridMapService::load()
{
    if (!safePath(m_root) || !safePath(m_indexPath)) {
        m_loadError = QStringLiteral("网格图数据路径包含不安全的目录链接，已停止读取和保存。");
        return;
    }
    if (!QFileInfo::exists(m_indexPath))
        return;
    QFile file(m_indexPath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaximumMapBytes) {
        m_loadError = QStringLiteral("无法读取网格图索引，已保护原文件，禁止覆盖。");
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    const QJsonObject root = document.object();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || root.value(QStringLiteral("version")).toInt() != 1
        || !root.value(QStringLiteral("items")).isArray()) {
        m_loadError = QStringLiteral("网格图索引损坏或版本不支持，已保护原文件，禁止覆盖。");
        return;
    }
    QSet<QString> ids;
    for (const QJsonValue& value : root.value(QStringLiteral("items")).toArray()) {
        const QJsonObject object = value.toObject();
        Item item{object.value(QStringLiteral("id")).toString(),
                  object.value(QStringLiteral("title")).toString(),
                  object.value(QStringLiteral("updatedAt")).toString()};
        if (!value.isObject() || !stableId.match(item.id).hasMatch() || ids.contains(item.id)
            || item.title.trimmed().isEmpty()) {
            m_loadError = QStringLiteral("网格图索引包含无效记录，已禁止覆盖。");
            return;
        }
        ids.insert(item.id);
        m_items.append(item);
    }
}

QVariantMap GridMapService::summary(const Item& item)
{
    return {{QStringLiteral("id"), item.id}, {QStringLiteral("title"), item.title},
            {QStringLiteral("updatedAt"), item.updatedAt}};
}

QVariantMap GridMapService::snapshot() const
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    QVariantList items;
    for (const Item& item : m_items)
        items.append(summary(item));
    return ServiceResult::success(QVariantMap{{QStringLiteral("items"), items},
        {QStringLiteral("warnings"), m_warnings}});
}

int GridMapService::position(const QString& id) const
{
    for (int index = 0; index < m_items.size(); ++index) {
        if (m_items.at(index).id == id)
            return index;
    }
    return -1;
}

QString GridMapService::mapPath(const QString& id) const
{
    return QDir(m_root).filePath(QStringLiteral("maps") + QLatin1Char('/') + id + QStringLiteral(".json"));
}

bool GridMapService::prepareIndex(QSaveFile& file, const QVector<Item>& items, QString& error) const
{
    if (!m_loadError.isEmpty()) {
        error = m_loadError;
        return false;
    }
    if (!safePath(m_indexPath) || !QDir().mkpath(m_root)) {
        error = QStringLiteral("网格图索引路径不安全或目录无法创建。");
        return false;
    }
    QJsonArray itemArray;
    for (const Item& item : items)
        itemArray.append(QJsonObject::fromVariantMap(summary(item)));
    const QByteArray bytes = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
        {QStringLiteral("items"), itemArray}}).toJson();
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        error = QStringLiteral("无法保存网格图索引：") + file.errorString();
        return false;
    }
    return true;
}

QVariantMap GridMapService::commitItems(const QVector<Item>& items)
{
    QString error;
    QSaveFile index(m_indexPath);
    if (!prepareIndex(index, items, error))
        return ServiceResult::failure(error);
    if (!index.commit())
        return ServiceResult::failure(QStringLiteral("无法提交网格图索引：") + index.errorString());
    m_items = items;
    emit changed();
    return ServiceResult::success();
}

QVariantMap GridMapService::create(const QString& title)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const QString trimmed = title.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > 200)
        return ServiceResult::failure(QStringLiteral("网格图名称不能为空，且不能超过 200 个字符。"));
    if (m_items.size() >= MaximumProjects)
        return ServiceResult::failure(QStringLiteral("网格图项目最多 ") + QString::number(MaximumProjects) + QStringLiteral(" 个。"));
    Item item;
    item.id = newId();
    item.title = trimmed;
    item.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);

    // 先写入初始数据文件，成功后才提交索引，避免出现索引指向缺失内容的记录。
    QJsonObject content{{QStringLiteral("version"), 1},
        {QStringLiteral("lineColor"), QStringLiteral("#94a3b8")},
        {QStringLiteral("lineWidth"), 1},
        {QStringLiteral("cells"), QJsonArray()}};
    const QByteArray bytes = QJsonDocument(content).toJson();
    const QString path = mapPath(item.id);
    if (!safePath(path) || !QDir().mkpath(QDir(m_root).filePath(QStringLiteral("maps"))))
        return ServiceResult::failure(QStringLiteral("网格图保存路径不安全。"));
    QSaveFile contentFile(path);
    contentFile.setDirectWriteFallback(false);
    if (!contentFile.open(QIODevice::WriteOnly) || contentFile.write(bytes) != bytes.size()
        || !contentFile.commit())
        return ServiceResult::failure(QStringLiteral("无法创建网格图数据文件：") + contentFile.errorString());

    QVector<Item> items = m_items;
    items.append(item);
    const QVariantMap committed = commitItems(items);
    if (!committed.value(QStringLiteral("ok")).toBool())
        return committed;
    return ServiceResult::success(summary(item));
}

QVariantMap GridMapService::rename(const QString& id, const QString& title)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const int index = position(id);
    if (index < 0)
        return ServiceResult::failure(QStringLiteral("网格图项目不存在。"));
    const QString trimmed = title.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > 200)
        return ServiceResult::failure(QStringLiteral("网格图名称不能为空，且不能超过 200 个字符。"));
    QVector<Item> items = m_items;
    items[index].title = trimmed;
    items[index].updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const QVariantMap committed = commitItems(items);
    return committed.value(QStringLiteral("ok")).toBool()
        ? ServiceResult::success(summary(items.at(index))) : committed;
}

QVariantMap GridMapService::remove(const QStringList& ids)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const QSet<QString> selected(ids.begin(), ids.end());
    for (const QString& id : selected) {
        if (position(id) < 0)
            return ServiceResult::failure(QStringLiteral("部分待删除网格图不存在，已取消本次删除。"));
    }
    QVector<Item> items;
    for (const Item& item : m_items) {
        if (!selected.contains(item.id))
            items.append(item);
    }
    const QVariantMap committed = commitItems(items);
    if (!committed.value(QStringLiteral("ok")).toBool())
        return committed;
    // 索引提交成功后移除数据文件；个别文件删除失败只留下无害的孤立文件。
    for (const QString& id : selected) {
        const QString path = mapPath(id);
        if (safePath(path))
            QFile::remove(path);
    }
    return ServiceResult::success();
}

QVariantMap GridMapService::read(const QString& id) const
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const int index = position(id);
    if (index < 0)
        return ServiceResult::failure(QStringLiteral("网格图项目不存在。"));
    const QString path = mapPath(id);
    if (!safePath(path))
        return ServiceResult::failure(QStringLiteral("网格图数据路径不安全，已拒绝读取。"));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaximumMapBytes)
        return ServiceResult::failure(QStringLiteral("无法读取网格图数据，文件可能缺失、过大或不可访问。"));
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    const QJsonObject root = document.object();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || root.value(QStringLiteral("version")).toInt() != 1
        || !root.value(QStringLiteral("cells")).isArray()
        || !root.value(QStringLiteral("lineColor")).isString())
        return ServiceResult::failure(QStringLiteral("网格图数据文件损坏，无法读取。"));
    QVariantMap result = summary(m_items.at(index));
    result.insert(QStringLiteral("lineColor"), root.value(QStringLiteral("lineColor")).toString());
    result.insert(QStringLiteral("lineWidth"), root.value(QStringLiteral("lineWidth")).toInt(1));
    QVariantList cells;
    for (const QJsonValue& value : root.value(QStringLiteral("cells")).toArray()) {
        const QJsonArray triple = value.toArray();
        if (triple.size() != 3 || !triple.at(0).isDouble() || !triple.at(1).isDouble()
            || !triple.at(2).isString())
            return ServiceResult::failure(QStringLiteral("网格图数据文件包含无效格子，无法读取。"));
        cells.append(QVariant(QVariantList{triple.at(0).toVariant(), triple.at(1).toVariant(), triple.at(2).toString()}));
    }
    result.insert(QStringLiteral("cells"), cells);
    return ServiceResult::success(result);
}

QVariantList GridMapService::normalizeCells(const QVariantList& cells) const
{
    QVariantList normalized;
    QSet<QString> seen;
    for (const QVariant& value : cells) {
        const QVariantList triple = value.toList();
        if (triple.size() != 3)
            return {};
        qint64 x = 0;
        qint64 y = 0;
        if (!toCoordinate(triple.at(0), &x) || !toCoordinate(triple.at(1), &y))
            return {};
        const QString color = normalizeColor(triple.at(2).toString());
        if (color.isEmpty())
            return {};
        const QString key = QString::number(x) + QLatin1Char(',') + QString::number(y);
        if (!seen.contains(key)) {
            seen.insert(key);
            normalized.append(QVariant(QVariantList{static_cast<double>(x), static_cast<double>(y), color}));
        }
    }
    return normalized;
}

bool GridMapService::normalizeStyle(const QVariantMap& data, QString* lineColor, int* lineWidth) const
{
    const QString color = normalizeColor(data.value(QStringLiteral("lineColor")).toString());
    if (color.isEmpty())
        return false;
    bool valid = false;
    const double width = data.value(QStringLiteral("lineWidth")).toDouble(&valid);
    if (!valid || width < 1 || width > 6 || width != std::round(width))
        return false;
    *lineColor = color;
    *lineWidth = static_cast<int>(width);
    return true;
}

QVariantMap GridMapService::save(const QString& id, const QVariantMap& data)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const int index = position(id);
    if (index < 0)
        return ServiceResult::failure(QStringLiteral("网格图项目不存在，无法保存。"));
    QString lineColor;
    int lineWidth = 1;
    if (!normalizeStyle(data, &lineColor, &lineWidth))
        return ServiceResult::failure(QStringLiteral("网格线设置无效：颜色需为十六进制颜色，粗细需在 1–6 之间。"));
    if (data.value(QStringLiteral("cells")).toList().size() > MaximumCells)
        return ServiceResult::failure(QStringLiteral("网格图格子数量超过上限，无法保存。"));
    const QVariantList cells = normalizeCells(data.value(QStringLiteral("cells")).toList());
    if (cells.isEmpty() && !data.value(QStringLiteral("cells")).toList().isEmpty())
        return ServiceResult::failure(QStringLiteral("网格图包含无效的格子坐标或颜色，本次保存已取消。"));
    if (cells.size() > MaximumCells)
        return ServiceResult::failure(QStringLiteral("网格图格子数量超过上限，无法保存。"));

    QJsonArray cellArray;
    for (const QVariant& triple : cells) {
        const QVariantList parts = triple.toList();
        cellArray.append(QJsonArray{parts.at(0).toDouble(), parts.at(1).toDouble(), parts.at(2).toString()});
    }
    const QByteArray bytes = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
        {QStringLiteral("lineColor"), lineColor}, {QStringLiteral("lineWidth"), lineWidth},
        {QStringLiteral("cells"), cellArray}}).toJson();

    // 先准备索引写入，尽早发现只读路径或磁盘错误。
    QVector<Item> items = m_items;
    items[index].updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QString error;
    QSaveFile indexFile(m_indexPath);
    if (!prepareIndex(indexFile, items, error))
        return ServiceResult::failure(error);

    const QString path = mapPath(id);
    if (!safePath(path))
        return ServiceResult::failure(QStringLiteral("网格图保存路径不安全。"));
    const bool hadContent = QFileInfo::exists(path);
    QByteArray previous;
    if (hadContent) {
        QFile old(path);
        if (!old.open(QIODevice::ReadOnly) || old.size() > MaximumMapBytes)
            return ServiceResult::failure(QStringLiteral("无法备份现有网格图数据，已停止保存。"));
        previous = old.readAll();
    }
    QSaveFile contentFile(path);
    contentFile.setDirectWriteFallback(false);
    if (!contentFile.open(QIODevice::WriteOnly) || contentFile.write(bytes) != bytes.size()
        || !contentFile.commit())
        return ServiceResult::failure(QStringLiteral("无法保存网格图数据：") + contentFile.errorString());
    if (!indexFile.commit()) {
        bool restored = true;
        if (hadContent) {
            QSaveFile rollback(path);
            restored = rollback.open(QIODevice::WriteOnly) && rollback.write(previous) == previous.size()
                && rollback.commit();
        }
        return ServiceResult::failure(QStringLiteral("无法提交网格图索引：") + indexFile.errorString()
            + (restored ? QString() : QStringLiteral("；原数据回滚失败，请保留文件并检查磁盘状态。")));
    }
    m_items = items;
    emit changed();
    QVariantMap result = summary(items.at(index));
    result.insert(QStringLiteral("cellCount"), cells.size());
    return ServiceResult::success(result);
}
