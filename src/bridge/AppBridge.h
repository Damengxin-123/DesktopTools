#pragma once

#include <QObject>
#include <QPointer>
#include <QVariantMap>
#include <QStringList>

class GlobalHotkey;
class DownloadService;
class ClipboardService;
class EmojiService;
class NoteService;
class SettingsService;
class ShortcutService;
class WallpaperService;
class GridMapService;
class QrService;
class ScreenshotService;
class ScreenshotOverlay;
class QImage;
class QWidget;

template<typename T> class QPointer;

// HTML 页面的唯一原生接口，转发业务操作并处理系统交互。
class AppBridge final : public QObject
{
    Q_OBJECT
public:
    // 创建服务并加载兼容数据；测试可关闭热键、自启项和剪贴板等系统操作。
    explicit AppBridge(const QString& dataRoot, QWidget* window, bool nativeIntegration = true);
    // 返回快捷方式和分类快照。
    Q_INVOKABLE QVariantMap getShortcuts() const;
    // 新增或修改快捷方式。
    Q_INVOKABLE QVariantMap saveShortcut(const QVariantMap& item);
    // 批量删除指定快捷方式。
    Q_INVOKABLE QVariantMap deleteShortcuts(const QStringList& ids);
    // 移动条目到指定分类与顺序。
    Q_INVOKABLE QVariantMap moveShortcut(const QString& id, const QString& categoryId, const QString& beforeId);
    // 新增或重命名快捷方式分类。
    Q_INVOKABLE QVariantMap saveShortcutCategory(const QString& id, const QString& name);
    // 删除分类并保留其中的条目。
    Q_INVOKABLE QVariantMap deleteShortcutCategory(const QString& id);
    // 使用系统关联程序打开已保存的快捷方式。
    Q_INVOKABLE QVariantMap openShortcut(const QString& id);
    // 显示原生文件或目录选择器。
    Q_INVOKABLE QVariantMap chooseTarget(int type);
    // 将文本复制到系统剪贴板。
    Q_INVOKABLE QVariantMap copyText(const QString& text);
    // 查询剪贴板摘要，可搜索完整文本、文件名及路径。
    Q_INVOKABLE QVariantMap getClipboardHistory(const QString& query) const;
    // 保存监听类型，全部取消勾选时暂停记录。
    Q_INVOKABLE QVariantMap setClipboardTypes(const QStringList& types);
    // 读取完整剪贴板记录。
    Q_INVOKABLE QVariantMap getClipboardItem(const QString& id) const;
    // 将历史内容恢复到系统剪贴板。
    Q_INVOKABLE QVariantMap copyClipboardItem(const QString& id);
    // 设置或取消历史记录的置顶状态。
    Q_INVOKABLE QVariantMap pinClipboardItem(const QString& id, bool pinned);
    // 批量删除历史记录，保留引用的原文件。
    Q_INVOKABLE QVariantMap deleteClipboardItems(const QStringList& ids);
    // 清空历史并保留监听类型。
    Q_INVOKABLE QVariantMap clearClipboardHistory();
    // 打开指定文件引用的所在目录。
    Q_INVOKABLE QVariantMap openClipboardDirectory(const QString& id, int fileIndex);
    // 返回表情分类和列表，可按文件名与检索关键字过滤。
    Q_INVOKABLE QVariantMap getEmojis(const QString& query) const;
    // 显示原生图片选择器，返回路径和是否取消的标志。
    Q_INVOKABLE QVariantMap chooseEmojiImage();
    // 把当前剪贴板中的图像或复制的单个文件转换为新表情的来源路径。
    Q_INVOKABLE QVariantMap pasteEmojiImage();
    // 添加本地表情图片并设置检索关键字。
    Q_INVOKABLE QVariantMap addEmoji(const QVariantMap& item);
    // 修改表情的检索关键字和所属分类。
    Q_INVOKABLE QVariantMap saveEmoji(const QVariantMap& item);
    // 批量删除表情记录，保留原文件。
    Q_INVOKABLE QVariantMap deleteEmojis(const QStringList& ids);
    // 新增或重命名表情分类。
    Q_INVOKABLE QVariantMap saveEmojiCategory(const QString& id, const QString& name);
    // 删除表情分类，其中的表情移入默认分类。
    Q_INVOKABLE QVariantMap deleteEmojiCategory(const QString& id);
    // 打开表情原文件所在目录并选中该文件。
    Q_INVOKABLE QVariantMap openEmojiDirectory(const QString& id);
    // 复制表情原文件到剪贴板，不记录到剪贴板历史。
    Q_INVOKABLE QVariantMap copyEmojiFile(const QString& id);
    // 返回动态壁纸开关状态和历史记录。
    Q_INVOKABLE QVariantMap getWallpaper() const;
    // 返回网格图项目列表。
    Q_INVOKABLE QVariantMap getGridMaps() const;
    // 新建空白网格图项目。
    Q_INVOKABLE QVariantMap createGridMap(const QString& title);
    // 重命名网格图项目。
    Q_INVOKABLE QVariantMap renameGridMap(const QString& id, const QString& title);
    // 批量删除网格图项目及其栅格数据。
    Q_INVOKABLE QVariantMap deleteGridMap(const QStringList& ids);
    // 读取网格图项目的栅格数据与线条设置。
    Q_INVOKABLE QVariantMap getGridMap(const QString& id) const;
    // 保存网格图栅格与线条设置。
    Q_INVOKABLE QVariantMap saveGridMap(const QString& id, const QVariantMap& data);
    // 把网页渲染的 PNG 数据保存为本地文件。
    Q_INVOKABLE QVariantMap exportGridMapPng(const QString& title, const QString& imageDataUrl);
    // 把网页渲染的 PNG 数据复制到系统剪贴板。
    Q_INVOKABLE QVariantMap copyGridMapPng(const QString& imageDataUrl);
    // 返回二维码识别历史摘要。
    Q_INVOKABLE QVariantMap getQrHistory() const;
    // 显示原生图片选择器，返回二维码图片的数据地址。
    Q_INVOKABLE QVariantMap chooseQrImage();
    // 读取系统剪贴板中的图片，返回二维码图片的数据地址。
    Q_INVOKABLE QVariantMap pasteQrImage();
    // 识别二维码图片并把成功结果记入历史。
    Q_INVOKABLE QVariantMap decodeQr(const QString& imageDataUrl, const QString& source);
    // 读取完整识别记录，包含原图数据地址。
    Q_INVOKABLE QVariantMap readQr(const QString& id) const;
    // 批量删除识别历史。
    Q_INVOKABLE QVariantMap removeQrHistory(const QStringList& ids);
    // 清空识别历史。
    Q_INVOKABLE QVariantMap clearQrHistory();
    // 触发一次交互式截图（全屏遮罩选区并标注），完成后写入剪贴板与历史。
    Q_INVOKABLE QVariantMap startScreenshot();
    // 校验并保存截图热键，注册成功后写入设置，失败时回滚注册。
    Q_INVOKABLE QVariantMap saveScreenshotHotkey(int modifier, int key);
    // 返回截图历史摘要（缩略图、时间、尺寸与大小）。
    Q_INVOKABLE QVariantMap getScreenshotHistory() const;
    // 读取完整截图记录，包含原图数据地址。
    Q_INVOKABLE QVariantMap getScreenshot(const QString& id) const;
    // 批量删除截图历史及其 PNG 文件。
    Q_INVOKABLE QVariantMap deleteScreenshots(const QStringList& ids);
    // 清空截图历史与文件。
    Q_INVOKABLE QVariantMap clearScreenshotHistory();
    // 打开截图文件所在目录。
    Q_INVOKABLE QVariantMap openScreenshotDirectory(const QString& id);
    // 把历史中的截图重新复制到系统剪贴板。
    Q_INVOKABLE QVariantMap copyScreenshot(const QString& id);
    // 显示原生媒体选择器，返回路径和是否取消的标志。
    Q_INVOKABLE QVariantMap chooseWallpaperResource();
    // 把当前剪贴板中的图像或复制的单个文件转换为壁纸资源路径。
    Q_INVOKABLE QVariantMap pasteWallpaperResource();
    // 添加本地壁纸资源并设为当前壁纸。
    Q_INVOKABLE QVariantMap addWallpaper(const QVariantMap& item);
    // 切换到历史中的指定壁纸，源文件丢失时返回明确错误。
    Q_INVOKABLE QVariantMap useWallpaper(const QString& id);
    // 把历史壁纸应用到指定显示器，不影响其他屏幕。
    Q_INVOKABLE QVariantMap useWallpaperOnScreen(const QString& id, const QString& screenId);
    // 撤回指定屏幕的壁纸窗口，恢复系统壁纸。
    Q_INVOKABLE QVariantMap clearScreenWallpaper(const QString& screenId);
    // 独立保存并应用指定显示器的图片显示方式。
    Q_INVOKABLE QVariantMap setWallpaperDisplayMode(const QString& screenId, const QString& mode);
    // 批量删除壁纸历史记录，保留原文件。
    Q_INVOKABLE QVariantMap removeWallpapers(const QStringList& ids);
    // 开启或关闭动态壁纸，开启时恢复上次的资源。
    Q_INVOKABLE QVariantMap setWallpaperEnabled(bool enabled);
    // 打开壁纸原文件所在目录并选中该文件。
    Q_INVOKABLE QVariantMap openWallpaperDirectory(const QString& id);
    // 返回便签列表与分类。
    Q_INVOKABLE QVariantMap getNotes() const;
    // 读取指定便签的安全 HTML 内容。
    Q_INVOKABLE QVariantMap getNote(const QString& id) const;
    // 保存便签标题、分类与内容。
    Q_INVOKABLE QVariantMap saveNote(const QVariantMap& note);
    // 批量移除便签索引并保留磁盘原件。
    Q_INVOKABLE QVariantMap deleteNotes(const QStringList& ids);
    // 修改便签所属分类或排序。
    Q_INVOKABLE QVariantMap moveNote(const QString& id, const QString& categoryId, const QString& beforeId);
    // 新增或重命名便签分类。
    Q_INVOKABLE QVariantMap saveNoteCategory(const QString& id, const QString& name);
    // 删除便签分类并保留其中的便签。
    Q_INVOKABLE QVariantMap deleteNoteCategory(const QString& id);
    // 返回下载任务和历史记录。
    Q_INVOKABLE QVariantMap getDownloads() const;
    // 使用网址和可选目录创建下载任务。
    Q_INVOKABLE QVariantMap createDownload(const QVariantMap& task);
    // 获取磁力任务的文件清单，供网页显示勾选窗口。
    Q_INVOKABLE QVariantMap getDownloadFiles(const QString& id) const;
    // 确认磁力任务的文件索引，校验通过后才开始下载内容。
    Q_INVOKABLE QVariantMap confirmDownloadFiles(const QString& id, const QVariantList& indices);
    // 暂停指定任务并保留已下载内容。
    Q_INVOKABLE QVariantMap pauseDownload(const QString& id);
    // 继续已暂停或失败的下载任务。
    Q_INVOKABLE QVariantMap resumeDownload(const QString& id);
    // 取消指定下载任务。
    Q_INVOKABLE QVariantMap cancelDownload(const QString& id);
    // 移除指定下载任务的历史记录。
    Q_INVOKABLE QVariantMap removeDownload(const QString& id);
    // 通过资源管理器打开任务的下载目录。
    Q_INVOKABLE QVariantMap openDownloadDirectory(const QString& id);
    // 选择现有下载目录，返回目录和是否取消的标志。
    Q_INVOKABLE QVariantMap chooseDownloadDirectory();
    // 查询是否有下载正在进行，供原生退出确认使用。
    bool hasActiveDownloads() const;
    // 读取统一设置。
    Q_INVOKABLE QVariantMap getSettings() const;
    // 校验、切换系统热键并持久化设置，失败时恢复旧组合。
    Q_INVOKABLE QVariantMap saveSettings(const QVariantMap& settings);
    // 恢复默认字号、F8 热键和系统下载目录，并关闭开机自启。
    Q_INVOKABLE QVariantMap resetSettings();
    // 使用资源管理器打开数据目录。
    Q_INVOKABLE QVariantMap openDataDirectory();
    // 使用系统浏览器打开本项目的 GitHub 仓库。
    Q_INVOKABLE QVariantMap openGithubRepository();
    // 返回版本、数据目录和热键启动状态。
    Q_INVOKABLE QVariantMap getAppInfo() const;
signals:
    // 快捷方式数据已持久化。
    void shortcutsChanged();
    // 便签数据已持久化。
    void notesChanged();
    // 设置已持久化。
    void settingsChanged();
    // 下载任务状态、进度或历史记录发生变化。
    void downloadsChanged();
    // 剪贴板历史、监听设置或保存提示已变化。
    void clipboardChanged();
    // 表情库或分类已持久化。
    void emojisChanged();
    // 动态壁纸开关或历史已持久化。
    void wallpaperChanged();
    // 网格图项目或数据已持久化。
    void gridMapsChanged();
    // 二维码识别历史已持久化。
    void qrHistoryChanged();
    // 截图历史已持久化。
    void screenshotsChanged();
    // 热键请求唤醒主窗口。
    void activateWindowRequested();
private:
    // 当前数据根目录。
    QString m_dataRoot;
    // 原生对话框的所属窗口。
    QWidget* m_window;
    // 快捷方式服务，由本对象管理生命周期。
    ShortcutService* m_shortcuts;
    // 便签服务，由本对象管理生命周期。
    NoteService* m_notes;
    // 设置服务，由本对象管理生命周期。
    SettingsService* m_settings;
    // 下载任务服务，由本对象管理生命周期。
    DownloadService* m_downloads;
    // 剪贴板监听和持久化服务。
    ClipboardService* m_clipboard;
    // 表情分类和检索服务。
    EmojiService* m_emoji;
    // 动态壁纸历史和桌面挂载服务。
    WallpaperService* m_wallpaper;
    // 网格图项目与栅格数据服务。
    GridMapService* m_gridmaps;
    // 二维码识别历史服务。
    QrService* m_qr;
    // 截图历史服务。
    ScreenshotService* m_screenshots;
    // 截图全局热键注册器。
    GlobalHotkey* m_screenshotHotkey;
    // 启动时截图热键注册失败的说明。
    QString m_screenshotHotkeyWarning;
    // 正在显示的截图遮罩；确认或取消后置空。
    QPointer<ScreenshotOverlay> m_overlay;
    // 截图流程进行中，避免重复触发。
    bool m_screenshotBusy = false;
    // 是否启用系统级交互（热键注册、截屏遮罩）。
    bool m_nativeIntegration;
    // 全局热键注册器。
    GlobalHotkey* m_hotkey;
    // 启动时热键注册失败的说明。
    QString m_hotkeyWarning;
    // 隐藏主窗口后抓取桌面并显示遮罩；完成后恢复剪贴板、历史与窗口。
    void beginScreenshot(bool restoreWindow);
    // 抓取整个虚拟桌面的图像。
    QImage grabDesktopImage() const;
};
