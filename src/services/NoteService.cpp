#include "NoteService.h"
#include "ServiceResult.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextImageFormat>
#include <QTextTable>
#include <QUrl>
#include <QUuid>

namespace {
/** 单个富文本文件允许的最大字节数，避免异常文件占用过多内存。 */
constexpr qint64 MaximumHtmlBytes = 32 * 1024 * 1024;
/** 单张图片允许的最大压缩字节数。 */
constexpr qint64 MaximumImageBytes = 12 * 1024 * 1024;
/** 单张图片允许的最大像素数量。 */
constexpr qint64 MaximumImagePixels = 20 * 1024 * 1024;

/** 根据当前平台以正确的大小写规则判断路径从属关系。 */
bool isWithin(const QString& candidate, const QString& root)
{
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    return candidate.compare(root, sensitivity) == 0
        || candidate.startsWith(root + QLatin1Char('/'), sensitivity);
}

/** 生成只含十六进制字符的稳定记录标识。 */
QString newId()
{
    return QUuid::createUuid().toString(QUuid::Id128);
}

/** 禁止富文本解析器自动读取磁盘、网络或外部样式资源。 */
class IsolatedDocument final : public QTextDocument
{
protected:
    /** 所有资源由服务显式校验后嵌入，此处始终拒绝自动加载。 */
    QVariant loadResource(int, const QUrl&) override { return {}; }
};
}

NoteService::NoteService(const QString& dataRoot, QObject* parent)
    : QObject(parent)
    , m_dataRoot(QDir::cleanPath(QFileInfo(dataRoot).absoluteFilePath()))
    , m_noteRoot(QDir(m_dataRoot).filePath(QStringLiteral("note")))
    , m_indexPath(QDir(m_noteRoot).filePath(QStringLiteral("notes.v2.json")))
{
    load();
}

bool NoteService::safeDirectoryName(const QString& name)
{
    return !name.isEmpty() && name != QStringLiteral(".") && name != QStringLiteral("..")
        && !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\'))
        && !name.contains(QLatin1Char(':')) && !name.contains(QChar::Null)
        && !name.endsWith(QLatin1Char('.')) && !name.endsWith(QLatin1Char(' '));
}

bool NoteService::safePath(const QString& path) const
{
    const QString absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (!isWithin(absolute, m_dataRoot))
        return false;

    // 检查每个已有路径片段，不允许符号链接或 Windows 目录联接绕过根目录。
    QString current = m_dataRoot;
    const QString relative = QDir(m_dataRoot).relativeFilePath(absolute);
    const QString canonicalRoot = QFileInfo(m_dataRoot).canonicalFilePath();
    for (const QString& component : relative.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (component == QStringLiteral("."))
            continue;
        if (!safeDirectoryName(component))
            return false;
        current = QDir(current).filePath(component);
        const QFileInfo info(current);
        if (info.isSymbolicLink() || info.isJunction())
            return false;
        if (info.exists() && !canonicalRoot.isEmpty()
            && !isWithin(info.canonicalFilePath(), canonicalRoot))
            return false;
    }
    return true;
}

void NoteService::load()
{
    if (!safePath(m_noteRoot) || !safePath(m_indexPath)) {
        m_loadError = QStringLiteral("便签数据路径包含不安全的目录链接，已停止读取和保存。");
        return;
    }
    if (!QFileInfo::exists(m_indexPath)) {
        importLegacy();
        return;
    }
    QFile file(m_indexPath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaximumHtmlBytes) {
        m_loadError = QStringLiteral("无法读取便签索引，已保护原文件，禁止覆盖。");
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    const QJsonObject root = document.object();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || root.value(QStringLiteral("version")).toInt() != 2
        || !root.value(QStringLiteral("categories")).isArray()
        || !root.value(QStringLiteral("items")).isArray()) {
        m_loadError = QStringLiteral("便签索引损坏或版本不支持，已保护原文件，禁止覆盖。");
        return;
    }
    QSet<QString> categoryIds;
    for (const QJsonValue& value : root.value(QStringLiteral("categories")).toArray()) {
        const QJsonObject object = value.toObject();
        Category category{object.value(QStringLiteral("id")).toString(), object.value(QStringLiteral("name")).toString()};
        if (!value.isObject() || category.id.isEmpty() || category.name.trimmed().isEmpty()
            || categoryIds.contains(category.id)) {
            m_loadError = QStringLiteral("便签索引包含无效或重复分类，已禁止覆盖。");
            return;
        }
        categoryIds.insert(category.id);
        m_categories.append(category);
    }
    if (!categoryIds.contains(QStringLiteral("default"))) {
        m_loadError = QStringLiteral("便签索引缺少默认分类，已禁止覆盖。");
        return;
    }
    QSet<QString> noteIds;
    QSet<QString> directories;
    static const QRegularExpression stableId(QStringLiteral("^[0-9a-f]{32}$"));
    for (const QJsonValue& value : root.value(QStringLiteral("items")).toArray()) {
        const QJsonObject object = value.toObject();
        Note note{object.value(QStringLiteral("id")).toString(),
                  object.value(QStringLiteral("categoryId")).toString(),
                  object.value(QStringLiteral("title")).toString(),
                  object.value(QStringLiteral("updatedAt")).toString(),
                  object.value(QStringLiteral("directory")).toString()};
        const QString directoryKey = note.directory.toCaseFolded();
        if (!value.isObject() || !stableId.match(note.id).hasMatch() || noteIds.contains(note.id)
            || !categoryIds.contains(note.categoryId) || note.title.trimmed().isEmpty()
            || !safeDirectoryName(note.directory) || note.directory.startsWith(QLatin1Char('.'))
            || directories.contains(directoryKey)
            || !safePath(QDir(m_noteRoot).filePath(note.directory + QStringLiteral("/index.html")))) {
            m_loadError = QStringLiteral("便签索引包含无效记录或越界路径，已禁止覆盖。");
            return;
        }
        noteIds.insert(note.id);
        directories.insert(directoryKey);
        m_notes.append(note);
    }
}

void NoteService::importLegacy()
{
    m_categories.append({QStringLiteral("default"), QStringLiteral("默认分类")});
    const QDir root(m_noteRoot);
    QSet<QString> imported;
    const QString legacyPath = root.filePath(QStringLiteral("tree_config.json"));
    const bool legacyExists = QFileInfo::exists(legacyPath);
    QFile legacy(legacyPath);
    QJsonObject oldRoot;
    if (legacyExists) {
        QJsonParseError error;
        if (safePath(legacyPath) && legacy.open(QIODevice::ReadOnly) && legacy.size() <= MaximumHtmlBytes) {
            const QJsonDocument document = QJsonDocument::fromJson(legacy.readAll(), &error);
            if (error.error == QJsonParseError::NoError && document.isObject()
                && document.object().value(QStringLiteral("categories")).isArray())
                oldRoot = document.object();
        }
        if (oldRoot.isEmpty())
            m_warnings.append(QStringLiteral("旧便签分类索引无法读取，已尝试从安全目录恢复便签。"));
    }

    // 只登记存在的单层目录，分类索引中的目录穿越及链接均忽略。
    const auto importNote = [this, &root, &imported](const QString& name, const QString& categoryId) {
        const QString htmlPath = root.filePath(name + QStringLiteral("/index.html"));
        if (imported.contains(name) || !safeDirectoryName(name) || name.startsWith(QLatin1Char('.'))
            || !safePath(htmlPath) || !QFileInfo(htmlPath).isFile())
            return;
        imported.insert(name);
        m_notes.append({newId(), categoryId, name,
                        QFileInfo(htmlPath).lastModified().toUTC().toString(Qt::ISODateWithMs), name});
    };
    for (const QJsonValue& value : oldRoot.value(QStringLiteral("categories")).toArray()) {
        const QJsonObject object = value.toObject();
        const QString name = object.value(QStringLiteral("name")).toString().trimmed();
        QString categoryId = QStringLiteral("default");
        if (!name.isEmpty() && name != QStringLiteral("默认分类")) {
            for (const Category& category : m_categories) {
                if (category.name == name) {
                    categoryId = category.id;
                    break;
                }
            }
            if (categoryId == QStringLiteral("default")) {
                categoryId = newId();
                m_categories.append({categoryId, name});
            }
        }
        for (const QJsonValue& item : object.value(QStringLiteral("items")).toArray())
            importNote(item.toString(), categoryId);
    }
    for (const QString& name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        importNote(name, QStringLiteral("default"));

    QString error;
    QSaveFile index(m_indexPath);
    if (!prepareIndex(index, m_categories, m_notes, error) || !index.commit())
        m_warnings.append(QStringLiteral("迁移索引暂未保存：") + (error.isEmpty() ? index.errorString() : error));
}

QVariantMap NoteService::summary(const Note& note)
{
    return {{QStringLiteral("id"), note.id}, {QStringLiteral("categoryId"), note.categoryId},
            {QStringLiteral("title"), note.title}, {QStringLiteral("updatedAt"), note.updatedAt}};
}

QVariantMap NoteService::snapshot() const
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    QVariantList categories;
    QVariantList items;
    for (const Category& category : m_categories)
        categories.append(QVariantMap{{QStringLiteral("id"), category.id}, {QStringLiteral("name"), category.name}});
    for (const Note& note : m_notes)
        items.append(summary(note));
    return ServiceResult::success(QVariantMap{{QStringLiteral("categories"), categories},
        {QStringLiteral("items"), items}, {QStringLiteral("warnings"), m_warnings}});
}

bool NoteService::hasCategory(const QString& id) const
{
    for (const Category& category : m_categories) {
        if (category.id == id)
            return true;
    }
    return false;
}

int NoteService::notePosition(const QString& id) const
{
    for (qsizetype index = 0; index < m_notes.size(); ++index) {
        if (m_notes.at(index).id == id)
            return static_cast<int>(index);
    }
    return -1;
}

bool NoteService::prepareIndex(QSaveFile& file, const QVector<Category>& categories,
                               const QVector<Note>& notes, QString& error) const
{
    if (!m_loadError.isEmpty()) {
        error = m_loadError;
        return false;
    }
    if (!safePath(m_indexPath) || !QDir().mkpath(m_noteRoot)) {
        error = QStringLiteral("便签索引路径不安全或目录无法创建。");
        return false;
    }
    QJsonArray categoryArray;
    QJsonArray noteArray;
    for (const Category& category : categories)
        categoryArray.append(QJsonObject{{QStringLiteral("id"), category.id}, {QStringLiteral("name"), category.name}});
    for (const Note& note : notes) {
        QJsonObject object = QJsonObject::fromVariantMap(summary(note));
        object.insert(QStringLiteral("directory"), note.directory);
        noteArray.append(object);
    }
    const QByteArray bytes = QJsonDocument(QJsonObject{{QStringLiteral("version"), 2},
        {QStringLiteral("categories"), categoryArray}, {QStringLiteral("items"), noteArray}}).toJson();
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        error = QStringLiteral("无法保存便签索引：") + file.errorString();
        return false;
    }
    return true;
}

QVariantMap NoteService::commit(const QVector<Category>& categories, const QVector<Note>& notes)
{
    QString error;
    QSaveFile index(m_indexPath);
    if (!prepareIndex(index, categories, notes, error))
        return ServiceResult::failure(error);
    if (!index.commit())
        return ServiceResult::failure(QStringLiteral("无法提交便签索引：") + index.errorString());
    m_categories = categories;
    m_notes = notes;
    emit changed();
    return ServiceResult::success();
}

QVariantMap NoteService::saveCategory(const QString& id, const QString& name)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > 200)
        return ServiceResult::failure(QStringLiteral("分类名称不能为空，且不能超过 200 个字符。"));
    if (!id.isEmpty() && !hasCategory(id))
        return ServiceResult::failure(QStringLiteral("分类不存在。"));
    for (const Category& category : m_categories) {
        if (category.id != id && category.name == trimmed)
            return ServiceResult::failure(QStringLiteral("已存在同名分类。"));
    }
    QVector<Category> categories = m_categories;
    const QString categoryId = id.isEmpty() ? newId() : id;
    if (id.isEmpty())
        categories.append({categoryId, trimmed});
    else {
        for (Category& category : categories) {
            if (category.id == id)
                category.name = trimmed;
        }
    }
    const QVariantMap result = commit(categories, m_notes);
    return result.value(QStringLiteral("ok")).toBool()
        ? ServiceResult::success(QVariantMap{{QStringLiteral("id"), categoryId}, {QStringLiteral("name"), trimmed}}) : result;
}

QVariantMap NoteService::removeCategory(const QString& id)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    if (id == QStringLiteral("default"))
        return ServiceResult::failure(QStringLiteral("默认分类不能删除。"));
    if (!hasCategory(id))
        return ServiceResult::failure(QStringLiteral("分类不存在。"));
    QVector<Category> categories = m_categories;
    for (qsizetype index = 0; index < categories.size(); ++index) {
        if (categories.at(index).id == id) {
            categories.removeAt(index);
            break;
        }
    }
    QVector<Note> notes = m_notes;
    for (Note& note : notes) {
        if (note.categoryId == id)
            note.categoryId = QStringLiteral("default");
    }
    return commit(categories, notes);
}

QString NoteService::imageDataUrl(const QString& source, const QString& noteDirectory) const
{
    QByteArray bytes;
    if (source.startsWith(QStringLiteral("data:"), Qt::CaseInsensitive)) {
        static const QRegularExpression expression(QStringLiteral("^data:image/(?:png|jpeg|jpg|gif|webp|bmp);base64,([A-Za-z0-9+/=\\s]+)$"), QRegularExpression::CaseInsensitiveOption);
        if (source.size() > MaximumImageBytes * 2)
            return {};
        const QRegularExpressionMatch match = expression.match(source);
        if (!match.hasMatch())
            return {};
        bytes = QByteArray::fromBase64(match.captured(1).toLatin1());
    } else {
        if (noteDirectory.isEmpty())
            return {};
        const QUrl url(source);
        if (!url.scheme().isEmpty() && !url.isLocalFile())
            return {};
        if (!url.host().isEmpty() || url.hasQuery() || url.hasFragment())
            return {};
        const QString filePath = url.isLocalFile() ? url.toLocalFile()
            : QDir(noteDirectory).filePath(QUrl::fromPercentEncoding(source.toUtf8()));
        const QString absolute = QDir::cleanPath(QFileInfo(filePath).absoluteFilePath());
        if (!isWithin(absolute, noteDirectory) || !safePath(absolute))
            return {};
        QFile file(absolute);
        if (!file.open(QIODevice::ReadOnly) || file.size() > MaximumImageBytes)
            return {};
        bytes = file.readAll();
    }
    if (bytes.isEmpty() || bytes.size() > MaximumImageBytes)
        return {};
    QBuffer input(&bytes);
    input.open(QIODevice::ReadOnly);
    QImageReader reader(&input);
    const QByteArray format = reader.format().toLower();
    if (format != "png" && format != "jpeg" && format != "jpg" && format != "gif" && format != "webp" && format != "bmp")
        return {};
    const QSize size = reader.size();
    if (!size.isValid() || qint64(size.width()) * size.height() > MaximumImagePixels)
        return {};
    const QImage image = reader.read();
    if (image.isNull())
        return {};
    QByteArray png;
    QBuffer output(&png);
    output.open(QIODevice::WriteOnly);
    if (!image.save(&output, "PNG") || png.size() > MaximumImageBytes)
        return {};
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());
}

QString NoteService::safeHtml(const QString& html, const QString& noteDirectory, QString* error) const
{
    IsolatedDocument document;
    document.setHtml(html);
    // 从尾到头替换富文本片段，删除图片时不会改变尚未处理片段的位置。
    /** 独立保存片段位置和格式，避免编辑文档使片段迭代器失效。 */
    struct Fragment {
        int position;           ///< 原片段在文档中的位置。
        int length;             ///< 原片段占用的字符数。
        QTextCharFormat format; ///< 原片段的字符格式。
    };
    QList<Fragment> fragments;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        QTextCursor blockCursor(block);
        QTextBlockFormat blockFormat = block.blockFormat();
        blockFormat.clearProperty(QTextFormat::BackgroundImageUrl);
        blockCursor.setBlockFormat(blockFormat);
        for (auto iterator = block.begin(); !iterator.atEnd(); ++iterator) {
            const QTextFragment fragment = iterator.fragment();
            if (fragment.isValid())
                fragments.append({fragment.position(), fragment.length(), fragment.charFormat()});
        }
    }
    QHash<QString, QString> images;
    for (auto iterator = fragments.crbegin(); iterator != fragments.crend(); ++iterator) {
        QTextCursor cursor(&document);
        cursor.setPosition(iterator->position);
        cursor.setPosition(iterator->position + iterator->length, QTextCursor::KeepAnchor);
        QTextCharFormat format = iterator->format;
        format.setAnchor(false);
        format.setAnchorHref(QString());
        format.setAnchorNames({});
        format.clearProperty(QTextFormat::BackgroundImageUrl);
        if (format.isImageFormat()) {
            QTextImageFormat image = format.toImageFormat();
            if (!images.contains(image.name()))
                images.insert(image.name(), imageDataUrl(image.name(), noteDirectory));
            const QString source = images.value(image.name());
            if (source.isEmpty()) {
                if (error) {
                    *error = QStringLiteral("便签包含无法保存的图片：图片可能损坏、超过 2000 万像素、转换后超过 12 MB，或引用了不允许的外部路径。请替换或缩小图片后重试；原便签尚未修改。");
                    return {};
                }
                cursor.insertText(QStringLiteral("[图片不可用]"), QTextCharFormat());
                continue;
            }
            image.setName(source);
            format = image;
        }
        cursor.setCharFormat(format);
    }
    // 清除根框架、嵌套框架及表格单元格中的背景图片引用。
    QList<QTextFrame*> frames{document.rootFrame()};
    while (!frames.isEmpty()) {
        QTextFrame* frame = frames.takeLast();
        frames.append(frame->childFrames());
        QTextFrameFormat frameFormat = frame->frameFormat();
        frameFormat.clearProperty(QTextFormat::BackgroundImageUrl);
        frame->setFrameFormat(frameFormat);
        if (QTextTable* table = qobject_cast<QTextTable*>(frame)) {
            for (int row = 0; row < table->rows(); ++row) {
                for (int column = 0; column < table->columns(); ++column) {
                    QTextTableCell cell = table->cellAt(row, column);
                    QTextCharFormat cellFormat = cell.format();
                    cellFormat.clearProperty(QTextFormat::BackgroundImageUrl);
                    cell.setFormat(cellFormat);
                }
            }
        }
    }
    return document.toHtml();
}

QVariantMap NoteService::readNote(const QString& id) const
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const int position = notePosition(id);
    if (position < 0)
        return ServiceResult::failure(QStringLiteral("便签不存在。"));
    const Note& note = m_notes.at(position);
    const QString directory = QDir(m_noteRoot).filePath(note.directory);
    const QString path = QDir(directory).filePath(QStringLiteral("index.html"));
    if (!safePath(path))
        return ServiceResult::failure(QStringLiteral("便签路径不安全，已拒绝读取。"));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaximumHtmlBytes)
        return ServiceResult::failure(QStringLiteral("无法读取便签内容，文件可能缺失、过大或不可访问。"));
    QVariantMap result = summary(note);
    result.insert(QStringLiteral("html"), safeHtml(QString::fromUtf8(file.readAll()), directory));
    return ServiceResult::success(result);
}

QVariantMap NoteService::saveNote(const QVariantMap& input)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const QString requestedId = input.value(QStringLiteral("id")).toString();
    const int position = notePosition(requestedId);
    if (!requestedId.isEmpty() && position < 0)
        return ServiceResult::failure(QStringLiteral("便签不存在，无法保存。"));
    const QString title = input.value(QStringLiteral("title")).toString().trimmed();
    if (title.isEmpty() || title.size() > 200)
        return ServiceResult::failure(QStringLiteral("便签标题不能为空，且不能超过 200 个字符。"));
    QString categoryId = input.value(QStringLiteral("categoryId")).toString();
    if (categoryId.isEmpty())
        categoryId = position < 0 ? QStringLiteral("default") : m_notes.at(position).categoryId;
    if (!hasCategory(categoryId))
        return ServiceResult::failure(QStringLiteral("便签分类不存在。"));
    const QString inputHtml = input.value(QStringLiteral("html")).toString();
    if (inputHtml.toUtf8().size() > MaximumHtmlBytes)
        return ServiceResult::failure(QStringLiteral("便签内容过大，无法保存。"));
    QString id = position < 0 ? newId() : requestedId;
    while (position < 0 && QFileInfo::exists(QDir(m_noteRoot).filePath(id)))
        id = newId();
    const QString sourceDirectory = position < 0 ? QString() : QDir(m_noteRoot).filePath(m_notes.at(position).directory);
    QString htmlError;
    const QString html = safeHtml(inputHtml, sourceDirectory, &htmlError);
    if (!htmlError.isEmpty())
        return ServiceResult::failure(htmlError);
    const QByteArray bytes = html.toUtf8();
    if (bytes.size() > MaximumHtmlBytes)
        return ServiceResult::failure(QStringLiteral("便签图片转换后内容过大，无法保存。"));
    const QString path = QDir(m_noteRoot).filePath(id + QStringLiteral("/index.html"));
    if (!safePath(path))
        return ServiceResult::failure(QStringLiteral("便签保存路径不安全。"));
    if (position >= 0 && m_notes.at(position).directory != id
        && QFileInfo::exists(QFileInfo(path).absolutePath())
        && !QDir(QFileInfo(path).absolutePath()).isEmpty())
        return ServiceResult::failure(QStringLiteral("便签迁移目标目录已存在，已停止保存以保护原文件。"));
    Note note{id, categoryId, title, QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs), id};
    QVector<Note> notes = m_notes;
    if (position < 0)
        notes.append(note);
    else
        notes[position] = note;

    // 先准备索引的原子写入，尽早发现只读路径或磁盘错误。
    QString error;
    QSaveFile index(m_indexPath);
    if (!prepareIndex(index, m_categories, notes, error))
        return ServiceResult::failure(error);
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return ServiceResult::failure(QStringLiteral("无法创建便签内容目录。"));
    const bool hadContent = QFileInfo::exists(path);
    QByteArray previous;
    if (hadContent) {
        QFile old(path);
        if (!old.open(QIODevice::ReadOnly) || old.size() > MaximumHtmlBytes)
            return ServiceResult::failure(QStringLiteral("无法备份现有便签内容，已停止保存。"));
        previous = old.readAll();
    }
    QSaveFile content(path);
    content.setDirectWriteFallback(false);
    if (!content.open(QIODevice::WriteOnly) || content.write(bytes) != bytes.size() || !content.commit())
        return ServiceResult::failure(QStringLiteral("无法保存便签内容：") + content.errorString());
    if (!index.commit()) {
        bool restored = true;
        if (hadContent) {
            QSaveFile rollback(path);
            restored = rollback.open(QIODevice::WriteOnly) && rollback.write(previous) == previous.size() && rollback.commit();
        } else {
            // 仅撤销本次新建的文件；不删除旧便签或递归删除目录。
            restored = safePath(path) && QFile::remove(path);
            if (restored)
                QDir().rmdir(QFileInfo(path).absolutePath());
        }
        return ServiceResult::failure(QStringLiteral("无法提交便签索引：") + index.errorString()
            + (restored ? QString() : QStringLiteral("；原内容回滚失败，请保留目录并检查磁盘状态。")));
    }
    m_notes = notes;
    emit changed();
    QVariantMap result = summary(note);
    result.insert(QStringLiteral("html"), html);
    return ServiceResult::success(result);
}

QVariantMap NoteService::removeNotes(const QStringList& ids)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const QSet<QString> selected(ids.begin(), ids.end());
    for (const QString& id : selected) {
        if (notePosition(id) < 0)
            return ServiceResult::failure(QStringLiteral("部分待删除便签不存在，已取消本次删除。"));
    }
    QVector<Note> notes;
    for (const Note& note : m_notes) {
        if (!selected.contains(note.id))
            notes.append(note);
    }
    return commit(m_categories, notes);
}

QVariantMap NoteService::moveNote(const QString& id, const QString& categoryId, const QString& beforeId)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const int position = notePosition(id);
    if (position < 0 || !hasCategory(categoryId))
        return ServiceResult::failure(QStringLiteral("便签或目标分类不存在。"));
    if (beforeId == id)
        return ServiceResult::success();
    if (!beforeId.isEmpty()) {
        const int beforePosition = notePosition(beforeId);
        if (beforePosition < 0 || m_notes.at(beforePosition).categoryId != categoryId)
            return ServiceResult::failure(QStringLiteral("排序目标不在目标分类中。"));
    }
    QVector<Note> notes = m_notes;
    Note note = notes.takeAt(position);
    note.categoryId = categoryId;
    qsizetype insertion = notes.size();
    if (!beforeId.isEmpty()) {
        for (qsizetype index = 0; index < notes.size(); ++index) {
            if (notes.at(index).id == beforeId) {
                insertion = index;
                break;
            }
        }
    }
    notes.insert(insertion, note);
    return commit(m_categories, notes);
}
