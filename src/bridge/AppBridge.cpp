#include "app/InterfaceLanguage.h"
#include "AppBridge.h"

#include "app/GlobalHotkey.h"
#include "app/RecordingControlBar.h"
#include "app/RecordingOverlay.h"
#include "app/RecordingRegionFrame.h"
#include "app/ScreenRecorder.h"
#include "app/ScreenshotOverlay.h"
#include "services/DownloadService.h"
#include "services/ClipboardService.h"
#include "services/EmojiService.h"
#include "services/BookmarkService.h"
#include "services/FileSearchService.h"
#include "services/TerrainService.h"
#include "app/BookmarkWindow.h"
#include "app/TerrainEditWindow.h"
#include "app/PaintWindow.h"
#include "level/LevelWindow.h"
#include "level/LevelDocument.h"
#include <QUuid>
#include "services/MediaService.h"
#include "services/NoteService.h"
#include "services/QrService.h"
#include "services/RecordingService.h"
#include "services/ScreenshotService.h"
#include "services/ServiceResult.h"
#include "services/SettingsService.h"
#include "services/ShortcutService.h"
#include "services/WallpaperService.h"
#include "services/GridMapService.h"
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QJsonValue>
#include <QMessageBox>
#include <QMimeData>
#include <QPointer>
#include <QProcess>
#include <QSaveFile>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QWidget>
#include <utility>

namespace
{
// 网格图导出数据允许的最大 base64 长度。
constexpr qint64 MaximumGridPngChars = 96 * 1024 * 1024;
// 二维码图片数据地址与像素上限，与剪贴板服务保持一致的防护边界。
constexpr qint64 MaximumImageDataUrlChars = 16 * 1024 * 1024;
constexpr qint64 MaximumImagePixels = 20000000;
constexpr qint64 MaximumClipboardImageBytes = 8 * 1024 * 1024;

// 把图像编码为 PNG 数据地址（录屏首帧缩略图）；失败返回空。
QString encodeImageDataUrl(const QImage& image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG"))
        return {};
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(bytes.toBase64());
}

// 把网页提交的 PNG 数据地址转换为图像；格式或大小不符时返回空图像。
QImage decodeGridPng(const QString& imageDataUrl)
{
    if (imageDataUrl.size() > MaximumGridPngChars)
        return QImage();
    const QString prefix = QStringLiteral("data:image/png;base64,");
    if (!imageDataUrl.startsWith(prefix, Qt::CaseInsensitive))
        return QImage();
    const QByteArray bytes = QByteArray::fromBase64(imageDataUrl.mid(prefix.size()).toLatin1());
    if (bytes.isEmpty())
        return QImage();
    QImage image;
    if (!image.loadFromData(bytes, "PNG"))
        return QImage();
    return image;
}

// 只为正式运行连接当前用户的 Windows 自启注册表；测试不接触系统启动项。
QSettings* startupSettings(bool nativeIntegration, QObject* owner)
{
#ifdef Q_OS_WIN
    if (nativeIntegration)
        return new QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
                             QSettings::NativeFormat, owner);
#else
    Q_UNUSED(nativeIntegration);
    Q_UNUSED(owner);
#endif
    return nullptr;
}
}

AppBridge::AppBridge(const QString& dataRoot, QWidget* window, bool nativeIntegration)
    : QObject(window), m_dataRoot(QDir(dataRoot).absolutePath()), m_window(window),
      m_shortcuts(new ShortcutService(m_dataRoot, this)),
      m_notes(new NoteService(m_dataRoot, this)),
      m_bookmarks(new BookmarkService(m_dataRoot, this)),
      m_settings(new SettingsService(m_dataRoot, this, startupSettings(nativeIntegration, this))),
      m_downloads(new DownloadService(m_dataRoot, this)),
      m_clipboard(new ClipboardService(m_dataRoot, nativeIntegration, this)),
      m_emoji(new EmojiService(m_dataRoot, m_clipboard, this)),
      m_wallpaper(new WallpaperService(m_dataRoot, nativeIntegration, this)),
      m_gridmaps(new GridMapService(m_dataRoot, this)),
      m_qr(new QrService(m_dataRoot, this)),
      m_screenshots(new ScreenshotService(m_dataRoot, this)),
      m_recordings(new RecordingService(m_dataRoot, this)),
      m_media(new MediaService(nativeIntegration, this)),
      m_fileSearch(new FileSearchService(m_dataRoot, nativeIntegration, this)),
      m_terrain(new TerrainService(m_dataRoot, this)),
      m_hotkey(new GlobalHotkey(nativeIntegration, 0x4D01, this)),
      m_screenshotHotkey(new GlobalHotkey(nativeIntegration, 0x4D03, this)),
      m_nativeIntegration(nativeIntegration)
{
    // 更新旧版本的打开命令，默认媒体关联以后直接启动 tools 中的独立工具。
    if (m_nativeIntegration)
        m_media->registerBrowserProgIds();
    connect(m_shortcuts, &ShortcutService::changed, this, &AppBridge::shortcutsChanged);
    connect(m_notes, &NoteService::changed, this, &AppBridge::notesChanged);
    connect(m_bookmarks, &BookmarkService::changed, this, &AppBridge::bookmarksChanged);
    connect(m_settings, &SettingsService::changed, this, &AppBridge::settingsChanged);
    connect(m_downloads, &DownloadService::changed, this, &AppBridge::downloadsChanged);
    connect(m_clipboard, &ClipboardService::changed, this, &AppBridge::clipboardChanged);
    connect(m_emoji, &EmojiService::changed, this, &AppBridge::emojisChanged);
    connect(m_wallpaper, &WallpaperService::changed, this, &AppBridge::wallpaperChanged);
    connect(m_gridmaps, &GridMapService::changed, this, &AppBridge::gridMapsChanged);
    connect(m_qr, &QrService::changed, this, &AppBridge::qrHistoryChanged);
    connect(m_screenshots, &ScreenshotService::changed, this, &AppBridge::screenshotsChanged);
    connect(m_recordings, &RecordingService::changed, this, &AppBridge::recordingsChanged);
    connect(m_fileSearch, &FileSearchService::statusChanged, this, &AppBridge::fileSearchStatusChanged);
    connect(m_terrain, &TerrainService::changed, this, &AppBridge::terrainChanged);
    connect(m_fileSearch, &FileSearchService::searchFinished, this, &AppBridge::fileSearchResults);
    connect(m_hotkey, &GlobalHotkey::activated, this, &AppBridge::activateWindowRequested);
    connect(m_screenshotHotkey, &GlobalHotkey::activated, this,
        [this]() { beginScreenshot(m_window && m_window->isVisible()); });
    const auto result = m_settings->snapshot();
    if (result.value("ok").toBool()) {
        const auto settings = result.value("data").toMap();
        m_screenshotEnabled = settings.value("screenshotEnabled").toBool();
        m_hotkey->setShortcut(settings.value("hotkeyModifier").toInt(),
                             settings.value("hotkeyKey").toInt(), &m_hotkeyWarning);
        // 功能关闭时不注册截图热键，避免与用户当前的使用习惯冲突。
        if (m_screenshotEnabled)
            m_screenshotHotkey->setShortcut(settings.value("screenshotHotkeyModifier").toInt(),
                settings.value("screenshotHotkeyKey").toInt(), &m_screenshotHotkeyWarning);
    } else {
        m_hotkeyWarning = result.value("error").toString();
    }
}

QVariantMap AppBridge::getShortcuts() const { return m_shortcuts->snapshot(); }
QVariantMap AppBridge::saveShortcut(const QVariantMap& item) { return m_shortcuts->saveShortcut(item); }
QVariantMap AppBridge::deleteShortcuts(const QStringList& ids) { return m_shortcuts->removeShortcuts(ids); }
QVariantMap AppBridge::moveShortcut(const QString& id, const QString& categoryId, const QString& beforeId)
{ return m_shortcuts->moveShortcut(id, categoryId, beforeId); }
QVariantMap AppBridge::getBookmarks() const { return m_bookmarks->snapshot(); }
QVariantMap AppBridge::openBookmarkWindow()
{
    // 子控件由主窗口持有，切换功能仅隐藏，不销毁网页和下载状态。
    if (!m_window)
        return ServiceResult::failure(QStringLiteral("未找到书签宿主窗口。"));
    const QString directory = m_settings->snapshot().value(QStringLiteral("data")).toMap()
        .value(QStringLiteral("downloadDirectory")).toString();
    if (!m_bookmarkWindow)
        m_bookmarkWindow = BookmarkWindow::open(m_bookmarks, directory, m_window);
    m_bookmarkWindow->setDownloadDirectory(directory);
    return ServiceResult::success();
}

QVariantMap AppBridge::setBookmarkViewport(bool visible, int x, int y, int width, int height)
{
    if (!visible) {
        if (m_bookmarkWindow) m_bookmarkWindow->hide();
        return ServiceResult::success();
    }
    const auto result = openBookmarkWindow();
    if (!result.value(QStringLiteral("ok")).toBool()) return result;
    const QRect bounds = QRect(x, y, qMax(0, width), qMax(0, height)).intersected(m_window->rect());
    m_bookmarkWindow->setGeometry(bounds);
    m_bookmarkWindow->setVisible(!bounds.isEmpty());
    m_bookmarkWindow->raise();
    return ServiceResult::success();
}
QVariantMap AppBridge::saveShortcutCategory(const QString& id, const QString& name)
{ return m_shortcuts->saveCategory(id, name); }
QVariantMap AppBridge::deleteShortcutCategory(const QString& id) { return m_shortcuts->removeCategory(id); }
QVariantMap AppBridge::openShortcut(const QString& id) { return m_shortcuts->openShortcut(id); }

QVariantMap AppBridge::chooseTarget(int type)
{
    if (type != 0 && type != 2)
        return ServiceResult::failure(QStringLiteral("请选择文件或目录类型。"));
    const QString path = type == 0
        ? QFileDialog::getExistingDirectory(m_window, InterfaceLanguage::text(QStringLiteral("选择目录")))
        : QFileDialog::getOpenFileName(m_window, InterfaceLanguage::text(QStringLiteral("选择文件")));
    if (path.isEmpty())
        return ServiceResult::success(QVariantMap{{"cancelled", true}});
    return ServiceResult::success(QVariantMap{{"target", path}, {"title", QFileInfo(path).fileName()}});
}

QVariantMap AppBridge::copyText(const QString& text)
{
    QApplication::clipboard()->setText(text);
    return ServiceResult::success();
}

QVariantMap AppBridge::getNotes() const { return m_notes->snapshot(); }
QVariantMap AppBridge::getClipboardHistory(const QString& query) const { return m_clipboard->snapshot(query); }
QVariantMap AppBridge::setClipboardTypes(const QStringList& types) { return m_clipboard->setTypes(types); }
QVariantMap AppBridge::getClipboardItem(const QString& id) const { return m_clipboard->read(id); }
QVariantMap AppBridge::copyClipboardItem(const QString& id) { return m_clipboard->copy(id); }
QVariantMap AppBridge::pinClipboardItem(const QString& id, bool pinned) { return m_clipboard->pin(id, pinned); }
QVariantMap AppBridge::deleteClipboardItems(const QStringList& ids) { return m_clipboard->remove(ids); }
QVariantMap AppBridge::clearClipboardHistory() { return m_clipboard->clear(); }
QVariantMap AppBridge::openClipboardDirectory(const QString& id, int fileIndex)
{ return m_clipboard->openDirectory(id, fileIndex); }

QVariantMap AppBridge::getEmojis(const QString& query) const { return m_emoji->snapshot(query); }

QVariantMap AppBridge::chooseEmojiImage()
{
    QStringList patterns;
    for (const auto& format : QImageReader::supportedImageFormats())
        patterns.append(QStringLiteral("*.") + QString::fromLatin1(format));
    const QString path = QFileDialog::getOpenFileName(m_window, InterfaceLanguage::text(QStringLiteral("选择表情图片")), QString(),
        InterfaceLanguage::text(QStringLiteral("图片文件 (%1)")).arg(patterns.join(QLatin1Char(' '))));
    if (path.isEmpty())
        return ServiceResult::success(QVariantMap{{"cancelled", true}});
    return ServiceResult::success(QVariantMap{{"target", path}, {"title", QFileInfo(path).fileName()}});
}

QVariantMap AppBridge::pasteEmojiImage()
{
    return m_clipboard->pasteResource(QDir(m_dataRoot).absoluteFilePath(QStringLiteral("emoji/clipboard")));
}

QVariantMap AppBridge::addEmoji(const QVariantMap& item) { return m_emoji->add(item); }
QVariantMap AppBridge::saveEmoji(const QVariantMap& item) { return m_emoji->save(item); }
QVariantMap AppBridge::deleteEmojis(const QStringList& ids) { return m_emoji->remove(ids); }
QVariantMap AppBridge::saveEmojiCategory(const QString& id, const QString& name)
{ return m_emoji->saveCategory(id, name); }
QVariantMap AppBridge::deleteEmojiCategory(const QString& id) { return m_emoji->removeCategory(id); }
QVariantMap AppBridge::openEmojiDirectory(const QString& id) { return m_emoji->openDirectory(id); }
QVariantMap AppBridge::copyEmojiFile(const QString& id) { return m_emoji->copyFile(id); }

QVariantMap AppBridge::getWallpaper() const { return m_wallpaper->snapshot(); }

QVariantMap AppBridge::chooseWallpaperResource()
{
    const QString patterns = QStringLiteral(
        "*.gif *.webp *.png *.jpg *.jpeg *.bmp *.apng *.svg"
        " *.mp4 *.webm *.mkv *.avi *.mov *.m4v *.wmv *.mpg *.mpeg *.flv *.ts *.3gp");
    const QString path = QFileDialog::getOpenFileName(m_window, InterfaceLanguage::text(QStringLiteral("选择动态壁纸资源")), QString(),
        InterfaceLanguage::text(QStringLiteral("图片和视频 (%1);;所有文件 (*.*)")).arg(patterns));
    if (path.isEmpty())
        return ServiceResult::success(QVariantMap{{"cancelled", true}});
    return ServiceResult::success(QVariantMap{{"target", path}, {"title", QFileInfo(path).fileName()}});
}

QVariantMap AppBridge::addWallpaper(const QVariantMap& item) { return m_wallpaper->add(item); }
QVariantMap AppBridge::useWallpaper(const QString& id) { return m_wallpaper->use(id); }
// 按稳定显示器标识应用历史壁纸。
QVariantMap AppBridge::useWallpaperOnScreen(const QString& id, const QString& screenId) { return m_wallpaper->use(id, screenId); }
// 撤回单个显示器的动态壁纸。
QVariantMap AppBridge::clearScreenWallpaper(const QString& screenId) { return m_wallpaper->clearScreen(screenId); }
// 图片显示方式按屏幕持久化并立即应用。
QVariantMap AppBridge::setWallpaperDisplayMode(const QString& screenId, const QString& mode) { return m_wallpaper->setDisplayMode(screenId, mode); }
QVariantMap AppBridge::removeWallpapers(const QStringList& ids) { return m_wallpaper->remove(ids); }
QVariantMap AppBridge::setWallpaperEnabled(bool enabled) { return m_wallpaper->setEnabled(enabled); }
QVariantMap AppBridge::openWallpaperDirectory(const QString& id) { return m_wallpaper->openDirectory(id); }

QVariantMap AppBridge::pasteWallpaperResource()
{
    return m_clipboard->pasteResource(QDir(m_dataRoot).absoluteFilePath(QStringLiteral("wallpaper/clipboard")));
}
QVariantMap AppBridge::getGridMaps() const { return m_gridmaps->snapshot(); }
QVariantMap AppBridge::createGridMap(const QString& title) { return m_gridmaps->create(title); }
QVariantMap AppBridge::renameGridMap(const QString& id, const QString& title) { return m_gridmaps->rename(id, title); }
QVariantMap AppBridge::deleteGridMap(const QStringList& ids) { return m_gridmaps->remove(ids); }
QVariantMap AppBridge::getGridMap(const QString& id) const { return m_gridmaps->read(id); }
QVariantMap AppBridge::saveGridMap(const QString& id, const QVariantMap& data) { return m_gridmaps->save(id, data); }

// 把网页渲染的网格图 PNG 保存到用户选择的位置。
QVariantMap AppBridge::exportGridMapPng(const QString& title, const QString& imageDataUrl)
{
    const QImage image = decodeGridPng(imageDataUrl);
    if (image.isNull())
        return ServiceResult::failure(QStringLiteral("网格图导出数据无效或过大，请重试。"));
    QString name = title.trimmed();
    for (const QChar character : QStringLiteral("\\/:*?\"<>|"))
        name.remove(character);
    if (name.isEmpty())
        name = QStringLiteral("网格图");
    const QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString suggested = QDir(pictures.isEmpty() ? m_dataRoot : pictures)
                                  .filePath(name + QStringLiteral(".png"));
    QString path = QFileDialog::getSaveFileName(m_window, InterfaceLanguage::text(QStringLiteral("导出网格图 PNG")),
        suggested, InterfaceLanguage::text(QStringLiteral("PNG 图片 (*.png)")));
    if (path.isEmpty())
        return ServiceResult::success(QVariantMap{{"cancelled", true}});
    if (!path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
        path += QStringLiteral(".png");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !image.save(&file, "PNG") || !file.commit())
        return ServiceResult::failure(QStringLiteral("无法保存 PNG 文件：") + file.errorString());
    return ServiceResult::success(QVariantMap{{QStringLiteral("target"), path}});
}

// 把网页渲染的网格图 PNG 复制到系统剪贴板。
QVariantMap AppBridge::copyGridMapPng(const QString& imageDataUrl)
{
    const QImage image = decodeGridPng(imageDataUrl);
    if (image.isNull())
        return ServiceResult::failure(QStringLiteral("网格图导出数据无效或过大，请重试。"));
    QApplication::clipboard()->setImage(image);
    return ServiceResult::success();
}

QVariantMap AppBridge::getQrHistory() const { return m_qr->snapshot(); }
QVariantMap AppBridge::readQr(const QString& id) const { return m_qr->read(id); }
QVariantMap AppBridge::removeQrHistory(const QStringList& ids) { return m_qr->remove(ids); }
QVariantMap AppBridge::clearQrHistory() { return m_qr->clear(); }

// 显示原生图片选择器并把所选图片编码为可显示的数据地址。
QVariantMap AppBridge::chooseQrImage()
{
    QStringList patterns;
    for (const auto& format : QImageReader::supportedImageFormats())
        patterns.append(QStringLiteral("*.") + QString::fromLatin1(format));
    const QString path = QFileDialog::getOpenFileName(m_window, InterfaceLanguage::text(QStringLiteral("选择二维码图片")), QString(),
        InterfaceLanguage::text(QStringLiteral("图片文件 (%1)")).arg(patterns.join(QLatin1Char(' '))));
    if (path.isEmpty())
        return ServiceResult::success(QVariantMap{{"cancelled", true}});
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    // 在解码前检查尺寸，避免超大图片占用过多内存。
    const QSize size = reader.size();
    if (size.isValid() && qint64(size.width()) * size.height() > MaximumImagePixels)
        return ServiceResult::failure(QStringLiteral("图片超过 2000 万像素，请缩小后再使用。"));
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    if (image.isNull())
        return ServiceResult::failure(QStringLiteral("图片无法读取或格式不支持。"));
    const QString encoded = QrService::encodeBoundedImage(image);
    if (encoded.isEmpty())
        return ServiceResult::failure(QStringLiteral("图片过大或无法编码，请缩小后再使用。"));
    return ServiceResult::success(QVariantMap{{"image", encoded}, {"title", QFileInfo(path).fileName()}});
}

// 读取系统剪贴板中的图片，优先标准图像格式，兼容仅提供 PNG/JPEG 数据的程序。
QVariantMap AppBridge::pasteQrImage()
{
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (!mime)
        return ServiceResult::failure(QStringLiteral("剪贴板暂时无法读取，请重试。"));
    QImage image = qvariant_cast<QImage>(mime->imageData());
    if (image.isNull()) {
        const QVariant data = mime->imageData();
        if (data.canConvert<QPixmap>())
            image = qvariant_cast<QPixmap>(data).toImage();
    }
    for (const QString& format : {QStringLiteral("image/png"), QStringLiteral("image/jpeg"),
             QStringLiteral("application/x-qt-windows-mime;value=\"PNG\"")}) {
        if (!image.isNull() || !mime->hasFormat(format))
            continue;
        const QByteArray bytes = mime->data(format);
        if (bytes.isEmpty() || bytes.size() > MaximumClipboardImageBytes)
            continue;
        image = QImage::fromData(bytes);
    }
    if (image.isNull())
        return ServiceResult::failure(QStringLiteral("剪贴板中没有可用的图片，请先复制二维码图片。"));
    if (qint64(image.width()) * image.height() > MaximumImagePixels)
        return ServiceResult::failure(QStringLiteral("剪贴板图像超过 2000 万像素，请缩小后再使用。"));
    const QString encoded = QrService::encodeBoundedImage(image);
    if (encoded.isEmpty())
        return ServiceResult::failure(QStringLiteral("图片过大或无法编码，请缩小后再使用。"));
    return ServiceResult::success(QVariantMap{{"image", encoded}});
}

// 识别网页回传的二维码图片并记入历史。
QVariantMap AppBridge::decodeQr(const QString& imageDataUrl, const QString& source)
{
    if (imageDataUrl.size() > MaximumImageDataUrlChars)
        return ServiceResult::failure(QStringLiteral("图片数据过大，无法识别。"));
    const QString marker = QStringLiteral(";base64,");
    const qsizetype markerIndex = imageDataUrl.indexOf(marker, 0, Qt::CaseInsensitive);
    if (!imageDataUrl.startsWith(QStringLiteral("data:image/"), Qt::CaseInsensitive) || markerIndex < 0)
        return ServiceResult::failure(QStringLiteral("图片数据格式无效。"));
    const QByteArray bytes = QByteArray::fromBase64(imageDataUrl.mid(markerIndex + marker.size()).toLatin1());
    const QImage image = QImage::fromData(bytes);
    if (image.isNull())
        return ServiceResult::failure(QStringLiteral("图片无法读取或格式不支持。"));
    if (qint64(image.width()) * image.height() > MaximumImagePixels)
        return ServiceResult::failure(QStringLiteral("图片超过 2000 万像素，请缩小后再使用。"));
    return m_qr->decode(image, source);
}

QVariantMap AppBridge::getScreenshotHistory() const { return m_screenshots->snapshot(); }
QVariantMap AppBridge::getScreenshot(const QString& id) const { return m_screenshots->read(id); }
QVariantMap AppBridge::deleteScreenshots(const QStringList& ids) { return m_screenshots->remove(ids); }
QVariantMap AppBridge::clearScreenshotHistory() { return m_screenshots->clear(); }
QVariantMap AppBridge::openScreenshotDirectory(const QString& id) { return m_screenshots->openDirectory(id); }

// 把历史中的截图重新复制到系统剪贴板。
QVariantMap AppBridge::copyScreenshot(const QString& id)
{
    const auto result = m_screenshots->read(id);
    if (!result.value("ok").toBool())
        return result;
    const auto image = result.value("data").toMap().value("image").toString();
    const QString prefix = QStringLiteral("data:image/png;base64,");
    if (!image.startsWith(prefix))
        return ServiceResult::failure(QStringLiteral("截图数据无效，无法复制。"));
    const QImage decoded = QImage::fromData(QByteArray::fromBase64(image.mid(prefix.size()).toLatin1()), "PNG");
    if (decoded.isNull())
        return ServiceResult::failure(QStringLiteral("截图文件损坏，无法复制。"));
    QApplication::clipboard()->setImage(decoded);
    return ServiceResult::success();
}

// 校验并保存截图热键；启用状态下先预留注册，保存成功后才启用，失败时回滚。
QVariantMap AppBridge::saveScreenshotHotkey(int modifier, int key)
{
    const bool registered = m_screenshotEnabled;
    QString error;
    if (registered && !m_screenshotHotkey->prepareShortcut(modifier, key, &error))
        return ServiceResult::failure(error);
    const auto saved = m_settings->save({{"screenshotHotkeyModifier", modifier}, {"screenshotHotkeyKey", key}});
    if (!saved.value("ok").toBool()) {
        if (registered)
            m_screenshotHotkey->cancelShortcut();
        return saved;
    }
    if (registered)
        m_screenshotHotkey->commitShortcut();
    m_screenshotHotkeyWarning.clear();
    return saved;
}

// 开关截图功能：关闭时解除热键，开启时按已保存的组合重新注册，保存失败回滚。
QVariantMap AppBridge::setScreenshotEnabled(bool enabled)
{
    if (enabled == m_screenshotEnabled)
        return m_settings->snapshot();
    QString error;
    if (enabled) {
        const auto settings = m_settings->snapshot().value("data").toMap();
        if (!m_screenshotHotkey->setShortcut(settings.value("screenshotHotkeyModifier").toInt(),
                settings.value("screenshotHotkeyKey").toInt(), &error))
            return ServiceResult::failure(error);
    } else {
        m_screenshotHotkey->releaseShortcut();
    }
    const auto saved = m_settings->save({{"screenshotEnabled", enabled}});
    if (!saved.value("ok").toBool()) {
        if (enabled)
            m_screenshotHotkey->releaseShortcut();
        else {
            const auto settings = m_settings->snapshot().value("data").toMap();
            m_screenshotHotkey->setShortcut(settings.value("screenshotHotkeyModifier").toInt(),
                settings.value("screenshotHotkeyKey").toInt(), nullptr);
        }
        return saved;
    }
    m_screenshotEnabled = enabled;
    return saved;
}

// 逐屏抓取并显示遮罩；选区限制在所在屏幕内，按各屏 DPR 精确映射，
// 保证混合 DPI（如 4K 150% + 1080p 100%）下坐标与分辨率都正确。
void AppBridge::beginScreenshot(bool restoreWindow)
{
    if (!m_nativeIntegration || m_screenshotBusy)
        return;
    m_screenshotBusy = true;
    if (restoreWindow && m_window)
        m_window->hide();
    // 等待窗口真正从屏幕消失后再抓取，避免把本程序截进图里。
    QTimer::singleShot(restoreWindow ? 260 : 80, this, [this, restoreWindow]() {
        struct ScreenCapture {
            QRect geometry;
            QImage image;
        };
        QList<ScreenCapture> captures;
        for (QScreen* screen : QGuiApplication::screens()) {
            const QImage image = screen->grabWindow(0).toImage();
            if (!image.isNull())
                captures.append({screen->geometry(), image});
        }
        if (captures.isEmpty()) {
            m_screenshotBusy = false;
            if (restoreWindow && m_window)
                m_window->show();
            QMessageBox::warning(m_window, InterfaceLanguage::text(QStringLiteral("截图")),
                InterfaceLanguage::text(QStringLiteral("无法截取屏幕内容，请重试或联系开发者。")));
            return;
        }
        for (const ScreenCapture& capture : captures) {
            auto* overlay = new ScreenshotOverlay(capture.image);
            overlay->setGeometry(capture.geometry);
            m_overlays.append(overlay);
            connect(overlay, &ScreenshotOverlay::finished, this,
                [this, overlay, restoreWindow](const QImage& image, bool cancelled) {
                overlay->deleteLater();
                m_overlays.removeAll(overlay);
                // 本轮截图由第一个完成的遮罩处理；其余遮罩随关闭只做销毁。
                if (!m_screenshotBusy)
                    return;
                m_screenshotBusy = false;
                // 先移出待关闭列表，避免其他遮罩的 finished 回调修改正在遍历的列表。
                const auto overlays = std::exchange(m_overlays, {});
                for (const QPointer<ScreenshotOverlay>& other : overlays) {
                    if (other)
                        other->close();
                }
                if (!image.isNull()) {
                    QApplication::clipboard()->setImage(image);
                    m_screenshots->add(image, QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
                }
                // 完成后保持主窗口隐藏；仅取消时恢复操作前可见的窗口。
                if (cancelled && restoreWindow && m_window)
                    m_window->show();
            });
            // 任一屏幕开始重新框选时，其余屏幕遮罩的旧选区与工具栏一并收起。
            connect(overlay, &ScreenshotOverlay::newSelectionStarted, this, [this, overlay]() {
                for (const QPointer<ScreenshotOverlay>& other : std::as_const(m_overlays)) {
                    if (other && other != overlay)
                        other->dismissSelection();
                }
            });
            overlay->show();
            overlay->raise();
            overlay->activateWindow();
        }
    });
}

// 由网页按钮或热键触发一次交互式截图。
QVariantMap AppBridge::startScreenshot()
{
    if (!m_nativeIntegration)
        return ServiceResult::failure(QStringLiteral("当前环境不支持截图功能。"));
    if (!m_screenshotEnabled)
        return ServiceResult::failure(QStringLiteral("截图功能已关闭，请在截图页重新开启。"));
    if (m_screenshotBusy || !m_overlays.isEmpty())
        return ServiceResult::failure(QStringLiteral("截图正在进行中，请先完成或取消。"));
    beginScreenshot(m_window && m_window->isVisible());
    return ServiceResult::success();
}

QVariantMap AppBridge::getRecordingHistory() const { return m_recordings->snapshot(); }
QVariantMap AppBridge::getRecording(const QString& id) const { return m_recordings->read(id); }
QVariantMap AppBridge::deleteRecordings(const QStringList& ids) { return m_recordings->remove(ids); }
QVariantMap AppBridge::clearRecordingHistory() { return m_recordings->clear(); }
QVariantMap AppBridge::openRecordingDirectory(const QString& id) { return m_recordings->openDirectory(id); }

// 把本地文件复制到系统剪贴板；Windows 上以 CF_HDROP 提供，资源管理器可直接粘贴。
void AppBridge::copyFileToClipboard(const QString& path)
{
    auto* mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(path)});
    QApplication::clipboard()->setMimeData(mime);
}

// 把历史中的录屏视频文件复制到系统剪贴板。
QVariantMap AppBridge::copyRecording(const QString& id)
{
    const auto result = m_recordings->read(id);
    if (!result.value("ok").toBool())
        return result;
    copyFileToClipboard(result.value("data").toMap().value("path").toString());
    return ServiceResult::success();
}

// 由网页按钮触发一次交互式录屏。
QVariantMap AppBridge::startRecording()
{
    if (!m_nativeIntegration)
        return ServiceResult::failure(QStringLiteral("当前环境不支持录屏功能。"));
    if (m_recordingBusy || !m_recordingOverlays.isEmpty())
        return ServiceResult::failure(QStringLiteral("录屏正在进行中，请先结束或取消。"));
    beginRecording(m_window && m_window->isVisible());
    return ServiceResult::success();
}

// 关闭并销毁全部录屏选区遮罩。
void AppBridge::closeRecordingOverlays()
{
    const auto overlays = std::exchange(m_recordingOverlays, {});
    for (const QPointer<RecordingOverlay>& overlay : overlays) {
        if (!overlay)
            continue;
        // 主动关闭其他屏幕遮罩不代表用户取消，先断开流程回调以免重新显示主窗口。
        disconnect(overlay, nullptr, this, nullptr);
        overlay->close();
        overlay->deleteLater();
    }
}

// 结束当前录制流程：销毁控制条、提示框与管线，失败时删除半成品文件并提示。
void AppBridge::finishRecordingFlow(bool succeeded, const QString& error)
{
    if (m_recordingBar) {
        m_recordingBar->close();
        m_recordingBar->deleteLater();
    }
    m_recordingBar.clear();
    if (m_recordingFrame) {
        m_recordingFrame->close();
        m_recordingFrame->deleteLater();
    }
    m_recordingFrame.clear();
    if (m_recorder)
        m_recorder->deleteLater();
    m_recorder.clear();
    const QString path = m_recordingPath;
    m_recordingPath.clear();
    m_recordingBusy = false;
    const bool restoreWindow = std::exchange(m_recordingRestoreWindow, false);
    if (!succeeded) {
        if (!path.isEmpty())
            QFile::remove(path);
        if (!error.isEmpty())
            QMessageBox::warning(m_window, InterfaceLanguage::text(QStringLiteral("录屏")),
                InterfaceLanguage::text(QStringLiteral("录制失败：%1")).arg(error));
    }
    // 成功保存后保持主窗口隐藏；失败时仅恢复本次操作前可见的窗口。
    if (!succeeded && restoreWindow && m_window) {
        m_window->show();
    }
}

// 隐藏主窗口后逐屏抓取并显示录屏选区遮罩；确认后立即开始录制。
void AppBridge::beginRecording(bool restoreWindow)
{
    if (!m_nativeIntegration || m_recordingBusy)
        return;
    m_recordingBusy = true;
    m_recordingRestoreWindow = restoreWindow;
    if (restoreWindow && m_window)
        m_window->hide();
    // 等待窗口真正从屏幕消失后再抓取，与截图流程保持一致。
    QTimer::singleShot(restoreWindow ? 260 : 80, this, [this]() {
        struct ScreenCapture {
            QScreen* screen;
            QRect geometry;
            QImage image;
        };
        QList<ScreenCapture> captures;
        for (QScreen* screen : QGuiApplication::screens()) {
            const QImage image = screen->grabWindow(0).toImage();
            if (!image.isNull())
                captures.append({screen, screen->geometry(), image});
        }
        if (captures.isEmpty()) {
            m_recordingBusy = false;
            const bool restoreWindow = std::exchange(m_recordingRestoreWindow, false);
            if (restoreWindow && m_window)
                m_window->show();
            QMessageBox::warning(m_window, InterfaceLanguage::text(QStringLiteral("录屏")),
                InterfaceLanguage::text(QStringLiteral("无法截取屏幕内容，请重试或联系开发者。")));
            return;
        }
        for (const ScreenCapture& capture : captures) {
            auto* overlay = new RecordingOverlay(capture.image);
            overlay->setGeometry(capture.geometry);
            m_recordingOverlays.append(overlay);
            // 确认选区：关闭全部遮罩后立即开始录制，避免遮罩被录进视频。
            connect(overlay, &RecordingOverlay::confirmRequested, this,
                [this, overlay, screen = capture.screen, geometry = capture.geometry](
                    const QRectF& selection) {
                    if (!m_recordingBusy)
                        return;
                    const QSizeF overlaySize(overlay->size());
                    closeRecordingOverlays();

                    const auto reserved = m_recordings->reserve();
                    if (!reserved.value(QStringLiteral("ok")).toBool()) {
                        finishRecordingFlow(false,
                            reserved.value(QStringLiteral("error")).toString());
                        return;
                    }
                    m_recordingPath = reserved.value(QStringLiteral("data")).toMap()
                                          .value(QStringLiteral("path")).toString();

                    // 读取录屏音频选项；选项开关关闭时仅录制画面。
                    bool recordMicrophone = false;
                    bool recordSystemSound = false;
                    const auto settings = m_settings->snapshot();
                    if (settings.value(QStringLiteral("ok")).toBool()) {
                        const auto values = settings.value(QStringLiteral("data")).toMap();
                        if (values.value(QStringLiteral("recordingAudioEnabled")).toBool()) {
                            recordMicrophone = values.value(QStringLiteral("recordingAudioMicrophone")).toBool();
                            recordSystemSound = values.value(QStringLiteral("recordingAudioSystem")).toBool();
                        }
                    }

                    auto* recorder = new ScreenRecorder(this);
                    m_recorder = recorder;
                    QString error;
                    QString warning;
                    if (!recorder->start(screen, selection, overlaySize, m_recordingPath,
                            recordMicrophone, recordSystemSound, &error, &warning)) {
                        finishRecordingFlow(false, error);
                        return;
                    }

                    // 区域提示框：沿选区外侧描红框（点击穿透，画在裁剪范围外不进视频）。
                    auto* frame = new RecordingRegionFrame();
                    m_recordingFrame = frame;
                    const QRect selectionGlobal(
                        geometry.topLeft() + selection.topLeft().toPoint(),
                        QSize(int(selection.width()), int(selection.height())));
                    frame->setFrameRegion(selectionGlobal);
                    frame->show();
                    frame->raise();
                    // 个别音频来源不可用时继续录制，但明确告知用户。
                    if (!warning.isEmpty())
                        QMessageBox::warning(m_window, InterfaceLanguage::text(QStringLiteral("录屏")), warning);

                    // 控制条放在选区外（选区内出现的任何窗口都会被录进视频）。
                    auto* bar = new RecordingControlBar();
                    m_recordingBar = bar;
                    bar->placeOutside(selectionGlobal, geometry);
                    bar->show();
                    bar->raise();
                    connect(bar, &RecordingControlBar::pauseRequested, this, [this]() {
                        if (!m_recorder || !m_recordingBar)
                            return;
                        const bool paused = !m_recorder->isPaused();
                        if (paused)
                            m_recorder->pause();
                        else
                            m_recorder->resume();
                        m_recordingBar->setPaused(paused);
                        // 暂停时提示框变黄，恢复后回到红色。
                        if (m_recordingFrame)
                            m_recordingFrame->setPausedVisual(paused);
                    });
                    connect(bar, &RecordingControlBar::finishRequested, this, [this]() {
                        if (m_recordingBar)
                            m_recordingBar->setSaving();
                        if (m_recorder)
                            m_recorder->stop();
                    });
                    connect(recorder, &ScreenRecorder::tick, this, [this](qint64 milliseconds) {
                        if (m_recordingBar)
                            m_recordingBar->setElapsed(milliseconds);
                    });
                    connect(recorder, &ScreenRecorder::finished, this,
                        [this](const QVariantMap& result) {
                        const QString path = result.value(QStringLiteral("path")).toString();
                        QVariantMap meta;
                        meta.insert(QStringLiteral("recordedAt"),
                            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
                        meta.insert(QStringLiteral("width"), result.value(QStringLiteral("width")));
                        meta.insert(QStringLiteral("height"), result.value(QStringLiteral("height")));
                        meta.insert(QStringLiteral("durationMs"),
                            result.value(QStringLiteral("durationMs")));
                        const QImage thumbnail =
                            result.value(QStringLiteral("thumbnail")).value<QImage>();
                        if (!thumbnail.isNull())
                            meta.insert(QStringLiteral("thumbnail"), encodeImageDataUrl(thumbnail));
                        const auto added = m_recordings->add(path, meta);
                        finishRecordingFlow(true, {});
                        if (!added.value(QStringLiteral("ok")).toBool()) {
                            QFile::remove(path);
                            QMessageBox::warning(m_window, InterfaceLanguage::text(QStringLiteral("录屏")),
                                added.value(QStringLiteral("error")).toString());
                            return;
                        }
                        // 需求要求：完成录屏保存文件后，把视频文件复制到剪贴板。
                        copyFileToClipboard(path);
                    });
                    connect(recorder, &ScreenRecorder::failed, this,
                        [this](const QString& error) { finishRecordingFlow(false, error); });
                });
            // 任一屏幕开始重新框选时，其余屏幕遮罩的旧选区与工具栏一并收起。
            connect(overlay, &RecordingOverlay::newSelectionStarted, this, [this, overlay]() {
                for (const QPointer<RecordingOverlay>& other : std::as_const(m_recordingOverlays)) {
                    if (other && other != overlay)
                        other->dismissSelection();
                }
            });
            connect(overlay, &RecordingOverlay::cancelled, this, [this]() {
                if (!m_recordingBusy)
                    return;
                m_recordingBusy = false;
                const bool restoreWindow = std::exchange(m_recordingRestoreWindow, false);
                closeRecordingOverlays();
                if (restoreWindow && m_window)
                    m_window->show();
            });
            overlay->show();
            overlay->raise();
            overlay->activateWindow();
        }
    });
}

QVariantMap AppBridge::getNote(const QString& id) const { return m_notes->readNote(id); }
QVariantMap AppBridge::saveNote(const QVariantMap& note) { return m_notes->saveNote(note); }
QVariantMap AppBridge::deleteNotes(const QStringList& ids) { return m_notes->removeNotes(ids); }
QVariantMap AppBridge::moveNote(const QString& id, const QString& categoryId, const QString& beforeId)
{ return m_notes->moveNote(id, categoryId, beforeId); }
QVariantMap AppBridge::saveNoteCategory(const QString& id, const QString& name)
{ return m_notes->saveCategory(id, name); }
QVariantMap AppBridge::deleteNoteCategory(const QString& id) { return m_notes->removeCategory(id); }

QVariantMap AppBridge::getDownloads() const { return m_downloads->snapshot(); }

QVariantMap AppBridge::createDownload(const QVariantMap& task)
{
    if (!QJsonValue::fromVariant(task.value(QStringLiteral("url"))).isString())
        return ServiceResult::failure(QStringLiteral("请填写有效的下载网址。"));
    if (task.contains(QStringLiteral("directory"))
        && !QJsonValue::fromVariant(task.value(QStringLiteral("directory"))).isString())
        return ServiceResult::failure(QStringLiteral("下载目录必须是文件夹路径。"));
    const auto settings = m_settings->snapshot();
    if (!settings.value(QStringLiteral("ok")).toBool())
        return settings;
    return m_downloads->createTask(task.value(QStringLiteral("url")).toString(),
        task.value(QStringLiteral("directory")).toString(),
        settings.value(QStringLiteral("data")).toMap().value(QStringLiteral("downloadDirectory")).toString());
}

QVariantMap AppBridge::getDownloadFiles(const QString& id) const { return m_downloads->files(id); }
QVariantMap AppBridge::confirmDownloadFiles(const QString& id, const QVariantList& indices)
{ return m_downloads->confirmFiles(id, indices); }
QVariantMap AppBridge::pauseDownload(const QString& id) { return m_downloads->pauseTask(id); }
QVariantMap AppBridge::resumeDownload(const QString& id) { return m_downloads->resumeTask(id); }
QVariantMap AppBridge::cancelDownload(const QString& id) { return m_downloads->cancelTask(id); }
QVariantMap AppBridge::removeDownload(const QString& id) { return m_downloads->removeTask(id); }
QVariantMap AppBridge::openDownloadDirectory(const QString& id) { return m_downloads->openDirectory(id); }
bool AppBridge::hasActiveDownloads() const { return m_downloads->hasActiveTasks(); }

QVariantMap AppBridge::chooseDownloadDirectory()
{
    const QString initial = m_settings->snapshot().value(QStringLiteral("data")).toMap()
                                .value(QStringLiteral("downloadDirectory")).toString();
    const QString directory = QFileDialog::getExistingDirectory(m_window, InterfaceLanguage::text(QStringLiteral("选择下载目录")), initial);
    return ServiceResult::success(QVariantMap{{QStringLiteral("directory"), directory},
        {QStringLiteral("cancelled"), directory.isEmpty()}});
}

QVariantMap AppBridge::getSettings() const { return m_settings->snapshot(); }

// 单独持久化语言，既保留用户内容也保留其他未提交的设置。
QVariantMap AppBridge::setInterfaceLanguage(const QString& language)
{
    return m_settings->save({{QStringLiteral("language"), language}});
}

QVariantMap AppBridge::saveSettings(const QVariantMap& settings)
{
    const auto current = m_settings->snapshot();
    if (!current.value("ok").toBool())
        return current;
    // 与快照合并，兼容未传下载目录的旧调用方以及只修改单个设置。
    auto candidate = current.value(QStringLiteral("data")).toMap();
    for (auto iterator = settings.cbegin(); iterator != settings.cend(); ++iterator)
        candidate.insert(iterator.key(), iterator.value());
    const auto validated = SettingsService::validate(candidate);
    if (!validated.value("ok").toBool())
        return validated;
    const auto next = validated.value("data").toMap();
    QString error;
    if (!m_hotkey->prepareShortcut(next.value("hotkeyModifier").toInt(), next.value("hotkeyKey").toInt(), &error))
        return ServiceResult::failure(error);
    const auto saved = m_settings->save(next);
    if (!saved.value("ok").toBool()) {
        m_hotkey->cancelShortcut();
        return saved;
    }
    m_hotkey->commitShortcut();
    m_hotkeyWarning.clear();
    return saved;
}

QVariantMap AppBridge::resetSettings()
{
    return saveSettings({{"fontSize", 16}, {"hotkeyModifier", 0}, {"hotkeyKey", 0x77},
        {"downloadDirectory", QString()}, {"autoStart", false},
        {"sidebarCollapsed", false}, {"navigationItems", QVariantList{}},
        {"screenshotHotkeyModifier", 3}, {"screenshotHotkeyKey", 0x41},
        {"screenshotEnabled", true},
        {"recordingAudioEnabled", false}, {"recordingAudioMicrophone", true},
        {"recordingAudioSystem", true}});
}

QVariantMap AppBridge::openDataDirectory()
{
    if (!QDir().mkpath(m_dataRoot) || !QDesktopServices::openUrl(QUrl::fromLocalFile(m_dataRoot)))
        return ServiceResult::failure(QStringLiteral("无法打开数据目录。"));
    return ServiceResult::success();
}

QVariantMap AppBridge::openGithubRepository()
{
    // 仓库地址固定在原生侧，网页无法请求打开任意外部链接。
    static const QUrl repository(QStringLiteral("https://github.com/Damengxin-123/DesktopTools"));
    if (!QDesktopServices::openUrl(repository))
        return ServiceResult::failure(QStringLiteral("无法打开浏览器，请手动访问：") + repository.toString());
    return ServiceResult::success();
}

QVariantMap AppBridge::getAppInfo() const
{
    return ServiceResult::success(QVariantMap{{"version", QCoreApplication::applicationVersion()},
        {"dataPath", m_dataRoot}, {"hotkeyWarning", m_hotkeyWarning},
        {"screenshotHotkeyWarning", m_screenshotHotkeyWarning}});
}

// ---- 资源浏览器 ----

// 显示刚创建的原生工具窗口：最小化则还原，并置前激活。
void AppBridge::presentBrowserWindow(QWidget* window)
{
    if (window->isMinimized())
        window->showNormal();
    else
        window->show();
    window->raise();
    window->activateWindow();
}

QVariantMap AppBridge::chooseMediaFile(const QString& kind)
{
    if (kind != QStringLiteral("image") && kind != QStringLiteral("video"))
        return ServiceResult::failure(QStringLiteral("请选择图片或视频类型。"));
    QString path;
    if (kind == QStringLiteral("image")) {
        QStringList patterns;
        for (const auto& format : QImageReader::supportedImageFormats())
            patterns.append(QStringLiteral("*.") + QString::fromLatin1(format));
        path = QFileDialog::getOpenFileName(m_window, InterfaceLanguage::text(QStringLiteral("选择图片")), QString(),
            InterfaceLanguage::text(QStringLiteral("图片文件 (%1);;所有文件 (*.*)")).arg(patterns.join(QLatin1Char(' '))));
    } else {
        QStringList patterns;
        for (const QString& suffix : MediaService::videoExtensions())
            patterns.append(QStringLiteral("*.") + suffix);
        path = QFileDialog::getOpenFileName(m_window, InterfaceLanguage::text(QStringLiteral("选择视频")), QString(),
            InterfaceLanguage::text(QStringLiteral("视频文件 (%1);;所有文件 (*.*)")).arg(patterns.join(QLatin1Char(' '))));
    }
    if (path.isEmpty())
        return ServiceResult::success(QVariantMap{{"cancelled", true}});
    return ServiceResult::success(QVariantMap{{"target", path}, {"title", QFileInfo(path).fileName()}});
}

QVariantMap AppBridge::listMediaDirectory(const QString& directory) const
{
    return m_media->listDirectory(directory);
}

// 启动独立图像工具，同目录清单由工具进程读取。
QVariantMap AppBridge::openImageBrowser(const QString& path)
{
    if (!MediaService::isImageFile(path))
        return ServiceResult::failure(QStringLiteral("暂不支持该图片格式。"));
    return m_media->launchFile(path, m_dataRoot);
}

// 启动独立视频工具，关闭主程序后播放仍可继续。
QVariantMap AppBridge::openVideoPlayer(const QString& path)
{
    if (!MediaService::isVideoFile(path))
        return ServiceResult::failure(QStringLiteral("暂不支持该视频格式。"));
    return m_media->launchFile(path, m_dataRoot);
}

QVariantMap AppBridge::getMediaAssociation() const { return m_media->associationStatus(); }

// 查询单个扩展名当前生效的打开方式；供手动设置向导跳过已生效的类型。
QVariantMap AppBridge::getMediaExtensionHandler(const QString& extension) const
{
    QString normalized = extension.trimmed();
    if (!normalized.startsWith(QLatin1Char('.')))
        normalized.prepend(QLatin1Char('.'));
    const QString suffix = normalized.mid(1).toLower();
    if (!MediaService::imageExtensions().contains(suffix, Qt::CaseInsensitive)
        && !MediaService::videoExtensions().contains(suffix, Qt::CaseInsensitive))
        return ServiceResult::failure(QStringLiteral("暂不支持该文件类型：") + normalized);
    return ServiceResult::success(m_media->extensionHandler(normalized));
}
// 将选中的格式提交给系统默认应用设置入口。
QVariantMap AppBridge::configureMediaDefaults(const QStringList& extensions) { return m_media->openDefaultApplications(extensions); }
QVariantMap AppBridge::setImageBrowserDefault(bool enabled) { return m_media->setImageBrowserDefault(enabled); }
QVariantMap AppBridge::setVideoPlayerDefault(bool enabled) { return m_media->setVideoPlayerDefault(enabled); }

// 兼容旧版单格式入口；返回待确认状态，不提前宣称已更改默认程序。
QVariantMap AppBridge::openSystemOpenWith(const QString& extension)
{
    return m_media->openDefaultApplications({extension});
}

// 验证历史记录后把原始 PNG 路径交给独立图像工具。
QVariantMap AppBridge::openScreenshotViewer(const QString& id)
{
    const auto result = m_screenshots->read(id);
    if (!result.value("ok").toBool())
        return result;
    return openImageBrowser(result.value("data").toMap().value("path").toString());
}

// 历史录屏也使用独立视频工具。
QVariantMap AppBridge::openRecordingPlayer(const QString& id)
{
    const auto result = m_recordings->read(id);
    if (!result.value("ok").toBool())
        return result;
    return openVideoPlayer(result.value("data").toMap().value("path").toString());
}

// ---- 文件检索 ----

QVariantMap AppBridge::getFileSearchStatus() const
{
    return ServiceResult::success(m_fileSearch->statusSnapshot());
}

QVariantMap AppBridge::searchFiles(const QString& query, int offset, int limit, bool searchPath)
{
    return m_fileSearch->search(query, offset, limit, searchPath);
}

// 启用受限 NTFS 权限代理，主窗口仍使用普通用户权限。
QVariantMap AppBridge::enableFileSearchService()
{
    return m_fileSearch->enableJournalService();
}

QVariantMap AppBridge::rebuildFileSearchIndex()
{
    return m_fileSearch->rebuild();
}

// 图片与视频使用独立资源工具，其余交给系统默认程序。
QVariantMap AppBridge::openFilePath(const QString& path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return ServiceResult::failure(QStringLiteral("文件不存在，可能已被移动或删除。"));
    if (info.isFile()) {
        if (MediaService::isImageFile(info.absoluteFilePath()))
            return openImageBrowser(info.absoluteFilePath());
        if (MediaService::isVideoFile(info.absoluteFilePath()))
            return openVideoPlayer(info.absoluteFilePath());
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(info.absoluteFilePath())))
        return ServiceResult::failure(QStringLiteral("无法打开该文件或目录。"));
    return ServiceResult::success();
}

// ---- 地形编辑 ----

// 同一主窗口始终复用当前画板，避免再次点击入口丢失画作。
QVariantMap AppBridge::openPaintWindow()
{
    if (!m_paintWindow) m_paintWindow = new PaintWindow(m_dataRoot, m_window);
    presentBrowserWindow(m_paintWindow);
    return ServiceResult::success();
}

// 主程序与独立工具通过同一项目目录交换数据。
QVariantMap AppBridge::getLevelLayouts() const
{
    return ServiceResult::success(LevelDocument::catalog(m_dataRoot));
}

// 校验目录内文件标识，避免网页接口读取任意路径。
QVariantMap AppBridge::openLevelLayout(const QString& id)
{
    QString path;
    if (!id.isEmpty()) {
        if (id != QFileInfo(id).fileName() || id.contains('/') || id.contains('\\')
            || !id.endsWith(".level.json") || id.contains(':'))
            return ServiceResult::failure(QStringLiteral("关卡项目标识无效。"));
        path = QDir(m_dataRoot).absoluteFilePath("level-layouts/" + id);
        if (QFileInfo(path).isSymLink())
            return ServiceResult::failure(QStringLiteral("不支持链接形式的项目文件。"));
        for (auto* editor : m_window->findChildren<LevelWindow*>()) {
            if (editor->projectPath() == path) {
                presentBrowserWindow(editor); return ServiceResult::success();
            }
        }
    }
    auto* editor = new LevelWindow(m_dataRoot, m_window);
    if (!path.isEmpty()) {
        QString error;
        if (!editor->openProject(path, error)) { delete editor; return ServiceResult::failure(error); }
    }
    presentBrowserWindow(editor);
    return ServiceResult::success();
}

// 导入时校验并写入新标识，外部原件保持独立。
QVariantMap AppBridge::importLevelLayout()
{
    const QString source = QFileDialog::getOpenFileName(m_window, QStringLiteral("导入关卡项目"),
        m_dataRoot, QStringLiteral("关卡项目 (*.level.json)"));
    if (source.isEmpty()) return ServiceResult::success();
    LevelDocument document;
    QString error;
    if (!LevelDocument::read(source, document, error)) return ServiceResult::failure(error);
    const QString directory = QDir(m_dataRoot).filePath("level-layouts");
    if (!QDir().mkpath(directory)) return ServiceResult::failure(QStringLiteral("无法创建关卡项目目录。"));
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces) + ".level.json";
    if (!document.write(QDir(directory).filePath(id), error)) return ServiceResult::failure(error);
    return openLevelLayout(id);
}

QVariantMap AppBridge::getTerrains() const { return m_terrain->snapshot(); }

QVariantMap AppBridge::createTerrain(const QString& title, double widthMeters,
                                     double lengthMeters, double spacing, double initialHeight)
{
    return m_terrain->create(title, widthMeters, lengthMeters, spacing, initialHeight);
}

QVariantMap AppBridge::renameTerrain(const QString& id, const QString& title)
{
    return m_terrain->rename(id, title);
}

QVariantMap AppBridge::deleteTerrains(const QStringList& ids)
{
    // 编辑窗口打开中的项目不得静默删除；网页侧的确认弹窗只覆盖未打开的情况。
    for (const QString& id : ids) {
        if (TerrainEditWindow::isProjectOpen(id))
            return ServiceResult::failure(QStringLiteral(
                "选中的地形项目正在编辑窗口中打开，请先关闭对应编辑窗口后再删除。"));
    }
    return m_terrain->remove(ids);
}

QVariantMap AppBridge::openTerrainEditor(const QString& id)
{
    if (TerrainEditWindow::isProjectOpen(id)) {
        TerrainEditWindow::activateExisting(id);
        return ServiceResult::success();
    }
    auto* editor = TerrainEditWindow::open(m_terrain, id, m_window);
    if (!editor)
        return ServiceResult::failure(QStringLiteral("无法打开地形编辑窗口，项目可能已被删除或损坏。"));
    presentBrowserWindow(editor);
    return ServiceResult::success();
}

QVariantMap AppBridge::getTerrainPreview(const QString& id) const
{
    return m_terrain->readPreview(id, 145);
}

QVariantMap AppBridge::getTerrainThumbnail(const QString& id) const
{
    return m_terrain->readThumbnail(id);
}

QVariantMap AppBridge::openFileLocation(const QString& path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return ServiceResult::failure(QStringLiteral("文件不存在，可能已被移动或删除。"));
#ifdef Q_OS_WIN
    // 资源管理器 /select 参数打开目录并选中该文件。
    if (!QProcess::startDetached(QStringLiteral("explorer.exe"),
            {QStringLiteral("/select,") + QDir::toNativeSeparators(info.absoluteFilePath())}))
        return ServiceResult::failure(QStringLiteral("无法打开文件所在目录。"));
#else
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath())))
        return ServiceResult::failure(QStringLiteral("无法打开文件所在目录。"));
#endif
    return ServiceResult::success();
}

// 命令行/单实例的本地文件打开入口；不支持的类型给出明确提示。
void AppBridge::openLocalFile(const QString& path)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        QMessageBox::warning(m_window, InterfaceLanguage::text(QStringLiteral("资源浏览器")),
            InterfaceLanguage::text(QStringLiteral("文件不存在，可能已被移动或删除：\n%1")).arg(path));
        return;
    }
    QVariantMap result;
    if (MediaService::isImageFile(info.absoluteFilePath())) {
        result = openImageBrowser(info.absoluteFilePath());
        if (!result.value("ok").toBool())
            QMessageBox::warning(m_window, InterfaceLanguage::text(QStringLiteral("图像浏览器")),
                result.value("error").toString());
        return;
    }
    if (MediaService::isVideoFile(info.absoluteFilePath())) {
        result = openVideoPlayer(info.absoluteFilePath());
        if (!result.value("ok").toBool())
            QMessageBox::warning(m_window, InterfaceLanguage::text(QStringLiteral("视频播放器")),
                result.value("error").toString());
        return;
    }
    QMessageBox::warning(m_window, InterfaceLanguage::text(QStringLiteral("资源浏览器")),
        InterfaceLanguage::text(QStringLiteral("暂不支持直接打开此文件类型：\n%1")).arg(path));
}
