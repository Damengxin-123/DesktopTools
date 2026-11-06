#pragma once

#include <QWidget>
#include <QVariantList>

class QWebEngineProfile;
class QWebEngineView;
class QMediaPlayer;
class QVideoWidget;
class QStackedLayout;
class QTimer;

// 承载动态壁纸内容的无边框窗口，原生侧负责挂到桌面图标层之下的 WorkerW。
class WallpaperWindow final : public QWidget
{
    Q_OBJECT
public:
    explicit WallpaperWindow(QWidget* parent = nullptr);
    ~WallpaperWindow() override;
    // 壁纸媒体协议须在创建应用对象前注册，进程内只注册一次。
    static void registerMediaScheme();
    // 提供网页视图以便执行真实桌面环境的媒体加载验证。
    QWebEngineView* webView() const { return m_view; }
    // 提供实际的视频输出，供冒烟验证解码帧和完整显示区域。
    QVideoWidget* videoView() const { return m_videoView; }
    // 视频必须已经收到有效解码帧，才能向界面报告正在显示。
    bool isMediaReady() const;
    // 切换显示的媒体；kind 为 image 或 video，路径在页面加载完成后应用。
    void setSource(const QString& path, const QString& kind, const QString& mode = QStringLiteral("fill"));
    // 枚举已连接显示器的稳定标识、编号和完整物理像素矩形。
    static QVariantList screens();
    // 挂到已验证的图标下方壁纸层，只覆盖指定显示器；空标识使用主屏。
    bool attachToDesktop(const QString& screenId = {});
    // 立即隐藏并解除原生父子关系，让系统重新绘制原桌面。
    void detachFromDesktop();
    // 桌面重建后本窗口可能被系统一并销毁，供守护定时器检查。
    bool isAlive() const;
signals:
    // 视频解码或加载失败，窗口已经撤回，由服务向相应屏幕报告错误。
    void mediaFailed(const QString& path, const QString& message);
    // 收到首帧时通知服务更新正在显示的状态。
    void mediaReady();
private:
    // 停止播放并释放解码资源，避免切换图片后视频仍在后台运行。
    void stopVideo();
    // 失败时立即恢复系统桌面，不把黑色窗口留在桌面上。
    void failVideo(const QString& message);
    // 把待显示资源以 JSON 参数传给内置壁纸页面。
    void applySource();
    // 壁纸窗口专用的离线浏览配置，安装了媒体协议处理器。
    QWebEngineProfile* m_profile;
    // 填充整个壁纸窗口的网页视图。
    QWebEngineView* m_view;
    QStackedLayout* m_contentLayout; // 图片网页与原生视频输出共用同一屏幕矩形。
    QMediaPlayer* m_player = nullptr; // Qt Multimedia 原生解码，不依赖浏览器的专有编码开关。
    QVideoWidget* m_videoView = nullptr; // 使用硬件视频输出，保持比例裁剪填满屏幕。
    QTimer* m_videoTimeout; // 限制等待首帧时间，处理没有视频流或无法完成加载的文件。
    bool m_videoReady = false; // 至少收到一帧有效视频后置为真。
    bool m_videoFailed = false; // 当前资源只报告一次错误。
    // 页面尚未加载完成前缓存的待显示路径。
    QString m_pendingPath;
    // 缓存的待显示媒体类型：image 或 video。
    QString m_pendingKind;
    // 内置页面是否已加载完成。
    bool m_pageReady = false;
    // 挂载目标（WorkerW 或 Progman）的句柄，用于存活检查。
    quintptr m_hostWindow = 0;
    // 已创建的原生句柄，存活检查不调用可能重新创建窗口的 winId。
    quintptr m_nativeWindow = 0;
    // 当前资源路径和媒体类型，防止守护检查反复重载视频。
    QString m_sourcePath;
    QString m_sourceKind; // 当前资源类型。
    QString m_displayMode = QStringLiteral("fill"); // 图片显示方式，切换时不重新加载媒体。
    QSize m_pixelSize; // 目标屏幕完整物理像素尺寸，用于原尺寸居中和平铺。
};
