#include "TerrainService.h"
#include "ServiceResult.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <QVector3D>
#include <cmath>
#include <cstring>

namespace {
// heightmap.bin 文件头：魔数 "DHM1" + 小端 int32 版本、列数、行数，共 16 字节。
const char HeightMagic[4] = {'D', 'H', 'M', '1'};
constexpr int HeightHeaderBytes = 16;
// 单个项目允许的规模上限：2049×2049 顶点约 16 MB 高度数据。
constexpr int MaximumProjects = 300;
constexpr int MaximumGridSide = 2049;
constexpr qint64 MaximumTotalCells = static_cast<qint64>(MaximumGridSide) * MaximumGridSide;
constexpr qint64 MaximumHeightBytes = HeightHeaderBytes + 4 * MaximumTotalCells;
// 采样间距与地形尺寸的合法范围（米）。
constexpr double MinimumSpacing = 0.1;
constexpr double MaximumSpacing = 100.0;
constexpr double MaximumSize = 16384.0;
constexpr double MaximumInitialHeight = 1000.0;
// 高度数据本身的绝对值上限，超出视为损坏数据。
constexpr float MaximumHeightValue = 10000.0f;
// 撤销步数的合法范围。
constexpr int MinimumUndoSteps = 1;
constexpr int MaximumUndoSteps = 200;
// 流式传输单块最大字节；Base64 后约 1 MB，避免单条 WebChannel 消息过大。
constexpr qint64 StreamChunkBytes = 768 * 1024;
// 同时允许的传输流数量上限，防止内存无限增长。
constexpr int MaximumActiveStreams = 4;
// 预览图与缩略图的最大边长。
constexpr int MaximumPreviewSide = 161;
constexpr int MaximumThumbnailSide = 256;
// 项目标识使用 32 位十六进制字符。
const QRegularExpression stableId(QStringLiteral("^[0-9a-f]{32}$"));

// 将小端字节序列按 Float32 解析为向量；调用方先校验长度。
QVector<float> parseFloatLe(const QByteArray& bytes, int count)
{
    QVector<float> result(count);
    static_assert(sizeof(float) == 4, "需要 4 字节 float");
    for (int index = 0; index < count; ++index) {
        quint32 raw = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(bytes.constData()) + index * 4);
        float value;
        std::memcpy(&value, &raw, 4);
        result[index] = value;
    }
    return result;
}

// 将 Float32 向量编码为小端字节序列。
QByteArray encodeFloatLe(const float* data, int count)
{
    QByteArray bytes;
    bytes.resize(count * 4);
    for (int index = 0; index < count; ++index) {
        quint32 raw;
        std::memcpy(&raw, data + index, 4);
        qToLittleEndian<quint32>(raw, reinterpret_cast<uchar*>(bytes.data()) + index * 4);
    }
    return bytes;
}

// 向指定小端位置写入 int32。
void putInt32Le(QByteArray& bytes, int offset, qint32 value)
{
    qToLittleEndian<qint32>(value, reinterpret_cast<uchar*>(bytes.data()) + offset);
}

// 从小端位置读取 int32。
qint32 getInt32Le(const QByteArray& bytes, int offset)
{
    return qFromLittleEndian<qint32>(reinterpret_cast<const uchar*>(bytes.constData()) + offset);
}
}

TerrainService::TerrainService(const QString& dataRoot, QObject* parent)
    : QObject(parent)
    , m_dataRoot(QDir::cleanPath(QFileInfo(dataRoot).absoluteFilePath()))
    , m_root(QDir(m_dataRoot).filePath(QStringLiteral("terrain")))
    , m_indexPath(QDir(m_root).filePath(QStringLiteral("index.v1.json")))
{
    load();
}

bool TerrainService::safeSegment(const QString& name)
{
    return !name.isEmpty() && name != QStringLiteral(".") && name != QStringLiteral("..")
        && !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\'))
        && !name.contains(QLatin1Char(':')) && !name.contains(QChar::Null)
        && !name.endsWith(QLatin1Char('.')) && !name.endsWith(QLatin1Char(' '));
}

bool TerrainService::safePath(const QString& path) const
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

QString TerrainService::newId()
{
    return QUuid::createUuid().toString(QUuid::Id128);
}

void TerrainService::load()
{
    if (!safePath(m_root) || !safePath(m_indexPath)) {
        m_loadError = QStringLiteral("地形数据路径包含不安全的目录链接，已停止读取和保存。");
        return;
    }
    if (!QFileInfo::exists(m_indexPath))
        return;
    QFile file(m_indexPath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaximumHeightBytes) {
        m_loadError = QStringLiteral("无法读取地形项目索引，已保护原文件，禁止覆盖。");
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    const QJsonObject root = document.object();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || root.value(QStringLiteral("version")).toInt() != 1
        || !root.value(QStringLiteral("items")).isArray()) {
        m_loadError = QStringLiteral("地形项目索引损坏或版本不支持，已保护原文件，禁止覆盖。");
        return;
    }
    QSet<QString> ids;
    for (const QJsonValue& value : root.value(QStringLiteral("items")).toArray()) {
        const QJsonObject object = value.toObject();
        Item item;
        item.id = object.value(QStringLiteral("id")).toString();
        item.title = object.value(QStringLiteral("title")).toString();
        item.updatedAt = object.value(QStringLiteral("updatedAt")).toString();
        item.gridWidth = object.value(QStringLiteral("gridWidth")).toInt();
        item.gridDepth = object.value(QStringLiteral("gridDepth")).toInt();
        item.spacing = object.value(QStringLiteral("spacing")).toDouble();
        item.sizeWidth = object.value(QStringLiteral("sizeWidth")).toDouble();
        item.sizeDepth = object.value(QStringLiteral("sizeDepth")).toDouble();
        item.undoSteps = object.value(QStringLiteral("undoSteps")).toDouble(30);
        if (!value.isObject() || !stableId.match(item.id).hasMatch() || ids.contains(item.id)
            || item.title.trimmed().isEmpty()
            || item.gridWidth < 2 || item.gridWidth > MaximumGridSide
            || item.gridDepth < 2 || item.gridDepth > MaximumGridSide
            || static_cast<qint64>(item.gridWidth) * item.gridDepth > MaximumTotalCells
            || item.spacing < MinimumSpacing || item.spacing > MaximumSpacing) {
            m_loadError = QStringLiteral("地形项目索引包含无效记录，已禁止覆盖。");
            return;
        }
        ids.insert(item.id);
        m_items.append(item);
    }
}

QVariantMap TerrainService::summary(const Item& item)
{
    return {{QStringLiteral("id"), item.id}, {QStringLiteral("title"), item.title},
            {QStringLiteral("updatedAt"), item.updatedAt},
            {QStringLiteral("gridWidth"), item.gridWidth}, {QStringLiteral("gridDepth"), item.gridDepth},
            {QStringLiteral("spacing"), item.spacing},
            {QStringLiteral("sizeWidth"), item.sizeWidth}, {QStringLiteral("sizeDepth"), item.sizeDepth},
            {QStringLiteral("undoSteps"), item.undoSteps}};
}

QVariantMap TerrainService::snapshot() const
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    QVariantList items;
    for (const Item& item : m_items)
        items.append(summary(item));
    return ServiceResult::success(QVariantMap{{QStringLiteral("items"), items},
        {QStringLiteral("warnings"), m_warnings}});
}

int TerrainService::position(const QString& id) const
{
    for (int index = 0; index < m_items.size(); ++index) {
        if (m_items.at(index).id == id)
            return index;
    }
    return -1;
}

QString TerrainService::projectDir(const QString& id) const
{
    return QDir(m_root).filePath(QStringLiteral("projects") + QLatin1Char('/') + id);
}

QString TerrainService::metaPath(const QString& id) const
{
    return QDir(projectDir(id)).filePath(QStringLiteral("project.json"));
}

QString TerrainService::heightsPath(const QString& id) const
{
    return QDir(projectDir(id)).filePath(QStringLiteral("heightmap.bin"));
}

QString TerrainService::thumbnailPath(const QString& id) const
{
    return QDir(projectDir(id)).filePath(QStringLiteral("thumbnail.png"));
}

bool TerrainService::atomicWrite(const QString& path, const QByteArray& bytes, QString* error)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

QVariantMap TerrainService::commitItems(const QVector<Item>& items)
{
    if (!safePath(m_indexPath) || !QDir().mkpath(m_root)) {
        return ServiceResult::failure(QStringLiteral("地形索引路径不安全或目录无法创建。"));
    }
    QJsonArray itemArray;
    for (const Item& item : items)
        itemArray.append(QJsonObject::fromVariantMap(summary(item)));
    const QByteArray bytes = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
        {QStringLiteral("items"), itemArray}}).toJson();
    QString error;
    if (!atomicWrite(m_indexPath, bytes, &error))
        return ServiceResult::failure(QStringLiteral("无法保存地形项目索引：") + error);
    m_items = items;
    emit changed();
    return ServiceResult::success();
}

bool TerrainService::writeProjectMeta(const Item& item, QString* error) const
{
    const QJsonObject meta{{QStringLiteral("formatVersion"), 1},
        {QStringLiteral("name"), item.title},
        {QStringLiteral("gridWidth"), item.gridWidth}, {QStringLiteral("gridDepth"), item.gridDepth},
        {QStringLiteral("spacing"), item.spacing},
        {QStringLiteral("sizeWidth"), item.sizeWidth}, {QStringLiteral("sizeDepth"), item.sizeDepth},
        {QStringLiteral("undoSteps"), item.undoSteps},
        {QStringLiteral("heightUnit"), QStringLiteral("m")},
        {QStringLiteral("heightFormat"), QStringLiteral("float32-le-row-major")},
        {QStringLiteral("updatedAt"), item.updatedAt}};
    return atomicWrite(metaPath(item.id),
        QJsonDocument(meta).toJson(QJsonDocument::Indented), error);
}

QVariantMap TerrainService::create(const QString& title, double widthMeters, double lengthMeters,
                                   double spacing, double initialHeight)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const QString trimmed = title.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > 200)
        return ServiceResult::failure(QStringLiteral("地形项目名称不能为空，且不能超过 200 个字符。"));
    if (m_items.size() >= MaximumProjects)
        return ServiceResult::failure(QStringLiteral("地形项目最多 ") + QString::number(MaximumProjects) + QStringLiteral(" 个。"));
    if (!qIsFinite(widthMeters) || !qIsFinite(lengthMeters) || !qIsFinite(spacing)
        || !qIsFinite(initialHeight))
        return ServiceResult::failure(QStringLiteral("地形尺寸或高度参数无效。"));
    if (spacing < MinimumSpacing || spacing > MaximumSpacing)
        return ServiceResult::failure(QStringLiteral("采样间距需在 ")
            + QString::number(MinimumSpacing) + QStringLiteral(" – ")
            + QString::number(MaximumSpacing) + QStringLiteral(" 米之间。"));
    if (widthMeters < spacing || lengthMeters < spacing || widthMeters > MaximumSize
        || lengthMeters > MaximumSize)
        return ServiceResult::failure(QStringLiteral("地形宽度和长度需在 ")
            + QString::number(spacing) + QStringLiteral(" – ")
            + QString::number(MaximumSize) + QStringLiteral(" 米之间。"));
    if (qAbs(initialHeight) > MaximumInitialHeight)
        return ServiceResult::failure(QStringLiteral("初始地面高度不能超过 1000 米。"));
    // 地形尺寸必须与采样网格一致：顶点数向下取整，实际尺寸随之调整并明确回传。
    const int gridWidth = static_cast<int>(std::floor(widthMeters / spacing + 1e-9)) + 1;
    const int gridDepth = static_cast<int>(std::floor(lengthMeters / spacing + 1e-9)) + 1;
    const double actualWidth = (gridWidth - 1) * spacing;
    const double actualDepth = (gridDepth - 1) * spacing;
    if (static_cast<qint64>(gridWidth) * gridDepth > MaximumTotalCells) {
        const int maxSide = static_cast<int>(std::sqrt(static_cast<double>(MaximumTotalCells)));
        return ServiceResult::failure(QStringLiteral("该尺寸与采样间距组合的网格过大（约 ")
            + QString::number(static_cast<qint64>(gridWidth) * gridDepth)
            + QStringLiteral(" 个采样点），请增大采样间距或缩小地形；单项目最多 ")
            + QString::number(maxSide) + QStringLiteral("×") + QString::number(maxSide)
            + QStringLiteral(" 个采样点。"));
    }

    Item item;
    item.id = newId();
    item.title = trimmed;
    item.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    item.gridWidth = gridWidth;
    item.gridDepth = gridDepth;
    item.spacing = spacing;
    item.sizeWidth = actualWidth;
    item.sizeDepth = actualDepth;
    item.undoSteps = 30;

    // 先写入数据目录，成功后才提交索引，避免索引指向缺失内容的记录。
    const QString dir = projectDir(item.id);
    if (!safePath(dir) && !safePath(dir + QLatin1Char('/'))) {
        return ServiceResult::failure(QStringLiteral("地形项目保存路径不安全。"));
    }
    if (!QDir().mkpath(dir))
        return ServiceResult::failure(QStringLiteral("无法创建地形项目目录：") + dir);
    QVector<float> heights(static_cast<qint64>(gridWidth) * gridDepth,
        static_cast<float>(initialHeight));
    const QByteArray payload = encodeFloatLe(heights.constData(), heights.size());
    // 文件头：魔数 + 版本 + 列数 + 行数，随后是小端 Float32 行主序高度。
    QByteArray fileBytes(HeightHeaderBytes + payload.size(), Qt::Uninitialized);
    std::memcpy(fileBytes.data(), HeightMagic, 4);
    putInt32Le(fileBytes, 4, 1);
    putInt32Le(fileBytes, 8, gridWidth);
    putInt32Le(fileBytes, 12, gridDepth);
    std::memcpy(fileBytes.data() + HeightHeaderBytes, payload.constData(), payload.size());
    QString error;
    if (!atomicWrite(heightsPath(item.id), fileBytes, &error))
        return ServiceResult::failure(QStringLiteral("无法创建高度数据文件：") + error);
    if (!writeProjectMeta(item, &error))
        return ServiceResult::failure(QStringLiteral("无法写入项目元数据：") + error);
    if (!writeThumbnail(item.id, heights, gridWidth, gridDepth, spacing, &error))
        m_warnings.append(QStringLiteral("缩略图生成失败：") + error);

    QVector<Item> items = m_items;
    items.append(item);
    const QVariantMap committed = commitItems(items);
    if (!committed.value(QStringLiteral("ok")).toBool())
        return committed;
    QVariantMap result = summary(item);
    // 创建时把“调整后的实际尺寸”明确回传，避免静默的尺寸误差。
    result.insert(QStringLiteral("adjusted"), actualWidth < widthMeters - 1e-9
        || actualDepth < lengthMeters - 1e-9);
    return ServiceResult::success(result);
}

QVariantMap TerrainService::rename(const QString& id, const QString& title)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const int index = position(id);
    if (index < 0)
        return ServiceResult::failure(QStringLiteral("地形项目不存在。"));
    const QString trimmed = title.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > 200)
        return ServiceResult::failure(QStringLiteral("地形项目名称不能为空，且不能超过 200 个字符。"));
    QVector<Item> items = m_items;
    items[index].title = trimmed;
    items[index].updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QString error;
    if (!writeProjectMeta(items.at(index), &error))
        return ServiceResult::failure(QStringLiteral("无法写入项目元数据：") + error);
    const QVariantMap committed = commitItems(items);
    return committed.value(QStringLiteral("ok")).toBool()
        ? ServiceResult::success(summary(items.at(index))) : committed;
}

QVariantMap TerrainService::remove(const QStringList& ids)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const QSet<QString> selected(ids.begin(), ids.end());
    for (const QString& id : selected) {
        if (position(id) < 0)
            return ServiceResult::failure(QStringLiteral("部分待删除地形项目不存在，已取消本次删除。"));
    }
    QVector<Item> items;
    for (const Item& item : m_items) {
        if (!selected.contains(item.id))
            items.append(item);
    }
    const QVariantMap committed = commitItems(items);
    if (!committed.value(QStringLiteral("ok")).toBool())
        return committed;
    // 索引提交成功后移除数据目录；个别目录删除失败只留下无害的孤立目录。
    for (const QString& id : selected) {
        const QString dir = projectDir(id);
        if (safePath(dir))
            QDir(dir).removeRecursively();
    }
    return ServiceResult::success();
}

QVariantMap TerrainService::readMeta(const QString& id) const
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const int index = position(id);
    if (index < 0)
        return ServiceResult::failure(QStringLiteral("地形项目不存在。"));
    return ServiceResult::success(summary(m_items.at(index)));
}

bool TerrainService::readProjectMeta(const QString& id, Item* item, QString* error) const
{
    QFile file(metaPath(id));
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("无法读取项目元数据文件。");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    const QJsonObject root = document.object();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || root.value(QStringLiteral("formatVersion")).toInt() != 1) {
        *error = QStringLiteral("项目元数据文件损坏或版本不支持。");
        return false;
    }
    item->title = root.value(QStringLiteral("name")).toString();
    item->gridWidth = root.value(QStringLiteral("gridWidth")).toInt();
    item->gridDepth = root.value(QStringLiteral("gridDepth")).toInt();
    item->spacing = root.value(QStringLiteral("spacing")).toDouble();
    item->sizeWidth = root.value(QStringLiteral("sizeWidth")).toDouble();
    item->sizeDepth = root.value(QStringLiteral("sizeDepth")).toDouble();
    item->undoSteps = root.value(QStringLiteral("undoSteps")).toDouble(30);
    if (item->gridWidth < 2 || item->gridDepth < 2
        || static_cast<qint64>(item->gridWidth) * item->gridDepth > MaximumTotalCells) {
        *error = QStringLiteral("项目元数据中的网格参数无效。");
        return false;
    }
    return true;
}

bool TerrainService::validateHeights(const QByteArray& bytes, int gridWidth, int gridDepth,
                                     QVector<float>* out, QString* error) const
{
    const qint64 expected = static_cast<qint64>(gridWidth) * gridDepth;
    if (bytes.size() != expected * 4) {
        *error = QStringLiteral("高度数据大小与网格尺寸不一致。");
        return false;
    }
    QVector<float> heights = parseFloatLe(bytes, static_cast<int>(expected));
    for (float value : heights) {
        if (!qIsFinite(value)) {
            *error = QStringLiteral("高度数据包含非法数值（NaN 或无穷大），已取消保存。");
            return false;
        }
        if (qAbs(value) > MaximumHeightValue) {
            *error = QStringLiteral("高度数据超出 ±10000 米的安全范围，已取消保存。");
            return false;
        }
    }
    if (out)
        *out = heights;
    return true;
}

QByteArray TerrainService::readHeightFile(const QString& id, QString* error) const
{
    const int index = position(id);
    if (index < 0) {
        *error = QStringLiteral("地形项目不存在。");
        return {};
    }
    const QString path = heightsPath(id);
    if (!safePath(path)) {
        *error = QStringLiteral("高度数据路径不安全，已拒绝读取。");
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < HeightHeaderBytes
        || file.size() > MaximumHeightBytes) {
        *error = QStringLiteral("无法读取高度数据，文件可能缺失、过大或不可访问。");
        return {};
    }
    const QByteArray bytes = file.readAll();
    if (!bytes.startsWith(QByteArray(HeightMagic, 4)) || getInt32Le(bytes, 4) != 1) {
        *error = QStringLiteral("高度数据文件头无效或版本不支持。");
        return {};
    }
    const qint32 gridWidth = getInt32Le(bytes, 8);
    const qint32 gridDepth = getInt32Le(bytes, 12);
    if (gridWidth != m_items.at(index).gridWidth || gridDepth != m_items.at(index).gridDepth
        || bytes.size() != HeightHeaderBytes + 4 * static_cast<qint64>(gridWidth) * gridDepth) {
        *error = QStringLiteral("高度数据与项目元数据不一致，文件可能已损坏。");
        return {};
    }
    return bytes;
}

QVariantMap TerrainService::readPreview(const QString& id, int maxSamples) const
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const int index = position(id);
    if (index < 0)
        return ServiceResult::failure(QStringLiteral("地形项目不存在。"));
    QString error;
    const QByteArray bytes = readHeightFile(id, &error);
    if (bytes.isEmpty())
        return ServiceResult::failure(error);
    const Item& item = m_items.at(index);
    QVector<float> heights;
    if (!validateHeights(bytes.mid(HeightHeaderBytes), item.gridWidth, item.gridDepth,
                         &heights, &error))
        return ServiceResult::failure(error);
    // 盒式降采样到预览网格，保持原始高度含义，不引入插值细节。
    const int side = qBound(16, maxSamples, MaximumPreviewSide);
    const double scale = qMin(1.0, static_cast<double>(side)
        / static_cast<double>(qMax(item.gridWidth, item.gridDepth)));
    const int outWidth = qMax(2, static_cast<int>(std::lround(item.gridWidth * scale)));
    const int outDepth = qMax(2, static_cast<int>(std::lround(item.gridDepth * scale)));
    QVector<float> preview(static_cast<qint64>(outWidth) * outDepth);
    for (int z = 0; z < outDepth; ++z) {
        const int srcZ0 = static_cast<int>(static_cast<qint64>(z) * item.gridDepth / outDepth);
        const int srcZ1 = qMin(item.gridDepth,
            static_cast<int>(static_cast<qint64>(z + 1) * item.gridDepth / outDepth));
        for (int x = 0; x < outWidth; ++x) {
            const int srcX0 = static_cast<int>(static_cast<qint64>(x) * item.gridWidth / outWidth);
            const int srcX1 = qMin(item.gridWidth,
                static_cast<int>(static_cast<qint64>(x + 1) * item.gridWidth / outWidth));
            double sum = 0;
            int count = 0;
            for (int sz = srcZ0; sz < qMax(srcZ1, srcZ0 + 1); ++sz) {
                for (int sx = srcX0; sx < qMax(srcX1, srcX0 + 1); ++sx) {
                    sum += heights.at(static_cast<qint64>(sz) * item.gridWidth + sx);
                    ++count;
                }
            }
            preview[static_cast<qint64>(z) * outWidth + x] = static_cast<float>(sum / qMax(1, count));
        }
    }
    return ServiceResult::success(QVariantMap{
        {QStringLiteral("gridWidth"), outWidth}, {QStringLiteral("gridDepth"), outDepth},
        {QStringLiteral("sizeWidth"), item.sizeWidth}, {QStringLiteral("sizeDepth"), item.sizeDepth},
        {QStringLiteral("title"), item.title},
        {QStringLiteral("heights"), encodeFloatLe(preview.constData(), preview.size())}});
}

QVariantMap TerrainService::readThumbnail(const QString& id) const
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    if (position(id) < 0)
        return ServiceResult::failure(QStringLiteral("地形项目不存在。"));
    const QString path = thumbnailPath(id);
    if (!safePath(path) || !QFileInfo::exists(path))
        return ServiceResult::success(QVariantMap{{QStringLiteral("image"), QByteArray()}});
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return ServiceResult::success(QVariantMap{{QStringLiteral("image"), QByteArray()}});
    return ServiceResult::success(QVariantMap{{QStringLiteral("image"), file.readAll()}});
}

bool TerrainService::writeThumbnail(const QString& id, const QVector<float>& heights,
                                    int gridWidth, int gridDepth, double spacing, QString* error) const
{
    // 由高度场生成灰度晕染图：法线朝向固定光源，明暗反映坡向与坡度。
    const double scaleX = static_cast<double>(MaximumThumbnailSide) / gridWidth;
    const double scaleZ = static_cast<double>(MaximumThumbnailSide) / gridDepth;
    const double scale = qMin(1.0, qMin(scaleX, scaleZ));
    const int outWidth = qMax(2, static_cast<int>(std::lround(gridWidth * scale)));
    const int outDepth = qMax(2, static_cast<int>(std::lround(gridDepth * scale)));
    QImage image(outWidth, outDepth, QImage::Format_Grayscale8);
    const auto sample = [&](int x, int z) {
        x = qBound(0, x, gridWidth - 1);
        z = qBound(0, z, gridDepth - 1);
        return static_cast<double>(heights.at(static_cast<qint64>(z) * gridWidth + x));
    };
    const QVector3D light = QVector3D(-0.45f, 0.78f, 0.44f).normalized();
    for (int z = 0; z < outDepth; ++z) {
        uchar* line = image.scanLine(z);
        for (int x = 0; x < outWidth; ++x) {
            const int ix = static_cast<int>(std::lround(
                static_cast<double>(x) / outWidth * (gridWidth - 1)));
            const int iz = static_cast<int>(std::lround(
                static_cast<double>(z) / outDepth * (gridDepth - 1)));
            // 以世界米为单位的一阶差分，得到地表法线。
            const double dx = (sample(ix + 1, iz) - sample(ix - 1, iz)) / (2.0 * spacing);
            const double dz = (sample(ix, iz + 1) - sample(ix, iz - 1)) / (2.0 * spacing);
            const QVector3D normal = QVector3D(
                static_cast<float>(-dx), 1.0f, static_cast<float>(-dz)).normalized();
            const double diffuse = qBound(0.0, static_cast<double>(
                QVector3D::dotProduct(normal, light)), 1.0);
            const double shade = qBound(0.0, 0.5 + 0.5 * diffuse, 1.0);
            line[x] = static_cast<uchar>(std::lround(shade * 255.0));
        }
    }
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) {
        *error = QStringLiteral("缩略图编码失败。");
        return false;
    }
    return atomicWrite(thumbnailPath(id), png, error);
}

QVariantMap TerrainService::openHeightStream(const QString& id)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    if (m_streams.size() + m_exports.size() >= MaximumActiveStreams)
        return ServiceResult::failure(QStringLiteral("存在未完成的传输任务，请稍后重试或重新打开窗口。"));
    QString error;
    const QByteArray bytes = readHeightFile(id, &error);
    if (bytes.isEmpty())
        return ServiceResult::failure(error);
    const Item& item = m_items.at(position(id));
    QString token = newId();
    HeightStream stream;
    stream.projectId = id;
    stream.buffer = bytes;
    stream.expected = bytes.size();
    stream.chunkSize = static_cast<int>(StreamChunkBytes);
    stream.write = false;
    m_streams.insert(token, stream);
    return ServiceResult::success(QVariantMap{{QStringLiteral("token"), token},
        {QStringLiteral("bytes"), stream.expected}, {QStringLiteral("chunkSize"), stream.chunkSize},
        {QStringLiteral("gridWidth"), item.gridWidth}, {QStringLiteral("gridDepth"), item.gridDepth},
        {QStringLiteral("spacing"), item.spacing},
        {QStringLiteral("sizeWidth"), item.sizeWidth}, {QStringLiteral("sizeDepth"), item.sizeDepth},
        {QStringLiteral("undoSteps"), item.undoSteps}, {QStringLiteral("title"), item.title}});
}

QByteArray TerrainService::decodeChunk(const QString& base64, int maxSize)
{
    if (base64.isEmpty() || base64.size() > maxSize * 2)
        return {};
    const QByteArray decoded = QByteArray::fromBase64(base64.toLatin1());
    if (decoded.size() > maxSize)
        return {};
    return decoded;
}

QVariantMap TerrainService::readHeightChunk(const QString& token, int index)
{
    const auto it = m_streams.constFind(token);
    if (it == m_streams.constEnd() || it->write)
        return ServiceResult::failure(QStringLiteral("传输会话无效或已过期，请重新打开项目。"));
    const qint64 offset = static_cast<qint64>(qMax(0, index)) * it->chunkSize;
    if (offset >= it->expected)
        return ServiceResult::failure(QStringLiteral("分块索引超出数据范围。"));
    const int size = static_cast<int>(qMin<qint64>(it->chunkSize, it->expected - offset));
    const QByteArray chunk = it->buffer.mid(static_cast<int>(offset), size);
    return ServiceResult::success(QVariantMap{{QStringLiteral("index"), index},
        {QStringLiteral("bytes"), size}, {QStringLiteral("data"), QString::fromLatin1(chunk.toBase64())}});
}

QVariantMap TerrainService::beginHeightWrite(const QString& id, int gridWidth, int gridDepth,
                                             double spacing, double undoSteps)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const int index = position(id);
    if (index < 0)
        return ServiceResult::failure(QStringLiteral("地形项目不存在，无法保存。"));
    if (m_streams.size() + m_exports.size() >= MaximumActiveStreams)
        return ServiceResult::failure(QStringLiteral("存在未完成的传输任务，请稍后重试。"));
    const Item& item = m_items.at(index);
    if (gridWidth < 2 || gridDepth < 2 || gridWidth > MaximumGridSide || gridDepth > MaximumGridSide
        || static_cast<qint64>(gridWidth) * gridDepth > MaximumTotalCells)
        return ServiceResult::failure(QStringLiteral("保存的网格参数超出允许范围。"));
    if (!qIsFinite(spacing) || spacing < MinimumSpacing || spacing > MaximumSpacing)
        return ServiceResult::failure(QStringLiteral("采样间距参数无效。"));
    const double sizeWidth = (gridWidth - 1) * spacing;
    const double sizeDepth = (gridDepth - 1) * spacing;
    // 重采样允许实际尺寸因向下取整缩小一个采样间距以内，不允许放大或明显缩短。
    if (qAbs(sizeWidth - item.sizeWidth) > spacing + 0.02
        || qAbs(sizeDepth - item.sizeDepth) > spacing + 0.02)
        return ServiceResult::failure(QStringLiteral("保存的尺寸与项目实际尺寸不一致，已取消保存。"));
    // 网页只传输高度数据本身；文件头由服务端在提交时按网格参数生成。
    const qint64 totalBytes = 4 * static_cast<qint64>(gridWidth) * gridDepth;
    QString token = newId();
    HeightStream stream;
    stream.projectId = id;
    stream.expected = totalBytes;
    stream.chunkSize = static_cast<int>(StreamChunkBytes);
    stream.gridWidth = gridWidth;
    stream.gridDepth = gridDepth;
    stream.spacing = spacing;
    stream.undoSteps = qBound(MinimumUndoSteps, static_cast<int>(std::lround(undoSteps)),
        MaximumUndoSteps);
    stream.write = true;
    m_streams.insert(token, stream);
    return ServiceResult::success(QVariantMap{{QStringLiteral("token"), token},
        {QStringLiteral("bytes"), totalBytes}, {QStringLiteral("chunkSize"), stream.chunkSize}});
}

QVariantMap TerrainService::writeHeightChunk(const QString& token, int index, const QString& base64)
{
    const auto it = m_streams.find(token);
    if (it == m_streams.end() || !it->write)
        return ServiceResult::failure(QStringLiteral("保存会话无效或已过期，请重新保存。"));
    if (index != it->receivedChunks)
        return ServiceResult::failure(QStringLiteral("分块顺序错误，已取消保存。"));
    const QByteArray chunk = decodeChunk(base64, it->chunkSize);
    if (chunk.isEmpty() || it->buffer.size() + static_cast<qint64>(chunk.size()) > it->expected)
        return ServiceResult::failure(QStringLiteral("分块数据无效，已取消保存。"));
    it->buffer.append(chunk);
    it->receivedChunks += 1;
    return ServiceResult::success();
}

QVariantMap TerrainService::commitHeightWrite(const QString& token)
{
    const auto it = m_streams.find(token);
    if (it == m_streams.end() || !it->write)
        return ServiceResult::failure(QStringLiteral("保存会话无效或已过期。"));
    HeightStream stream = *it;
    m_streams.erase(it);
    const int index = position(stream.projectId);
    if (index < 0)
        return ServiceResult::failure(QStringLiteral("地形项目不存在，无法保存。"));
    if (stream.buffer.size() != stream.expected)
        return ServiceResult::failure(QStringLiteral("高度数据不完整，已取消保存。"));
    const qint32 gridWidth = stream.gridWidth;
    const qint32 gridDepth = stream.gridDepth;
    QVector<float> heights;
    QString error;
    if (!validateHeights(stream.buffer, gridWidth, gridDepth, &heights, &error))
        return ServiceResult::failure(error);
    // 服务端构造文件头：魔数 + 版本 + 列数 + 行数。
    QByteArray fileBytes(HeightHeaderBytes + stream.buffer.size(), Qt::Uninitialized);
    std::memcpy(fileBytes.data(), HeightMagic, 4);
    putInt32Le(fileBytes, 4, 1);
    putInt32Le(fileBytes, 8, gridWidth);
    putInt32Le(fileBytes, 12, gridDepth);
    std::memcpy(fileBytes.data() + HeightHeaderBytes, stream.buffer.constData(), stream.buffer.size());
    stream.buffer = fileBytes;

    Item item = m_items.at(index);
    item.gridWidth = gridWidth;
    item.gridDepth = gridDepth;
    // 采样间距与撤销步数由写入会话在 beginHeightWrite 中记录，避免反推误差。
    item.spacing = stream.spacing;
    item.undoSteps = stream.undoSteps;
    item.sizeWidth = (gridWidth - 1) * item.spacing;
    item.sizeDepth = (gridDepth - 1) * item.spacing;
    item.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);

    // 先替换高度数据，再更新元数据与索引；后两者失败时高度文件已是完整有效数据。
    if (!atomicWrite(heightsPath(item.id), stream.buffer, &error))
        return ServiceResult::failure(QStringLiteral("无法写入高度数据：") + error);
    if (!writeProjectMeta(item, &error))
        return ServiceResult::failure(QStringLiteral("高度数据已保存，但项目元数据写入失败：") + error);
    if (!writeThumbnail(item.id, heights, gridWidth, gridDepth, item.spacing, &error))
        m_warnings.append(QStringLiteral("缩略图更新失败：") + error);
    QVector<Item> items = m_items;
    items[index] = item;
    const QVariantMap committed = commitItems(items);
    if (!committed.value(QStringLiteral("ok")).toBool())
        return committed;
    return ServiceResult::success(QVariantMap{{QStringLiteral("updatedAt"), item.updatedAt},
        {QStringLiteral("gridWidth"), item.gridWidth}, {QStringLiteral("gridDepth"), item.gridDepth},
        {QStringLiteral("sizeWidth"), item.sizeWidth}, {QStringLiteral("sizeDepth"), item.sizeDepth}});
}

QVariantMap TerrainService::cancelStream(const QString& token)
{
    m_streams.remove(token);
    m_exports.remove(token);
    return ServiceResult::success();
}

QVariantMap TerrainService::beginExport(const QString& id, const QString& filePath, int pngWidth,
                                        int pngHeight, double spacing, double minHeight,
                                        double maxHeight)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const int index = position(id);
    if (index < 0)
        return ServiceResult::failure(QStringLiteral("地形项目不存在，无法导出。"));
    if (m_streams.size() + m_exports.size() >= MaximumActiveStreams)
        return ServiceResult::failure(QStringLiteral("存在未完成的传输任务，请稍后重试。"));
    const Item& item = m_items.at(index);
    const QString path = QDir::cleanPath(QFileInfo(filePath).absoluteFilePath());
    if (path.isEmpty() || !path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
        return ServiceResult::failure(QStringLiteral("导出路径必须指向一个 .png 文件。"));
    if (pngWidth < 2 || pngHeight < 2
        || static_cast<qint64>(pngWidth) * pngHeight > MaximumTotalCells)
        return ServiceResult::failure(QStringLiteral("导出分辨率超出允许范围。"));
    // 导出采样间距必须不小于编辑采样间距，不允许宣称比编辑数据更高的精度。
    if (!qIsFinite(spacing) || spacing < item.spacing - 1e-9)
        return ServiceResult::failure(QStringLiteral("导出采样间距不能小于当前编辑采样间距。"));
    if (!qIsFinite(minHeight) || !qIsFinite(maxHeight) || minHeight > maxHeight)
        return ServiceResult::failure(QStringLiteral("高度映射范围无效。"));
    QString token = newId();
    ExportStream stream;
    stream.projectId = id;
    stream.filePath = path;
    stream.pngWidth = pngWidth;
    stream.pngHeight = pngHeight;
    stream.spacing = spacing;
    stream.minHeight = minHeight;
    stream.maxHeight = maxHeight;
    stream.expected = static_cast<qint64>(pngWidth) * pngHeight * 2;
    stream.chunkSize = static_cast<int>(StreamChunkBytes);
    m_exports.insert(token, stream);
    return ServiceResult::success(QVariantMap{{QStringLiteral("token"), token},
        {QStringLiteral("bytes"), stream.expected},
        {QStringLiteral("chunkSize"), stream.chunkSize},
        {QStringLiteral("projectName"), item.title}});
}

QVariantMap TerrainService::writeExportChunk(const QString& token, int index, const QString& base64)
{
    const auto it = m_exports.find(token);
    if (it == m_exports.end())
        return ServiceResult::failure(QStringLiteral("导出会话无效或已过期，请重新导出。"));
    if (index != it->receivedChunks)
        return ServiceResult::failure(QStringLiteral("导出分块顺序错误，已取消导出。"));
    const QByteArray chunk = decodeChunk(base64, it->chunkSize);
    if (chunk.isEmpty() || it->buffer.size() + static_cast<qint64>(chunk.size()) > it->expected)
        return ServiceResult::failure(QStringLiteral("导出分块数据无效，已取消导出。"));
    it->buffer.append(chunk);
    it->receivedChunks += 1;
    return ServiceResult::success();
}

bool TerrainService::parsePngHeader(const QByteArray& bytes, int* width, int* height,
                                    int* bitDepth, int* colorType)
{
    static const uchar signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (bytes.size() < 33 || std::memcmp(bytes.constData(), signature, 8) != 0)
        return false;
    const uchar* ihdr = reinterpret_cast<const uchar*>(bytes.constData()) + 12;
    if (std::memcmp(ihdr, "IHDR", 4) != 0)
        return false;
    // IHDR 数据区：宽 [4..7]、高 [8..11]、位深 [12]、颜色类型 [13]。
    const quint32 w = (quint32(ihdr[4]) << 24) | (quint32(ihdr[5]) << 16)
        | (quint32(ihdr[6]) << 8) | quint32(ihdr[7]);
    const quint32 h = (quint32(ihdr[8]) << 24) | (quint32(ihdr[9]) << 16)
        | (quint32(ihdr[10]) << 8) | quint32(ihdr[11]);
    *width = static_cast<int>(w);
    *height = static_cast<int>(h);
    *bitDepth = ihdr[12];
    *colorType = ihdr[13];
    return true;
}

QVariantMap TerrainService::commitExport(const QString& token)
{
    const auto it = m_exports.find(token);
    if (it == m_exports.end())
        return ServiceResult::failure(QStringLiteral("导出会话无效或已过期。"));
    ExportStream stream = *it;
    m_exports.erase(it);
    const int index = position(stream.projectId);
    if (index < 0)
        return ServiceResult::failure(QStringLiteral("地形项目不存在，无法导出。"));
    const Item& item = m_items.at(index);
    if (stream.buffer.size() != stream.expected)
        return ServiceResult::failure(QStringLiteral("导出数据不完整，已取消导出。"));

    // 以 16 位灰度图像逐行写入；数据按小端 Uint16 传输，按本机字节序还原。
    QImage image(stream.pngWidth, stream.pngHeight, QImage::Format_Grayscale16);
    if (image.isNull())
        return ServiceResult::failure(QStringLiteral("无法为该分辨率分配导出图像内存。"));
    const uchar* source = reinterpret_cast<const uchar*>(stream.buffer.constData());
    for (int row = 0; row < stream.pngHeight; ++row) {
        uchar* line = image.scanLine(row);
        for (int column = 0; column < stream.pngWidth; ++column) {
            const quint16 value = qFromLittleEndian<quint16>(
                source + (static_cast<qint64>(row) * stream.pngWidth + column) * 2);
            std::memcpy(line + column * 2, &value, 2);
        }
    }
    QString error;
    QByteArray png;
    QBuffer pngBuffer(&png);
    pngBuffer.open(QIODevice::WriteOnly);
    if (!image.save(&pngBuffer, "PNG"))
        return ServiceResult::failure(QStringLiteral("PNG 编码失败，已取消导出。"));
    int headerWidth = 0;
    int headerHeight = 0;
    int bitDepth = 0;
    int colorType = 0;
    // 独立回读校验：直接解析 PNG 头确认位深与颜色类型，再用 QImage 解码比对全部样本值。
    if (!parsePngHeader(png, &headerWidth, &headerHeight, &bitDepth, &colorType))
        return ServiceResult::failure(QStringLiteral("导出的 PNG 文件头校验失败，已取消导出。"));
    if (bitDepth != 16 || colorType != 0 || headerWidth != stream.pngWidth
        || headerHeight != stream.pngHeight) {
        return ServiceResult::failure(QStringLiteral("导出的 PNG 不是 16 位单通道灰度图，已取消导出。"));
    }
    QImage decoded;
    if (!decoded.loadFromData(png, "PNG") || decoded.format() != QImage::Format_Grayscale16
        || decoded.width() != stream.pngWidth || decoded.height() != stream.pngHeight) {
        return ServiceResult::failure(QStringLiteral("导出的 PNG 无法按 16 位灰度格式解码，已取消导出。"));
    }
    qint64 mismatches = 0;
    for (int row = 0; row < stream.pngHeight && mismatches == 0; ++row) {
        const uchar* line = decoded.constScanLine(row);
        for (int column = 0; column < stream.pngWidth; ++column) {
            const quint16 expected = qFromLittleEndian<quint16>(
                source + (static_cast<qint64>(row) * stream.pngWidth + column) * 2);
            quint16 actual;
            std::memcpy(&actual, line + column * 2, 2);
            if (actual != expected) {
                mismatches += 1;
                break;
            }
        }
    }
    if (mismatches > 0)
        return ServiceResult::failure(QStringLiteral("导出的 PNG 样本值与高度数据不一致，已取消导出。"));

    if (!atomicWrite(stream.filePath, png, &error))
        return ServiceResult::failure(QStringLiteral("无法写入 PNG 文件：") + error);

    // 同名 JSON 说明文件：记录分辨率、映射范围、方向与 UE 导入建议。
    const QString jsonPath = stream.filePath.left(stream.filePath.size() - 4) + QStringLiteral(".json");
    QVector<qint64> validSizes;
    for (qint64 size = 63; size <= 8129; size *= 2)
        validSizes.append(size);
    qint64 suggested = 0;
    for (qint64 candidate : validSizes) {
        if (candidate + 1 >= stream.pngWidth) {
            suggested = candidate + 1;
            break;
        }
    }
    if (suggested == 0)
        suggested = 8129;
    const QJsonObject verification{{QStringLiteral("bitDepth"), bitDepth},
        {QStringLiteral("colorType"), colorType},
        {QStringLiteral("width"), headerWidth}, {QStringLiteral("height"), headerHeight},
        {QStringLiteral("sampleMatch"), true}};
    const QJsonObject unreal{{QStringLiteral("note"),
            QStringLiteral("UE Landscape 仅接受特定分辨率（组件数×63+1，如 505、1009、2017）。"
                           "当前分辨率不一致时，请在 UE 中选择最接近的合法尺寸或调整导出采样间距后重新导出。")},
        {QStringLiteral("currentResolution"), stream.pngWidth},
        {QStringLiteral("suggestedResolution"), static_cast<int>(suggested)},
        {QStringLiteral("commonValidResolutions"), QJsonArray{63, 127, 253, 505, 1009, 2017, 4033, 8129}}};
    const QJsonObject document{{QStringLiteral("formatVersion"), 1},
        {QStringLiteral("kind"), QStringLiteral("terrain-heightmap")},
        {QStringLiteral("projectName"), item.title},
        {QStringLiteral("terrainSizeMeters"), QJsonObject{{QStringLiteral("width"), item.sizeWidth},
            {QStringLiteral("length"), item.sizeDepth}}},
        {QStringLiteral("exportSpacingMeters"), stream.spacing},
        {QStringLiteral("pngWidth"), stream.pngWidth}, {QStringLiteral("pngHeight"), stream.pngHeight},
        {QStringLiteral("heightRangeMeters"), QJsonObject{{QStringLiteral("min"), stream.minHeight},
            {QStringLiteral("max"), stream.maxHeight}}},
        {QStringLiteral("heightUnit"), QStringLiteral("m")},
        {QStringLiteral("mapping"), QJsonObject{{QStringLiteral("black"), QStringLiteral("最低高度")},
            {QStringLiteral("white"), QStringLiteral("最高高度")}, {QStringLiteral("linear"), true},
            {QStringLiteral("bitDepth"), 16}, {QStringLiteral("grayscale"), true},
            {QStringLiteral("channels"), 1},
            {QStringLiteral("byteOrder"), QStringLiteral("PNG 标准 16 位大端样本")}}},
        {QStringLiteral("orientation"), QJsonObject{{QStringLiteral("rows"),
                QStringLiteral("图像第一行对应最小 Z（北侧），向下 Z 递增")},
            {QStringLiteral("columns"), QStringLiteral("图像第一列对应最小 X（西侧），向右 X 递增")},
            {QStringLiteral("heightAxis"), QStringLiteral("高度向上为正，单位米")}}},
        {QStringLiteral("unreal"), unreal},
        {QStringLiteral("verification"), verification}};
    if (!atomicWrite(jsonPath, QJsonDocument(document).toJson(QJsonDocument::Indented), &error))
        return ServiceResult::failure(QStringLiteral("PNG 已导出，但说明文件写入失败：") + error);

    return ServiceResult::success(QVariantMap{{QStringLiteral("pngPath"), stream.filePath},
        {QStringLiteral("jsonPath"), jsonPath},
        {QStringLiteral("pngWidth"), stream.pngWidth}, {QStringLiteral("pngHeight"), stream.pngHeight},
        {QStringLiteral("spacing"), stream.spacing},
        {QStringLiteral("minHeight"), stream.minHeight}, {QStringLiteral("maxHeight"), stream.maxHeight},
        {QStringLiteral("bitDepth"), bitDepth}, {QStringLiteral("colorType"), colorType}});
}
