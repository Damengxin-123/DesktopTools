#include "ShortcutService.h"
#include "ServiceResult.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace
{
// 生成新增分类或条目的稳定标识。
QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// 验证数字确实是支持的目标类型，拒绝小数和字符串转换。
bool validType(const QJsonValue& value)
{
    const double number = value.toDouble(-1);
    return value.isDouble() && std::isfinite(number) && std::floor(number) == number
        && number >= 0 && number <= 2;
}

// 将目标转换成重复校验键，兼容 Windows 路径分隔符及大小写。
QString targetKey(const QString& target, int type)
{
    if (type == 1)
        return QUrl::fromUserInput(target).adjusted(QUrl::NormalizePathSegments).toString();
    QString path = QDir::cleanPath(QDir::fromNativeSeparators(target));
#ifdef Q_OS_WIN
    path = path.toCaseFolded();
#endif
    return path;
}
}

ShortcutService::ShortcutService(const QString& dataRoot, QObject* parent)
    : QObject(parent)
    , m_path(QDir(dataRoot).filePath(QStringLiteral("shortcut/shortcuts.v2.json")))
    , m_legacyPath(QDir(dataRoot).filePath(QStringLiteral("shortcut/shortcuts_config.txt")))
    , m_categories({{QStringLiteral("default"), QStringLiteral("默认分类")}})
{
    load();
}

QVariantMap ShortcutService::data() const
{
    QVariantList categories;
    for (const Category& category : m_categories)
        categories.append(QVariantMap{{QStringLiteral("id"), category.id}, {QStringLiteral("name"), category.name}});
    QVariantList items;
    for (const Shortcut& item : m_items)
    {
        items.append(QVariantMap{
            {QStringLiteral("id"), item.id}, {QStringLiteral("categoryId"), item.categoryId},
            {QStringLiteral("title"), item.title}, {QStringLiteral("target"), item.target},
            {QStringLiteral("type"), item.type}
        });
    }
    return {{QStringLiteral("categories"), categories}, {QStringLiteral("items"), items},
            {QStringLiteral("warnings"), m_warnings}};
}

QVariantMap ShortcutService::snapshot() const
{
    return m_loadError.isEmpty() ? ServiceResult::success(data()) : ServiceResult::failure(m_loadError);
}

QString ShortcutService::validate(const QVector<Category>& categories, const QVector<Shortcut>& items) const
{
    QSet<QString> categoryIds;
    QSet<QString> categoryNames;
    for (const Category& category : categories)
    {
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
    QSet<QString> itemIds;
    QHash<QString, QSet<QString>> titles;
    QHash<QString, QSet<QString>> targets;
    for (const Shortcut& item : items)
    {
        if (item.id.trimmed().isEmpty() || item.title.trimmed().isEmpty() || item.target.trimmed().isEmpty())
            return QStringLiteral("快捷方式标识、标题和目标不能为空。");
        if (!categoryIds.contains(item.categoryId))
            return QStringLiteral("快捷方式所属分类不存在：%1").arg(item.title);
        if (item.type < 0 || item.type > 2)
            return QStringLiteral("快捷方式类型必须为目录、网址或文件。");
        if (itemIds.contains(item.id))
            return QStringLiteral("快捷方式标识重复。");
        const QString title = item.title.toCaseFolded();
        const QString target = targetKey(item.target, item.type);
        if (titles[item.categoryId].contains(title))
            return QStringLiteral("目标分类已有同名快捷方式：%1").arg(item.title);
        if (targets[item.categoryId].contains(target))
            return QStringLiteral("目标分类已有相同地址：%1").arg(item.target);
        titles[item.categoryId].insert(title);
        targets[item.categoryId].insert(target);
        itemIds.insert(item.id);
    }
    return {};
}

void ShortcutService::load()
{
    QFile file(m_path);
    if (!file.exists())
    {
        if (QFileInfo::exists(m_legacyPath))
            importLegacy(m_legacyPath);
        return;
    }
    if (!file.open(QIODevice::ReadOnly))
    {
        m_loadError = QStringLiteral("无法读取快捷方式配置，已禁止覆盖：%1").arg(file.errorString());
        return;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
    {
        m_loadError = QStringLiteral("快捷方式配置损坏，已保留原文件并禁止覆盖。请修复后重新启动。");
        return;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("version")) != QJsonValue(2)
        || !root.value(QStringLiteral("categories")).isArray() || !root.value(QStringLiteral("items")).isArray())
    {
        m_loadError = QStringLiteral("快捷方式配置版本或结构无效，已禁止覆盖。");
        return;
    }
    QVector<Category> categories;
    QVector<Shortcut> items;
    for (const QJsonValue& value : root.value(QStringLiteral("categories")).toArray())
    {
        const QJsonObject category = value.toObject();
        if (!value.isObject() || !category.value(QStringLiteral("id")).isString()
            || !category.value(QStringLiteral("name")).isString())
        {
            m_loadError = QStringLiteral("快捷方式分类字段无效，已禁止覆盖。");
            return;
        }
        categories.append({category.value(QStringLiteral("id")).toString(), category.value(QStringLiteral("name")).toString()});
    }
    for (const QJsonValue& value : root.value(QStringLiteral("items")).toArray())
    {
        const QJsonObject item = value.toObject();
        if (!value.isObject() || !item.value(QStringLiteral("id")).isString()
            || !item.value(QStringLiteral("categoryId")).isString() || !item.value(QStringLiteral("title")).isString()
            || !item.value(QStringLiteral("target")).isString() || !validType(item.value(QStringLiteral("type"))))
        {
            m_loadError = QStringLiteral("快捷方式条目字段无效，已禁止覆盖。");
            return;
        }
        items.append({item.value(QStringLiteral("id")).toString(), item.value(QStringLiteral("categoryId")).toString(),
                      item.value(QStringLiteral("title")).toString(), item.value(QStringLiteral("target")).toString(),
                      item.value(QStringLiteral("type")).toInt()});
    }
    const QString validationError = validate(categories, items);
    if (!validationError.isEmpty())
    {
        m_loadError = QStringLiteral("快捷方式配置无效，已禁止覆盖：%1").arg(validationError);
        return;
    }
    if (root.contains(QStringLiteral("warnings")))
    {
        if (!root.value(QStringLiteral("warnings")).isArray())
        {
            m_loadError = QStringLiteral("快捷方式导入记录格式无效，已禁止覆盖。");
            return;
        }
        for (const QJsonValue& warning : root.value(QStringLiteral("warnings")).toArray())
        {
            if (!warning.isString())
            {
                m_loadError = QStringLiteral("快捷方式导入记录内容无效，已禁止覆盖。");
                return;
            }
            m_warnings.append(warning.toString());
        }
    }
    m_categories = categories;
    m_items = items;
}

void ShortcutService::importLegacy(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        m_loadError = QStringLiteral("无法读取旧版快捷方式配置，已保留原文件：%1").arg(file.errorString());
        return;
    }
    QVector<Category> categories = m_categories;
    QVector<Shortcut> items;
    QTextStream stream(&file);
    int lineNumber = 0;
    while (!stream.atEnd())
    {
        const QString line = stream.readLine().trimmed();
        ++lineNumber;
        if (line.isEmpty())
            continue;
        const QStringList parts = line.split(QLatin1Char(','));
        bool typeOk = false;
        const int type = parts.size() >= 3 ? parts.at(2).trimmed().toInt(&typeOk) : -1;
        if ((parts.size() != 3 && parts.size() != 4) || !typeOk || type < 0 || type > 2
            || parts.at(0).trimmed().isEmpty() || parts.at(1).trimmed().isEmpty())
        {
            m_warnings.append(QStringLiteral("旧配置第 %1 行无法无歧义解析，未自动导入；原文件已保留：%2")
                                  .arg(lineNumber).arg(line));
            continue;
        }
        QString categoryName = parts.size() == 4 ? parts.at(3).trimmed() : QStringLiteral("默认分类");
        if (categoryName.isEmpty())
            categoryName = QStringLiteral("默认分类");
        auto category = std::find_if(categories.cbegin(), categories.cend(), [&categoryName](const Category& value) {
            return value.name.compare(categoryName, Qt::CaseInsensitive) == 0;
        });
        QString categoryId;
        if (category == categories.cend())
        {
            categoryId = newId();
            categories.append({categoryId, categoryName});
        }
        else
            categoryId = category->id;
        const Shortcut item{newId(), categoryId, parts.at(0).trimmed(), parts.at(1).trimmed(), type};
        QVector<Shortcut> candidate = items;
        candidate.append(item);
        const QString error = validate(categories, candidate);
        if (!error.isEmpty())
        {
            m_warnings.append(QStringLiteral("旧配置第 %1 行未导入（%2），原文件已保留：%3")
                                  .arg(lineNumber).arg(error, line));
            continue;
        }
        items = candidate;
    }
    const QVariantMap result = commit(categories, items);
    if (!result.value(QStringLiteral("ok")).toBool())
        m_loadError = QStringLiteral("旧数据已读取但无法保存新版配置，原文件保持不变：%1")
                          .arg(result.value(QStringLiteral("error")).toString());
}

QVariantMap ShortcutService::commit(const QVector<Category>& categories, const QVector<Shortcut>& items)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const QString error = validate(categories, items);
    if (!error.isEmpty())
        return ServiceResult::failure(error);
    QJsonArray categoryArray;
    for (const Category& category : categories)
        categoryArray.append(QJsonObject{{QStringLiteral("id"), category.id}, {QStringLiteral("name"), category.name}});
    QJsonArray itemArray;
    for (const Shortcut& item : items)
    {
        itemArray.append(QJsonObject{
            {QStringLiteral("id"), item.id}, {QStringLiteral("categoryId"), item.categoryId},
            {QStringLiteral("title"), item.title}, {QStringLiteral("target"), item.target},
            {QStringLiteral("type"), item.type}
        });
    }
    const QJsonObject document{
        {QStringLiteral("version"), 2}, {QStringLiteral("categories"), categoryArray},
        {QStringLiteral("items"), itemArray}, {QStringLiteral("warnings"), QJsonArray::fromStringList(m_warnings)}
    };
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath()))
        return ServiceResult::failure(QStringLiteral("无法创建快捷方式配置目录。"));
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly))
        return ServiceResult::failure(QStringLiteral("无法写入快捷方式：%1").arg(file.errorString()));
    const QByteArray payload = QJsonDocument(document).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size() || !file.commit())
        return ServiceResult::failure(QStringLiteral("保存快捷方式失败，原配置保持不变：%1").arg(file.errorString()));
    m_categories = categories;
    m_items = items;
    emit changed();
    return ServiceResult::success();
}

QVariantMap ShortcutService::saveCategory(const QString& id, const QString& name)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    QVector<Category> categories = m_categories;
    const QString cleanName = name.trimmed();
    QString savedId = id;
    if (id.isEmpty())
    {
        savedId = newId();
        categories.append({savedId, cleanName});
    }
    else
    {
        auto category = std::find_if(categories.begin(), categories.end(), [&id](const Category& value) { return value.id == id; });
        if (category == categories.end())
            return ServiceResult::failure(QStringLiteral("要修改的分类不存在。"));
        category->name = cleanName;
    }
    const QVariantMap result = commit(categories, m_items);
    return result.value(QStringLiteral("ok")).toBool()
        ? ServiceResult::success(QVariantMap{{QStringLiteral("id"), savedId}, {QStringLiteral("name"), cleanName}}) : result;
}

QVariantMap ShortcutService::removeCategory(const QString& id)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    if (id == QStringLiteral("default"))
        return ServiceResult::failure(QStringLiteral("默认分类不能删除。"));
    QVector<Category> categories = m_categories;
    const auto category = std::find_if(categories.begin(), categories.end(), [&id](const Category& value) { return value.id == id; });
    if (category == categories.end())
        return ServiceResult::failure(QStringLiteral("要删除的分类不存在。"));
    categories.erase(category);
    QVector<Shortcut> items = m_items;
    for (Shortcut& item : items)
    {
        if (item.categoryId == id)
            item.categoryId = QStringLiteral("default");
    }
    return commit(categories, items);
}

QVariantMap ShortcutService::saveShortcut(const QVariantMap& item)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    if (!validType(QJsonValue::fromVariant(item.value(QStringLiteral("type")))))
        return ServiceResult::failure(QStringLiteral("快捷方式类型必须为 0（目录）、1（网址）或 2（文件）。"));
    for (const QString& key : {QStringLiteral("categoryId"), QStringLiteral("title"), QStringLiteral("target")})
    {
        if (!QJsonValue::fromVariant(item.value(key)).isString())
            return ServiceResult::failure(QStringLiteral("快捷方式字段格式错误：%1").arg(key));
    }
    if (item.contains(QStringLiteral("id")) && !QJsonValue::fromVariant(item.value(QStringLiteral("id"))).isString())
        return ServiceResult::failure(QStringLiteral("快捷方式标识格式错误。"));
    Shortcut value{item.value(QStringLiteral("id")).toString(), item.value(QStringLiteral("categoryId")).toString(),
                   item.value(QStringLiteral("title")).toString().trimmed(), item.value(QStringLiteral("target")).toString().trimmed(),
                   item.value(QStringLiteral("type")).toInt()};
    QVector<Shortcut> items = m_items;
    if (value.id.isEmpty())
    {
        value.id = newId();
        items.append(value);
    }
    else
    {
        auto existing = std::find_if(items.begin(), items.end(), [&value](const Shortcut& entry) { return entry.id == value.id; });
        if (existing == items.end())
            return ServiceResult::failure(QStringLiteral("要修改的快捷方式不存在。"));
        *existing = value;
    }
    const QVariantMap result = commit(m_categories, items);
    return result.value(QStringLiteral("ok")).toBool()
        ? ServiceResult::success(QVariantMap{{QStringLiteral("id"), value.id}, {QStringLiteral("categoryId"), value.categoryId},
              {QStringLiteral("title"), value.title}, {QStringLiteral("target"), value.target}, {QStringLiteral("type"), value.type}}) : result;
}

QVariantMap ShortcutService::removeShortcuts(const QStringList& ids)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    QSet<QString> requested;
    for (const QString& id : ids)
    {
        if (std::none_of(m_items.cbegin(), m_items.cend(), [&id](const Shortcut& value) { return value.id == id; }))
            return ServiceResult::failure(QStringLiteral("要删除的快捷方式不存在，未删除任何条目。"));
        requested.insert(id);
    }
    QVector<Shortcut> items = m_items;
    items.erase(std::remove_if(items.begin(), items.end(), [&requested](const Shortcut& item) { return requested.contains(item.id); }), items.end());
    return commit(m_categories, items);
}

QVariantMap ShortcutService::moveShortcut(const QString& id, const QString& categoryId, const QString& beforeId)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    if (std::none_of(m_categories.cbegin(), m_categories.cend(), [&categoryId](const Category& value) { return value.id == categoryId; }))
        return ServiceResult::failure(QStringLiteral("目标分类不存在。"));
    QVector<Shortcut> items = m_items;
    const auto existing = std::find_if(items.begin(), items.end(), [&id](const Shortcut& value) { return value.id == id; });
    if (existing == items.end())
        return ServiceResult::failure(QStringLiteral("要移动的快捷方式不存在。"));
    if (beforeId == id)
        return existing->categoryId == categoryId ? ServiceResult::success()
                                                : ServiceResult::failure(QStringLiteral("不能以跨分类移动的条目自身作为参照。"));
    Shortcut moving = *existing;
    moving.categoryId = categoryId;
    items.erase(existing);
    auto position = items.end();
    if (!beforeId.isEmpty())
    {
        position = std::find_if(items.begin(), items.end(), [&beforeId](const Shortcut& value) { return value.id == beforeId; });
        if (position == items.end() || position->categoryId != categoryId)
            return ServiceResult::failure(QStringLiteral("排序参照条目不存在或不属于目标分类。"));
    }
    else
    {
        for (auto iterator = items.begin(); iterator != items.end(); ++iterator)
        {
            if (iterator->categoryId == categoryId)
                position = iterator + 1;
        }
    }
    items.insert(position, moving);
    return commit(m_categories, items);
}

QVariantMap ShortcutService::openShortcut(const QString& id)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const auto item = std::find_if(m_items.cbegin(), m_items.cend(), [&id](const Shortcut& value) { return value.id == id; });
    if (item == m_items.cend())
        return ServiceResult::failure(QStringLiteral("要打开的快捷方式不存在。"));
    const QUrl url = item->type == 1 ? QUrl::fromUserInput(item->target) : QUrl::fromLocalFile(item->target);
    if (!url.isValid() || url.isEmpty())
        return ServiceResult::failure(QStringLiteral("快捷方式目标地址无效。"));
    if (item->type != 1)
    {
        const QFileInfo target(item->target);
        if (!target.exists() || (item->type == 0 && !target.isDir()) || (item->type == 2 && !target.isFile()))
            return ServiceResult::failure(QStringLiteral("快捷方式目标不存在或与所选类型不符：%1").arg(item->target));
    }
    return QDesktopServices::openUrl(url) ? ServiceResult::success()
        : ServiceResult::failure(QStringLiteral("系统无法打开该快捷方式，请检查目标和默认应用。"));
}
