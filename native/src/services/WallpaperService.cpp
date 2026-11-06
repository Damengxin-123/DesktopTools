#include "WallpaperService.h"
#include "ServiceResult.h"
#include "app/WallpaperWindow.h"

#include <QBuffer>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QGuiApplication>
#include <QScreen>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QPointer>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <algorithm>

namespace
{
// 限制历史数量与索引体积；缩略图随索引保存。
constexpr int MaximumItems = 100;
constexpr qint64 MaximumIndexBytes = 64 * 1024 * 1024;
constexpr qint64 MaximumImagePixels = 50000000;
constexpr qint64 MaximumThumbnailBytes = 256 * 1024;
// 列表缩略图的最长边，与表情库保持一致。
constexpr int ThumbnailSize = 200;
// 缩略图固定为 Qt 编码的 PNG 数据地址，与网页 CSP 一致。
const QString ThumbnailPrefix = QStringLiteral("data:image/png;base64,");
// 常见视频扩展名；其余可读文件按图片处理。
const QStringList VideoSuffixes = {QStringLiteral("mp4"), QStringLiteral("webm"),
    QStringLiteral("mkv"), QStringLiteral("avi"), QStringLiteral("mov"), QStringLiteral("m4v"),
    QStringLiteral("wmv"), QStringLiteral("mpg"), QStringLiteral("mpeg"), QStringLiteral("flv"),
    QStringLiteral("ts"), QStringLiteral("3gp")};
// 守护定时器的检查间隔，桌面重建后在此周期内恢复挂载。
constexpr int WatchdogIntervalMs = 3000;
// 图片显示方式白名单，前端和持久化数据均不能绕过校验。
const QStringList DisplayModes = {QStringLiteral("fill"), QStringLiteral("fit"), QStringLiteral("stretch"),
    QStringLiteral("tile"), QStringLiteral("center")};

// 只读取图片尺寸元数据，并按 EXIF 旋转调整宽高，用于真实像素比例预览。
QSize imageSize(const QString& path)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QSize size = reader.size();
    if (reader.transformation() & QImageIOHandler::TransformationRotate90)
        size.transpose();
    return size.isValid() ? size : QSize(0, 0);
}

// 生成新记录的稳定标识。
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
}

WallpaperService::WallpaperService(const QString& dataRoot, bool nativeIntegration, QObject* parent,
    std::function<QVariantList()> screenProvider)
    : QObject(parent), m_path(QDir(dataRoot).absoluteFilePath(QStringLiteral("wallpaper/wallpaper-history.v1.json"))),
      m_screenProvider(screenProvider ? std::move(screenProvider) : WallpaperWindow::screens),
      m_nativeIntegration(nativeIntegration),
      m_watchdog(new QTimer(this))
{
    m_watchdog->setInterval(WatchdogIntervalMs);
    connect(m_watchdog, &QTimer::timeout, this, &WallpaperService::ensureEngineAlive);
    m_screens = m_screenProvider();
    // 即使关闭壁纸也刷新屏幕列表；信号处理热插拔，定时器补充原生分辨率与布局变化。
    m_watchdog->start();
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [this](QScreen*) { refreshScreens(); });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen*) { refreshScreens(); });
    load();
    // 启动时已开启且资源仍在，自动恢复上次的壁纸显示。
    if (m_enabled)
        startEngine();
}

WallpaperService::~WallpaperService()
{
    stopEngine();
}

void WallpaperService::load()
{
    if (!QFileInfo::exists(m_path))
        return;
    QFile file(m_path);
    m_loadError = QStringLiteral("壁纸历史无法读取或格式损坏，已停止写入以保护原文件。请检查数据目录中的 wallpaper/wallpaper-history.v1.json。");
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaximumIndexBytes)
        return;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    const auto root = document.object();
    const int version = root.value("version").toInt();
    if (error.error != QJsonParseError::NoError || (version != 1 && version != 2)
        || !root.value("items").isArray() || !root.value("enabled").isBool()
        || root.value("items").toArray().size() > MaximumItems)
        return;
    QVector<Item> items;
    for (const auto& value : root.value("items").toArray()) {
        const auto item = value.toObject();
        if (!value.isObject() || !item.value("id").isString() || !item.value("name").isString()
            || !item.value("path").isString() || !item.value("type").isString()
            || !item.value("animated").isBool() || !item.value("addedAt").isString()
            || !item.value("lastUsedAt").isString() || !item.value("thumbnail").isString())
            return;
        Item entry;
        entry.id = item.value("id").toString();
        entry.name = item.value("name").toString();
        entry.path = item.value("path").toString();
        entry.type = item.value("type").toString();
        entry.animated = item.value("animated").toBool();
        entry.addedAt = item.value("addedAt").toString();
        entry.lastUsedAt = item.value("lastUsedAt").toString();
        entry.thumbnail = item.value("thumbnail").toString();
        const QSize size = entry.type == QStringLiteral("image") ? imageSize(entry.path) : QSize(0, 0);
        entry.width = size.width() > 0 ? size.width() : item.value("width").toInt();
        entry.height = size.height() > 0 ? size.height() : item.value("height").toInt();
        items.append(entry);
    }
    QMap<QString, QString> assignments;
    if (version == 1) {
        if (!root.value("activeId").isString())
            return;
        // 旧版跨屏壁纸迁移为各屏独立选择，首次升级保留原显示内容。
        const QString activeId = root.value("activeId").toString();
        if (!validate(items, false, activeId).isEmpty())
            return;
        for (const QVariant& screen : m_screens)
            assignments.insert(screen.toMap().value("id").toString(), activeId);
    } else {
        if (!root.value("screens").isObject())
            return;
        const QJsonObject saved = root.value("screens").toObject();
        for (auto it = saved.begin(); it != saved.end(); ++it) {
            if (it.key().isEmpty() || !it.value().isString())
                return;
            assignments.insert(it.key(), it.value().toString());
        }
    }
    const bool enabled = root.value("enabled").toBool();
    QMap<QString, QString> modes;
    if (root.contains("displayModes")) {
        if (!root.value("displayModes").isObject())
            return;
        const auto savedModes = root.value("displayModes").toObject();
        for (auto it = savedModes.begin(); it != savedModes.end(); ++it) {
            if (it.key().isEmpty() || !it.value().isString() || !DisplayModes.contains(it.value().toString()))
                return;
            modes.insert(it.key(), it.value().toString());
        }
    }
    const QString validationError = validate(items, enabled, {});
    if (!validationError.isEmpty())
        return;
    for (const QString& id : assignments) {
        if (!validate(items, enabled, id).isEmpty())
            return;
    }
    m_items = items;
    m_enabled = enabled;
    m_assignments = assignments;
    m_displayModes = modes;
    m_loadError.clear();
}

QString WallpaperService::validate(const QVector<Item>& items, bool enabled, const QString& activeId) const
{
    QSet<QString> ids;
    for (const Item& item : items) {
        if (item.id.trimmed().isEmpty() || item.name.isEmpty() || item.path.isEmpty())
            return QStringLiteral("壁纸标识、名称和路径不能为空。");
        if (item.type != QStringLiteral("image") && item.type != QStringLiteral("video"))
            return QStringLiteral("壁纸媒体类型无效：%1").arg(item.name);
        if (ids.contains(item.id))
            return QStringLiteral("壁纸标识重复。");
        if (item.type == QStringLiteral("video") && item.animated)
            return QStringLiteral("视频记录不应携带动图标志：%1").arg(item.name);
        if (item.type == QStringLiteral("image") && !item.thumbnail.isEmpty()
            && !item.thumbnail.startsWith(ThumbnailPrefix))
            return QStringLiteral("壁纸缩略图数据无效：%1").arg(item.name);
        if (!QDateTime::fromString(item.addedAt, Qt::ISODateWithMs).isValid()
            || (!item.lastUsedAt.isEmpty() && !QDateTime::fromString(item.lastUsedAt, Qt::ISODateWithMs).isValid()))
            return QStringLiteral("壁纸时间信息无效：%1").arg(item.name);
        ids.insert(item.id);
    }
    if (!activeId.isEmpty() && !ids.contains(activeId))
        return QStringLiteral("当前使用的壁纸记录不存在。");
    Q_UNUSED(enabled);
    return {};
}

QVariantMap WallpaperService::commit(const QVector<Item>& items, bool enabled, const QMap<QString, QString>& assignments,
    const QMap<QString, QString>& modes)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const QString error = validate(items, enabled, {});
    if (!error.isEmpty())
        return ServiceResult::failure(error);
    QJsonObject screens;
    for (auto it = assignments.cbegin(); it != assignments.cend(); ++it) {
        const QString screenError = validate(items, enabled, it.value());
        if (it.key().isEmpty() || !screenError.isEmpty())
            return ServiceResult::failure(screenError.isEmpty() ? QStringLiteral("屏幕标识不能为空。") : screenError);
        screens.insert(it.key(), it.value());
    }
    QJsonArray itemArray;
    QJsonObject displayModes;
    for (auto it = modes.cbegin(); it != modes.cend(); ++it) {
        if (it.key().isEmpty() || !DisplayModes.contains(it.value()))
            return ServiceResult::failure(QStringLiteral("图片显示方式无效。"));
        displayModes.insert(it.key(), it.value());
    }
    for (const Item& item : items) {
        itemArray.append(QJsonObject{{"id", item.id}, {"name", item.name}, {"path", item.path},
            {"type", item.type}, {"animated", item.animated}, {"addedAt", item.addedAt},
            {"lastUsedAt", item.lastUsedAt}, {"thumbnail", item.thumbnail}, {"width", item.width}, {"height", item.height}});
    }
    const QByteArray bytes = QJsonDocument(QJsonObject{{"version", 2}, {"enabled", enabled},
        {"screens", screens}, {"displayModes", displayModes}, {"items", itemArray}}).toJson(QJsonDocument::Indented);
    if (bytes.size() > MaximumIndexBytes)
        return ServiceResult::failure(QStringLiteral("壁纸历史容量已满，请删除部分记录后重试。"));
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath()))
        return ServiceResult::failure(QStringLiteral("无法创建壁纸数据目录。"));
    QSaveFile file(m_path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return ServiceResult::failure(QStringLiteral("无法保存壁纸历史：") + file.errorString());
    m_items = items;
    m_enabled = enabled;
    m_assignments = assignments;
    m_displayModes = modes;
    emit changed();
    return ServiceResult::success();
}

QVariantMap WallpaperService::itemMap(const Item& item) const
{
    return QVariantMap{{"id", item.id}, {"name", item.name}, {"path", item.path},
        {"type", item.type}, {"animated", item.animated}, {"addedAt", item.addedAt},
        {"lastUsedAt", item.lastUsedAt}, {"thumbnail", item.thumbnail},
        {"width", item.width}, {"height", item.height},
        {"exists", QFileInfo::exists(item.path)}};
}

QVariantMap WallpaperService::data() const
{
    QVariantList items;
    for (const Item& item : m_items)
        items.append(itemMap(item));
    QVariantList screens;
    for (const QVariant& value : m_screens) {
        auto screen = value.toMap();
        const QString id = screen.value("id").toString();
        screen.insert("activeId", m_assignments.value(id));
        screen.insert("displayMode", m_displayModes.value(id, QStringLiteral("fill")));
        screen.insert("running", m_engines.contains(id) && m_engines.value(id)->isAlive()
            && m_engines.value(id)->isMediaReady());
        screen.insert("playbackError", m_playbackErrors.value(id));
        screens.append(screen);
    }
    return QVariantMap{{"enabled", m_enabled}, {"activeId", m_assignments.value(primaryScreenId())},
        {"items", items}, {"screens", screens}, {"engineError", m_engineError}};
}

QVariantMap WallpaperService::snapshot() const
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    return ServiceResult::success(data());
}

qsizetype WallpaperService::position(const QString& id) const
{
    for (qsizetype index = 0; index < m_items.size(); ++index) {
        if (m_items.at(index).id == id)
            return index;
    }
    return -1;
}

QString WallpaperService::classify(const QString& path, bool* animated)
{
    *animated = false;
    if (VideoSuffixes.contains(QFileInfo(path).suffix().toLower()))
        return QStringLiteral("video");
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    if (!reader.canRead())
        return {};
    const QSize size = reader.size();
    // 在解码前检查图像尺寸，避免超大图片占用过多内存。
    if (size.isValid() && qint64(size.width()) * size.height() > MaximumImagePixels)
        return {};
    *animated = reader.imageCount() > 1;
    return QStringLiteral("image");
}

QString WallpaperService::buildThumbnail(const QString& path)
{
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    const QSize size = reader.size();
    if (size.isValid() && qint64(size.width()) * size.height() > MaximumImagePixels)
        return {};
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    if (image.isNull())
        return {};
    return imageDataUrl(image.scaled(ThumbnailSize, ThumbnailSize,
        Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

QVariantMap WallpaperService::add(const QVariantMap& item)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    refreshScreens();
    const QString screenId = item.value("screenId", primaryScreenId()).toString();
    if (!hasScreen(screenId))
        return ServiceResult::failure(QStringLiteral("目标屏幕已断开，请重新选择显示器。"));
    if (!QJsonValue::fromVariant(item.value(QStringLiteral("path"))).isString())
        return ServiceResult::failure(QStringLiteral("壁纸字段格式错误：path"));
    const QString rawPath = item.value("path").toString().trimmed();
    const QFileInfo file(QDir::cleanPath(rawPath));
    if (rawPath.isEmpty() || !file.isFile() || !file.exists())
        return ServiceResult::failure(QStringLiteral("壁纸文件不存在：") + QDir::toNativeSeparators(rawPath));
    bool animated = false;
    const QString type = classify(file.absoluteFilePath(), &animated);
    if (type.isEmpty())
        return ServiceResult::failure(QStringLiteral("所选文件不是支持的图片或视频格式。"));
    const QString path = QDir::toNativeSeparators(file.absoluteFilePath());
    // 同一文件已在历史中时直接切换使用，不重复记录。
    for (const Item& existing : m_items) {
        if (existing.path.compare(path, Qt::CaseInsensitive) == 0)
            return use(existing.id, screenId);
    }
    QString thumbnail;
    if (type == QStringLiteral("image")) {
        thumbnail = buildThumbnail(file.absoluteFilePath());
        if (thumbnail.isEmpty())
            return ServiceResult::failure(QStringLiteral("无法读取壁纸图片，或图片超过 5000 万像素。"));
    }
    if (m_items.size() >= MaximumItems)
        return ServiceResult::failure(QStringLiteral("壁纸历史已达上限，请删除部分记录后重试。"));
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    Item value{newId(), file.fileName(), path, type, animated, now, now, thumbnail};
    if (type == QStringLiteral("image")) {
        const QSize size = imageSize(path);
        value.width = size.width();
        value.height = size.height();
    }
    QVector<Item> items = m_items;
    items.prepend(value);
    auto assignments = m_assignments;
    assignments.insert(screenId, value.id);
    const auto result = commit(items, m_enabled, assignments, m_displayModes);
    if (!result.value("ok").toBool())
        return result;
    m_failedItems.remove(screenId);
    m_playbackErrors.remove(screenId);
    if (m_enabled && !startEngine())
        return ServiceResult::failure(QStringLiteral("无法挂载桌面壁纸层，请稍后重试。"));
    return ServiceResult::success(itemMap(value));
}

QVariantMap WallpaperService::use(const QString& id, const QString& screenId)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    refreshScreens();
    const QString target = screenId.isEmpty() ? primaryScreenId() : screenId;
    if (!hasScreen(target))
        return ServiceResult::failure(QStringLiteral("目标屏幕已断开，请重新选择显示器。"));
    const qsizetype index = position(id);
    if (index < 0)
        return ServiceResult::failure(QStringLiteral("要使用的壁纸记录不存在。"));
    const Item item = m_items.at(index);
    if (!QFileInfo::exists(item.path))
        return ServiceResult::failure(QStringLiteral("壁纸的源文件已丢失，无法显示：") + item.path);
    QVector<Item> items = m_items;
    items[index].lastUsedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    auto assignments = m_assignments;
    assignments.insert(target, item.id);
    const auto result = commit(items, m_enabled, assignments, m_displayModes);
    if (!result.value("ok").toBool())
        return result;
    // 用户再次选择同一资源属于主动重试；定时守护不会清除此失败状态。
    m_failedItems.remove(target);
    m_playbackErrors.remove(target);
    if (m_enabled && !startEngine())
        return ServiceResult::failure(QStringLiteral("无法挂载桌面壁纸层，请稍后重试。"));
    return ServiceResult::success(itemMap(items.at(index)));
}

QVariantMap WallpaperService::remove(const QStringList& ids)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    QSet<QString> requested;
    for (const QString& id : ids) {
        if (position(id) < 0)
            return ServiceResult::failure(QStringLiteral("要删除的壁纸记录不存在，未删除任何条目。"));
        requested.insert(id);
    }
    QVector<Item> items;
    for (const Item& item : m_items) {
        if (!requested.contains(item.id))
            items.append(item);
    }
    auto assignments = m_assignments;
    for (auto it = assignments.begin(); it != assignments.end(); ++it) {
        if (requested.contains(it.value()))
            it.value().clear();
    }
    const auto result = commit(items, m_enabled, assignments, m_displayModes);
    if (!result.value("ok").toBool())
        return result;
    ensureEngineAlive();
    return ServiceResult::success(data());
}

QVariantMap WallpaperService::setEnabled(bool enabled)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    refreshScreens();
    if (enabled) {
        for (const QVariant& value : m_screens) {
            const qsizetype index = position(m_assignments.value(value.toMap().value("id").toString()));
            if (index >= 0 && !QFileInfo::exists(m_items.at(index).path))
                return ServiceResult::failure(QStringLiteral("当前壁纸的源文件已丢失，请先选择其他资源或恢复该屏系统壁纸。"));
        }
    }
    const bool wasEnabled = m_enabled;
    if (enabled) {
        m_failedItems.clear();
        m_playbackErrors.clear();
    }
    if (enabled && !startEngine()) {
        if (!wasEnabled)
            stopEngine();
        return ServiceResult::failure(QStringLiteral("无法挂载桌面壁纸层，请稍后重试。"));
    }
    const auto result = commit(m_items, enabled, m_assignments, m_displayModes);
    if (!result.value("ok").toBool()) {
        if (!wasEnabled)
            stopEngine();
        return result;
    }
    if (!enabled)
        stopEngine();
    emit changed();
    return ServiceResult::success(data());
}

QVariantMap WallpaperService::openDirectory(const QString& id)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const qsizetype index = position(id);
    if (index < 0)
        return ServiceResult::failure(QStringLiteral("该壁纸记录已不存在。"));
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

bool WallpaperService::startEngine()
{
    if (!m_nativeIntegration)
        return true;
    QSet<QString> desired;
    for (const QVariant& value : m_screens) {
        const QString id = value.toMap().value("id").toString();
        if (m_failedItems.contains(id) && m_failedItems.value(id) != m_assignments.value(id)) {
            m_failedItems.remove(id);
            m_playbackErrors.remove(id);
        }
        const qsizetype index = position(m_assignments.value(id));
        if (index >= 0 && QFileInfo::exists(m_items.at(index).path) && !m_failedItems.contains(id))
            desired.insert(id);
    }
    for (const QString& id : m_engines.keys()) {
        if (!desired.contains(id) || !m_engines.value(id)->isAlive())
            delete m_engines.take(id);
    }
    bool success = true;
    for (const QString& id : desired) {
        if (!m_engines.contains(id)) {
            auto* engine = new WallpaperWindow;
            if (!engine->attachToDesktop(id)) {
                delete engine;
                success = false;
                continue;
            }
            m_engines.insert(id, engine);
            connect(engine, &WallpaperWindow::mediaReady, this, &WallpaperService::changed);
            // 排队处理，避免在 QMediaPlayer 的错误回调栈上直接析构播放器。
            const QPointer<WallpaperWindow> guardedEngine(engine);
            connect(engine, &WallpaperWindow::mediaFailed, this, [this, id, guardedEngine](const QString& path, const QString& message) {
                const qsizetype index = position(m_assignments.value(id));
                if (!guardedEngine || m_engines.value(id) != guardedEngine.data()
                    || index < 0 || m_items.at(index).path != path)
                    return;
                m_failedItems.insert(id, m_assignments.value(id));
                m_playbackErrors.insert(id, message);
                delete m_engines.take(id);
                emit changed();
            }, Qt::QueuedConnection);
        }
        const Item& item = m_items.at(position(m_assignments.value(id)));
        m_engines.value(id)->setSource(item.path, item.type, m_displayModes.value(id, QStringLiteral("fill")));
    }
    const QString error = success ? QString() : QStringLiteral("部分屏幕无法挂载桌面壁纸层，正在等待重试。");
    if (m_engineError != error) {
        m_engineError = error;
        emit changed();
    }
    return success;
}

void WallpaperService::stopEngine()
{
    // 同步析构，先撤回原生窗口再释放 WebEngine；关闭开关返回前桌面已恢复。
    qDeleteAll(m_engines);
    m_engines.clear();
    m_engineError.clear();
    m_failedItems.clear();
    m_playbackErrors.clear();
}

void WallpaperService::ensureEngineAlive()
{
    refreshScreens();
    if (!m_enabled) {
        stopEngine();
        return;
    }
    const auto previous = m_engines.keys();
    startEngine();
    if (previous != m_engines.keys())
        emit changed();
}

// 枚举变化后立即回收旧矩形窗口；持久化选择不随拔屏删除。
void WallpaperService::refreshScreens()
{
    const QVariantList screens = m_screenProvider();
    if (screens == m_screens)
        return;
    stopEngine();
    m_screens = screens;
    if (m_enabled)
        startEngine();
    emit changed();
}

// 兼容旧接口时优先使用系统主屏。
QString WallpaperService::primaryScreenId() const
{
    for (const QVariant& value : m_screens) {
        const auto screen = value.toMap();
        if (screen.value("primary").toBool())
            return screen.value("id").toString();
    }
    return m_screens.isEmpty() ? QString() : m_screens.first().toMap().value("id").toString();
}

// 检查目标屏仍存在，防止异步选择文件期间拔屏后应用到其他屏幕。
bool WallpaperService::hasScreen(const QString& id) const
{
    for (const QVariant& screen : m_screens) {
        if (screen.toMap().value("id").toString() == id)
            return true;
    }
    return false;
}

// 清除单个屏幕的选择，历史和其他屏幕保持不变。
QVariantMap WallpaperService::clearScreen(const QString& screenId)
{
    refreshScreens();
    if (!hasScreen(screenId))
        return ServiceResult::failure(QStringLiteral("目标屏幕已断开，请重新选择显示器。"));
    auto assignments = m_assignments;
    assignments.remove(screenId);
    const auto result = commit(m_items, m_enabled, assignments, m_displayModes);
    if (!result.value("ok").toBool())
        return result;
    ensureEngineAlive();
    return ServiceResult::success(data());
}

// 模式只在原子保存成功后更新，避免保存失败导致预览与桌面状态不一致。
QVariantMap WallpaperService::setDisplayMode(const QString& screenId, const QString& mode)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    refreshScreens();
    if (!hasScreen(screenId))
        return ServiceResult::failure(QStringLiteral("目标屏幕已断开，请重新选择显示器。"));
    if (!DisplayModes.contains(mode))
        return ServiceResult::failure(QStringLiteral("图片显示方式无效。"));
    auto modes = m_displayModes;
    modes.insert(screenId, mode);
    const auto result = commit(m_items, m_enabled, m_assignments, modes);
    if (!result.value("ok").toBool())
        return result;
    ensureEngineAlive();
    return ServiceResult::success(data());
}
