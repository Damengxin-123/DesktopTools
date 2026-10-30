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
class BookmarkService;
class SettingsService;
class ShortcutService;
class WallpaperService;
class GridMapService;
class QrService;
class ScreenshotService;
class RecordingService;
class MediaService;
class FileSearchService;
class TerrainService;
class BookmarkWindow;
class PaintWindow;
class ScreenshotOverlay;
class RecordingOverlay;
class RecordingControlBar;
class RecordingRegionFrame;
class ScreenRecorder;
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
    // 命令行/单实例的本地文件打开入口：按类型分流到图像或视频浏览器。
    // 仅原生方法（非 Q_INVOKABLE），网页侧无法调用。
    void openLocalFile(const QString& path);
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
    // 返回书签列表，供主界面显示数量。
    Q_INVOKABLE QVariantMap getBookmarks() const;
    // 惰性创建内嵌书签控件；左侧管理书签，右侧浏览网页并可上传下载。
    Q_INVOKABLE QVariantMap openBookmarkWindow();
    // 打开或激活唯一画板窗口，保留正在编辑的作品。
    Q_INVOKABLE QVariantMap openPaintWindow();
    // 列出共享目录内的关卡矢量项目。
    Q_INVOKABLE QVariantMap getLevelLayouts() const;
    // 打开或激活关卡编辑窗口，空标识表示新项目。
    Q_INVOKABLE QVariantMap openLevelLayout(const QString& id);
    // 从外部文件导入一份独立项目副本，并打开编辑。
    Q_INVOKABLE QVariantMap importLevelLayout();
    /** 同步主页面书签区域的可见性与原生子控件边界。 */
    Q_INVOKABLE QVariantMap setBookmarkViewport(bool visible, int x, int y, int width, int height);
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
    // 开启或关闭截图功能；关闭时解除全局热键，开启时按已保存的组合重新注册。
    Q_INVOKABLE QVariantMap setScreenshotEnabled(bool enabled);
    // 触发一次交互式截图（全屏遮罩选区并标注），完成后写入剪贴板与历史。
    Q_INVOKABLE QVariantMap startScreenshot();
    // 触发一次交互式录屏（全屏遮罩拖拽选区），结束后保存到数据目录并复制文件。
    Q_INVOKABLE QVariantMap startRecording();
    // 返回录屏历史摘要（缩略图、时间、时长、尺寸与大小）。
    Q_INVOKABLE QVariantMap getRecordingHistory() const;
    // 读取完整录屏记录，包含磁盘路径。
    Q_INVOKABLE QVariantMap getRecording(const QString& id) const;
    // 批量删除录屏历史及其视频文件（网页侧先弹窗确认）。
    Q_INVOKABLE QVariantMap deleteRecordings(const QStringList& ids);
    // 清空录屏历史与文件。
    Q_INVOKABLE QVariantMap clearRecordingHistory();
    // 打开录屏文件所在目录。
    Q_INVOKABLE QVariantMap openRecordingDirectory(const QString& id);
    // 把历史中的录屏视频文件复制到系统剪贴板。
    Q_INVOKABLE QVariantMap copyRecording(const QString& id);
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
    // 即时保存界面语言，不提交其他设置字段或重新注册热键。
    Q_INVOKABLE QVariantMap setInterfaceLanguage(const QString& language);
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
    // 显示原生图片或视频选择器（kind 为 "image" 或 "video"）。
    Q_INVOKABLE QVariantMap chooseMediaFile(const QString& kind);
    // 列出一个目录中的图片与视频文件（非递归）。
    Q_INVOKABLE QVariantMap listMediaDirectory(const QString& directory) const;
    // 使用独立图像工具打开指定图片，可切换同目录图片。
    Q_INVOKABLE QVariantMap openImageBrowser(const QString& path);
    // 使用独立视频工具播放指定视频，可切换同目录视频。
    Q_INVOKABLE QVariantMap openVideoPlayer(const QString& path);
    // 返回图像/视频浏览器是否已是系统默认打开方式。
    Q_INVOKABLE QVariantMap getMediaAssociation() const;
    // 旧版接口仅返回引导提示，不修改默认关联。
    Q_INVOKABLE QVariantMap setImageBrowserDefault(bool enabled);
    // 按勾选的类型进入 Windows 默认应用设置。
    Q_INVOKABLE QVariantMap configureMediaDefaults(const QStringList& extensions);
    // 旧版接口仅返回引导提示，不修改默认关联。
    Q_INVOKABLE QVariantMap setVideoPlayerDefault(bool enabled);
    // 兼容单格式入口，返回系统设置已打开、等待用户确认的状态。
    Q_INVOKABLE QVariantMap openSystemOpenWith(const QString& extension);
    // 查询单个扩展名当前生效的打开方式（progId、name、ours）。
    Q_INVOKABLE QVariantMap getMediaExtensionHandler(const QString& extension) const;
    // 使用独立图像工具查看历史截图。
    Q_INVOKABLE QVariantMap openScreenshotViewer(const QString& id);
    // 使用独立视频工具播放历史录屏。
    Q_INVOKABLE QVariantMap openRecordingPlayer(const QString& id);
    // 返回文件检索索引状态（阶段、模式、各卷进度与计数）。
    Q_INVOKABLE QVariantMap getFileSearchStatus() const;
    // 提交一次全盘文件名检索，结果经 fileSearchResults 信号异步返回。
    Q_INVOKABLE QVariantMap searchFiles(const QString& query, int offset, int limit, bool searchPath);
    // 丢弃当前索引并重建。
    Q_INVOKABLE QVariantMap rebuildFileSearchIndex();
    // 用户主动启用 NTFS 高速索引服务，Windows 会显示管理员授权。
    Q_INVOKABLE QVariantMap enableFileSearchService();
    // 打开检索结果：图片与视频走独立工具，其余交给系统默认程序。
    Q_INVOKABLE QVariantMap openFilePath(const QString& path);
    // 在资源管理器中定位检索结果并选中。
    Q_INVOKABLE QVariantMap openFileLocation(const QString& path);
    // ---- 地形编辑 ----
    // 返回地形项目摘要列表。
    Q_INVOKABLE QVariantMap getTerrains() const;
    // 新建平坦地形项目；尺寸不能整除采样间距时返回调整后的实际尺寸。
    Q_INVOKABLE QVariantMap createTerrain(const QString& title, double widthMeters,
                                          double lengthMeters, double spacing,
                                          double initialHeight);
    // 重命名地形项目。
    Q_INVOKABLE QVariantMap renameTerrain(const QString& id, const QString& title);
    // 批量删除地形项目（网页侧先确认；编辑窗口打开中的项目会被拒绝）。
    Q_INVOKABLE QVariantMap deleteTerrains(const QStringList& ids);
    // 打开或激活独立地形编辑窗口。
    Q_INVOKABLE QVariantMap openTerrainEditor(const QString& id);
    // 返回降采样的预览高度数据（小端 Float32），供主页面三维缩略预览。
    Q_INVOKABLE QVariantMap getTerrainPreview(const QString& id) const;
    // 返回晕染缩略图 PNG 字节；缺失时为空数据。
    Q_INVOKABLE QVariantMap getTerrainThumbnail(const QString& id) const;
signals:
    // 快捷方式数据已持久化。
    void shortcutsChanged();
    // 书签索引已持久化。
    void bookmarksChanged();
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
    // 录屏历史已持久化。
    void recordingsChanged();
    // 文件检索索引状态或进度已更新。
    void fileSearchStatusChanged();
    // 地形项目或数据已持久化。
    void terrainChanged();
    // 一次文件检索已返回结果。
    void fileSearchResults(const QVariantMap& result);
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
    // 网页书签服务，同时被书签窗口复用。
    BookmarkService* m_bookmarks;
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
    // 录屏历史服务。
    RecordingService* m_recordings;
    // 截图全局热键注册器。
    GlobalHotkey* m_screenshotHotkey;
    // 启动时截图热键注册失败的说明。
    QString m_screenshotHotkeyWarning;
    // 截图流程进行中，避免重复触发。
    bool m_screenshotBusy = false;
    // 截图功能开关；关闭时热键解除且页面按钮不可用。
    bool m_screenshotEnabled = true;
    // 每块屏幕一个截图遮罩；收到 finished 后逐个销毁。
    QList<QPointer<ScreenshotOverlay>> m_overlays;
    // 录屏流程进行中，避免重复触发。
    bool m_recordingBusy = false;
    // 每块屏幕一个录屏选区遮罩；确认或取消后逐个销毁。
    QList<QPointer<RecordingOverlay>> m_recordingOverlays;
    // 录制中的控制条、区域提示框与录屏管线；结束时销毁。
    QPointer<RecordingControlBar> m_recordingBar;
    QPointer<RecordingRegionFrame> m_recordingFrame;
    QPointer<ScreenRecorder> m_recorder;
    // 本次录制的输出文件路径（来自录屏服务的预约）。
    QString m_recordingPath;
    // 录屏取消或失败时是否恢复主窗口；成功保存后保持隐藏。
    bool m_recordingRestoreWindow = false;
    // 媒体分类、目录清单与默认打开方式服务。
    MediaService* m_media;
    // 文件名全盘索引与检索服务；内部自带工作线程。
    FileSearchService* m_fileSearch;
    // 地形项目与高度数据服务；同时被独立编辑窗口共享。
    TerrainService* m_terrain;
    // 当前独立画板窗口，关闭销毁后自动清空。
    QPointer<PaintWindow> m_paintWindow;
    // 是否启用系统级交互（热键注册、截屏遮罩）。
    bool m_nativeIntegration;
    // 已打开的书签窗口；关闭后置空，避免重复创建。
    QPointer<BookmarkWindow> m_bookmarkWindow;
    // 全局热键注册器。
    GlobalHotkey* m_hotkey;
    // 启动时热键注册失败的说明。
    QString m_hotkeyWarning;
    // 显示刚创建的资源浏览器窗口并置前。
    void presentBrowserWindow(QWidget* window);
    // 隐藏主窗口后逐屏显示遮罩；完成后更新剪贴板与历史，仅取消或失败时恢复窗口。
    void beginScreenshot(bool restoreWindow);
    // 隐藏主窗口后逐屏抓取并显示录屏选区遮罩。
    void beginRecording(bool restoreWindow);
    // 关闭并销毁全部录屏选区遮罩。
    void closeRecordingOverlays();
    // 结束当前录制流程：销毁控制条与管线；失败时删除半成品文件。
    void finishRecordingFlow(bool succeeded, const QString& error);
    // 把本地文件复制到系统剪贴板（资源管理器可直接粘贴）。
    void copyFileToClipboard(const QString& path);
};
