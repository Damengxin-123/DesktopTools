#include "AppBridge.h"

#include "app/GlobalHotkey.h"
#include "app/ScreenshotOverlay.h"
#include "services/DownloadService.h"
#include "services/ClipboardService.h"
#include "services/EmojiService.h"
#include "services/NoteService.h"
#include "services/QrService.h"
#include "services/ScreenshotService.h"
#include "services/ServiceResult.h"
#include "services/SettingsService.h"
#include "services/ShortcutService.h"
#include "services/WallpaperService.h"
#include "services/GridMapService.h"
#include <QApplication>
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
#include <QSaveFile>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QWidget>

namespace
{
// 网格图导出数据允许的最大 base64 长度。
constexpr qint64 MaximumGridPngChars = 96 * 1024 * 1024;
// 二维码图片数据地址与像素上限，与剪贴板服务保持一致的防护边界。
constexpr qint64 MaximumImageDataUrlChars = 16 * 1024 * 1024;
constexpr qint64 MaximumImagePixels = 20000000;
constexpr qint64 MaximumClipboardImageBytes = 8 * 1024 * 1024;

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
      m_settings(new SettingsService(m_dataRoot, this, startupSettings(nativeIntegration, this))),
      m_downloads(new DownloadService(m_dataRoot, this)),
      m_clipboard(new ClipboardService(m_dataRoot, nativeIntegration, this)),
      m_emoji(new EmojiService(m_dataRoot, m_clipboard, this)),
      m_wallpaper(new WallpaperService(m_dataRoot, nativeIntegration, this)),
      m_gridmaps(new GridMapService(m_dataRoot, this)),
      m_qr(new QrService(m_dataRoot, this)),
      m_screenshots(new ScreenshotService(m_dataRoot, this)),
      m_hotkey(new GlobalHotkey(nativeIntegration, 0x4D01, this)),
      m_screenshotHotkey(new GlobalHotkey(nativeIntegration, 0x4D03, this)),
      m_nativeIntegration(nativeIntegration)
{
    connect(m_shortcuts, &ShortcutService::changed, this, &AppBridge::shortcutsChanged);
    connect(m_notes, &NoteService::changed, this, &AppBridge::notesChanged);
    connect(m_settings, &SettingsService::changed, this, &AppBridge::settingsChanged);
    connect(m_downloads, &DownloadService::changed, this, &AppBridge::downloadsChanged);
    connect(m_clipboard, &ClipboardService::changed, this, &AppBridge::clipboardChanged);
    connect(m_emoji, &EmojiService::changed, this, &AppBridge::emojisChanged);
    connect(m_wallpaper, &WallpaperService::changed, this, &AppBridge::wallpaperChanged);
    connect(m_gridmaps, &GridMapService::changed, this, &AppBridge::gridMapsChanged);
    connect(m_qr, &QrService::changed, this, &AppBridge::qrHistoryChanged);
    connect(m_screenshots, &ScreenshotService::changed, this, &AppBridge::screenshotsChanged);
    connect(m_hotkey, &GlobalHotkey::activated, this, &AppBridge::activateWindowRequested);
    connect(m_screenshotHotkey, &GlobalHotkey::activated, this, [this]() { beginScreenshot(true); });
    const auto result = m_settings->snapshot();
    if (result.value("ok").toBool()) {
        const auto settings = result.value("data").toMap();
        m_hotkey->setShortcut(settings.value("hotkeyModifier").toInt(),
                             settings.value("hotkeyKey").toInt(), &m_hotkeyWarning);
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
QVariantMap AppBridge::saveShortcutCategory(const QString& id, const QString& name)
{ return m_shortcuts->saveCategory(id, name); }
QVariantMap AppBridge::deleteShortcutCategory(const QString& id) { return m_shortcuts->removeCategory(id); }
QVariantMap AppBridge::openShortcut(const QString& id) { return m_shortcuts->openShortcut(id); }

QVariantMap AppBridge::chooseTarget(int type)
{
    if (type != 0 && type != 2)
        return ServiceResult::failure(QStringLiteral("请选择文件或目录类型。"));
    const QString path = type == 0
        ? QFileDialog::getExistingDirectory(m_window, QStringLiteral("选择目录"))
        : QFileDialog::getOpenFileName(m_window, QStringLiteral("选择文件"));
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
    const QString path = QFileDialog::getOpenFileName(m_window, QStringLiteral("选择表情图片"), QString(),
        QStringLiteral("图片文件 (%1)").arg(patterns.join(QLatin1Char(' '))));
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
    const QString path = QFileDialog::getOpenFileName(m_window, QStringLiteral("选择动态壁纸资源"), QString(),
        QStringLiteral("图片和视频 (%1);;所有文件 (*.*)").arg(patterns));
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
    QString path = QFileDialog::getSaveFileName(m_window, QStringLiteral("导出网格图 PNG"),
        suggested, QStringLiteral("PNG 图片 (*.png)"));
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
    const QString path = QFileDialog::getOpenFileName(m_window, QStringLiteral("选择二维码图片"), QString(),
        QStringLiteral("图片文件 (%1)").arg(patterns.join(QLatin1Char(' '))));
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

// 校验并保存截图热键；先预留注册，保存成功后才启用，失败时回滚。
QVariantMap AppBridge::saveScreenshotHotkey(int modifier, int key)
{
    QString error;
    if (!m_screenshotHotkey->prepareShortcut(modifier, key, &error))
        return ServiceResult::failure(error);
    const auto saved = m_settings->save({{"screenshotHotkeyModifier", modifier}, {"screenshotHotkeyKey", key}});
    if (!saved.value("ok").toBool()) {
        m_screenshotHotkey->cancelShortcut();
        return saved;
    }
    m_screenshotHotkey->commitShortcut();
    m_screenshotHotkeyWarning.clear();
    return saved;
}

// 抓取整个虚拟桌面的图像。
QImage AppBridge::grabDesktopImage() const
{
    QScreen* screen = QGuiApplication::primaryScreen();
    if (!screen)
        return QImage();
    return screen->grabWindow(0).toImage();
}

// 隐藏主窗口后抓取桌面并显示遮罩；结束后恢复剪贴板、历史与窗口可见性。
void AppBridge::beginScreenshot(bool restoreWindow)
{
    if (!m_nativeIntegration || m_screenshotBusy)
        return;
    m_screenshotBusy = true;
    if (restoreWindow && m_window)
        m_window->hide();
    // 等待窗口真正从屏幕消失后再抓取，避免把本程序截进图里。
    QTimer::singleShot(restoreWindow ? 260 : 80, this, [this, restoreWindow]() {
        const QImage desktop = grabDesktopImage();
        if (desktop.isNull()) {
            m_screenshotBusy = false;
            if (restoreWindow && m_window)
                m_window->show();
            QMessageBox::warning(m_window, QStringLiteral("截图"),
                QStringLiteral("无法截取屏幕内容，请重试或联系开发者。"));
            return;
        }
        auto* overlay = new ScreenshotOverlay(desktop);
        m_overlay = overlay;
        connect(overlay, &ScreenshotOverlay::finished, this,
            [this, overlay, restoreWindow](const QImage& image, bool cancelled) {
            Q_UNUSED(cancelled)
            m_screenshotBusy = false;
            m_overlay.clear();
            overlay->deleteLater();
            if (!image.isNull()) {
                QApplication::clipboard()->setImage(image);
                m_screenshots->add(image, QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
            }
            if (restoreWindow && m_window)
                m_window->show();
        });
        overlay->show();
        overlay->raise();
        overlay->activateWindow();
    });
}

// 由网页按钮或热键触发一次交互式截图。
QVariantMap AppBridge::startScreenshot()
{
    if (!m_nativeIntegration)
        return ServiceResult::failure(QStringLiteral("当前环境不支持截图功能。"));
    if (m_screenshotBusy || !m_overlay.isNull())
        return ServiceResult::failure(QStringLiteral("截图正在进行中，请先完成或取消。"));
    beginScreenshot(m_window && m_window->isVisible());
    return ServiceResult::success();
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
    const QString directory = QFileDialog::getExistingDirectory(m_window, QStringLiteral("选择下载目录"), initial);
    return ServiceResult::success(QVariantMap{{QStringLiteral("directory"), directory},
        {QStringLiteral("cancelled"), directory.isEmpty()}});
}

QVariantMap AppBridge::getSettings() const { return m_settings->snapshot(); }

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
        {"screenshotHotkeyModifier", 3}, {"screenshotHotkeyKey", 0x41}});
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
