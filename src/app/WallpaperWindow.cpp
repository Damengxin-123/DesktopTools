#include "app/WallpaperWindow.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QMediaPlayer>
#include <QScreen>
#include <QStackedLayout>
#include <QTimer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>
#include <QVideoWidget>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>
#include <QWebEngineView>
#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace
{
// 壁纸媒体协议：把本地文件以受限形式提供给内置壁纸页面，
// 绕开 Chromium 禁止非 file 页面加载本地资源的限制。
const QByteArray MediaScheme = QByteArrayLiteral("wallpaper-media");
const QString MediaPrefix = QStringLiteral("wallpaper-media://local/");

// 壁纸窗口专用的媒体协议处理器，把协议地址映射为本地文件流。
class MediaFileHandler final : public QWebEngineUrlSchemeHandler
{
public:
    explicit MediaFileHandler(QObject* parent) : QWebEngineUrlSchemeHandler(parent) {}
    void requestStarted(QWebEngineUrlRequestJob* job) override
    {
        const QString url = job->requestUrl().toString();
        if (!url.startsWith(MediaPrefix)) {
            job->fail(QWebEngineUrlRequestJob::UrlNotFound);
            return;
        }
        const QFileInfo file(QDir::cleanPath(QUrl::fromPercentEncoding(
            url.mid(MediaPrefix.size()).toUtf8())));
        if (!file.isFile() || !file.exists()) {
            job->fail(QWebEngineUrlRequestJob::UrlNotFound);
            return;
        }
        auto* stream = new QFile(file.absoluteFilePath(), job);
        if (!stream->open(QIODevice::ReadOnly)) {
            job->fail(QWebEngineUrlRequestJob::UrlNotFound);
            return;
        }
        const QMimeType mime = QMimeDatabase().mimeTypeForFile(file.absoluteFilePath(),
            QMimeDatabase::MatchExtension);
        job->reply(mime.name().toLatin1(), stream);
    }
};
}

// 进程内注册一次媒体协议；必须在创建应用对象之前调用。
void WallpaperWindow::registerMediaScheme()
{
    if (QWebEngineUrlScheme::schemeByName(MediaScheme).name() == MediaScheme)
        return;
    QWebEngineUrlScheme scheme(MediaScheme);
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
    scheme.setFlags(QWebEngineUrlScheme::SecureScheme);
    QWebEngineUrlScheme::registerScheme(scheme);
}

// 创建壁纸窗口并加载内置显示页面；窗口保持隐藏，挂载成功后才可见。
WallpaperWindow::WallpaperWindow(QWidget* parent)
    : QWidget(parent), m_profile(new QWebEngineProfile(this)), m_view(new QWebEngineView(this)),
      m_contentLayout(new QStackedLayout(this)), m_videoTimeout(new QTimer(this))
{
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus
        | Qt::WindowTransparentForInput);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_QuitOnClose, false);
    m_view->setContextMenuPolicy(Qt::NoContextMenu);
    m_view->setFocusPolicy(Qt::NoFocus);
    // 独立离线配置只服务壁纸页面，媒体协议不会暴露给主界面网页。
    m_profile->installUrlSchemeHandler(MediaScheme, new MediaFileHandler(m_profile));
    // 页面归属配置管理，析构时先视图后配置即可顺序正确。
    auto* page = new QWebEnginePage(m_profile, m_profile);
    // 视频静音循环播放不需要用户手势。
    page->settings()->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
    m_view->setPage(page);
    connect(m_view, &QWebEngineView::loadFinished, this, [this](bool loaded) {
        m_pageReady = loaded;
        if (loaded)
            applySource();
    });
    m_contentLayout->setContentsMargins(0, 0, 0, 0);
    m_contentLayout->addWidget(m_view);
    m_videoTimeout->setSingleShot(true);
    connect(m_videoTimeout, &QTimer::timeout, this, [this] {
        failVideo(QStringLiteral("未能读取视频画面，文件可能损坏或使用了不支持的编码。"));
    });
    m_view->load(QUrl(QStringLiteral("qrc:/web/wallpaper.html")));
}

// 先释放视图与页面，最后释放配置，避免 QtWebEngine 的关闭顺序警告。
WallpaperWindow::~WallpaperWindow()
{
    detachFromDesktop();
    stopVideo();
    QWebEnginePage* page = m_view ? m_view->page() : nullptr;
    delete m_view;
    m_view = nullptr;
    delete page;
    delete m_profile;
    m_profile = nullptr;
}

// 更换显示内容；页面未就绪时仅缓存，加载完成后自动应用。
void WallpaperWindow::setSource(const QString& path, const QString& kind, const QString& mode)
{
    if (m_sourcePath == path && m_sourceKind == kind && m_displayMode == mode)
        return;
    m_sourcePath = path;
    m_sourceKind = kind;
    m_displayMode = mode;
    stopVideo();
    m_pendingPath.clear();
    m_pendingKind.clear();
    if (kind == QStringLiteral("video")) {
        // 不创建音频输出，壁纸始终静音；直接用本地 URL 交给 FFmpeg 后端解码。
        m_videoView = new QVideoWidget(this);
        m_videoView->setAspectRatioMode(Qt::KeepAspectRatioByExpanding);
        m_videoView->setFocusPolicy(Qt::NoFocus);
        m_videoView->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_contentLayout->addWidget(m_videoView);
        m_contentLayout->setCurrentWidget(m_videoView);
        m_view->page()->runJavaScript(QStringLiteral(
            "window.desktopWallpaperPage && window.desktopWallpaperPage.clear();"));
        m_player = new QMediaPlayer(this);
        m_player->setVideoOutput(m_videoView);
        m_player->setLoops(QMediaPlayer::Infinite);
        // 回调随本次播放器销毁，快速切换时旧帧和旧错误不会污染新资源。
        connect(m_player, &QMediaPlayer::errorOccurred, m_player,
            [this](QMediaPlayer::Error, const QString& message) { failVideo(message); });
        connect(m_videoView->videoSink(), &QVideoSink::videoFrameChanged, m_player,
            [this](const QVideoFrame& frame) {
                if (!m_videoReady && !m_videoFailed && frame.isValid()) {
                    m_videoReady = true;
                    m_videoTimeout->stop();
                    emit mediaReady();
                }
            });
        m_videoTimeout->start(15000);
        m_player->setSource(QUrl::fromLocalFile(path));
        m_player->play();
        return;
    }
    m_contentLayout->setCurrentWidget(m_view);
    m_pendingPath = path;
    m_pendingKind = kind;
    applySource();
}

// 已解码的视频帧才算成功，单纯创建窗口或开始播放不代表有画面。
bool WallpaperWindow::isMediaReady() const
{
    return m_sourceKind != QStringLiteral("video") || (m_videoReady && !m_videoFailed);
}

// 先解除信号连接再停止，防止关闭过程中旧播放器回调污染下一份资源。
void WallpaperWindow::stopVideo()
{
    m_videoTimeout->stop();
    if (m_player) {
        m_player->disconnect(m_player);
        if (m_videoView)
            m_videoView->videoSink()->disconnect(m_player);
        m_player->stop();
        delete m_player;
        m_player = nullptr;
    }
    delete m_videoView;
    m_videoView = nullptr;
    m_videoReady = false;
    m_videoFailed = false;
}

// 解码失败时立即隐藏并脱离桌面层；服务随后释放窗口并显示可读原因。
void WallpaperWindow::failVideo(const QString& message)
{
    if (m_sourceKind != QStringLiteral("video") || m_videoFailed)
        return;
    m_videoFailed = true;
    m_videoTimeout->stop();
    detachFromDesktop();
    emit mediaFailed(m_sourcePath, QStringLiteral("视频壁纸播放失败，已恢复系统桌面：%1").arg(
        message.isEmpty() ? QStringLiteral("无法解码此文件，请检查文件是否完整及其编码格式。") : message));
}

// 向内置页面发送媒体地址；路径经 JSON 转义，不会被当作脚本执行。
void WallpaperWindow::applySource()
{
    if (!m_pageReady || m_pendingPath.isEmpty())
        return;
    // 本地路径经媒体协议与百分号编码进入页面，避免浏览器当作未知协议。
    const QJsonObject payload{{QStringLiteral("src"),
        MediaPrefix + QString::fromUtf8(QUrl::toPercentEncoding(m_pendingPath))},
        {QStringLiteral("kind"), m_pendingKind}, {QStringLiteral("mode"), m_displayMode},
        {QStringLiteral("screenWidth"), m_pixelSize.width()}, {QStringLiteral("screenHeight"), m_pixelSize.height()}};
    const QString script = QStringLiteral(
        "window.desktopWallpaperPage && window.desktopWallpaperPage.show(%1);")
        .arg(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact)));
    m_view->page()->runJavaScript(script);
    m_pendingPath.clear();
    m_pendingKind.clear();
}

// 枚举物理显示矩形，不能对混合 DPI 桌面统一乘一个缩放率。
QVariantList WallpaperWindow::screens()
{
    QVariantList result;
#ifdef Q_OS_WIN
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM context) -> BOOL {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info))
            return TRUE;
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        const QString name = QString::fromWCharArray(info.szDevice);
        QString id = name;
        if (EnumDisplayDevicesW(info.szDevice, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME) && device.DeviceID[0])
            id = QString::fromWCharArray(device.DeviceID);
        const RECT& rect = info.rcMonitor;
        reinterpret_cast<QVariantList*>(context)->append(QVariantMap{
            {"id", id}, {"name", name}, {"x", int(rect.left)}, {"y", int(rect.top)},
            {"width", int(rect.right - rect.left)}, {"height", int(rect.bottom - rect.top)},
            {"primary", (info.dwFlags & MONITORINFOF_PRIMARY) != 0}});
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
#else
    for (const QScreen* screen : QGuiApplication::screens()) {
        const QRect rect = screen->geometry();
        result.append(QVariantMap{{"id", screen->name()}, {"name", screen->name()},
            {"x", rect.x()}, {"y", rect.y()}, {"width", rect.width()}, {"height", rect.height()},
            {"primary", screen == QGuiApplication::primaryScreen()}});
    }
#endif
    // 按设备名称排序后从 1 编号；设置以稳定硬件标识保存，不依赖临时编号。
    std::sort(result.begin(), result.end(), [](const QVariant& a, const QVariant& b) {
        return a.toMap().value("name").toString() < b.toMap().value("name").toString();
    });
    for (qsizetype index = 0; index < result.size(); ++index) {
        auto screen = result[index].toMap();
        screen.insert("number", int(index + 1));
        result[index] = screen;
    }
    return result;
}

#ifdef Q_OS_WIN
namespace {
// 仅接受资源管理器图标层后方的 WorkerW，不将任意 WorkerW 或 Progman 当作回退宿主。
HWND desktopHost()
{
    const HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman)
        return nullptr;
    // 新版 Windows 11 将 WorkerW 放在 Progman 内，旧版放在图标宿主之后。
    const bool raised = (GetWindowLongPtrW(progman, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP) != 0;
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(progman, 0x052C, raised ? 0xD : 0, raised ? 1 : 0, SMTO_ABORTIFHUNG, 1000, &ignored);
    const HWND icons = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    if (icons) {
        const HWND child = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
        for (HWND sibling = GetWindow(icons, GW_HWNDNEXT); sibling; sibling = GetWindow(sibling, GW_HWNDNEXT)) {
            if (sibling == child && !FindWindowExW(child, nullptr, L"SHELLDLL_DefView", nullptr))
                return child;
        }
    }
    HWND host = nullptr;
    EnumWindows([](HWND top, LPARAM context) -> BOOL {
        if (!FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr))
            return TRUE;
        // 查找起点必须为同层顶级窗口，不能传入 DefView 子窗口。
        const HWND next = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
        DWORD shellProcess = 0, workerProcess = 0;
        GetWindowThreadProcessId(top, &shellProcess);
        GetWindowThreadProcessId(next, &workerProcess);
        if (next && shellProcess == workerProcess && IsWindowVisible(next)
            && !FindWindowExW(next, nullptr, L"SHELLDLL_DefView", nullptr)) {
            *reinterpret_cast<HWND*>(context) = next;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&host));
    return host;
}
}
#endif

// 每个实例只覆盖一块显示器，Qt 显示之后再使用原生坐标作最终校正。
bool WallpaperWindow::attachToDesktop(const QString& screenId)
{
    QVariantMap target;
    for (const QVariant& value : screens()) {
        const auto screen = value.toMap();
        if ((screenId.isEmpty() && screen.value("primary").toBool()) || screen.value("id").toString() == screenId) {
            target = screen;
            break;
        }
    }
    if (target.isEmpty())
        return false;
    const QRect bounds(target.value("x").toInt(), target.value("y").toInt(),
        target.value("width").toInt(), target.value("height").toInt());
    m_pixelSize = bounds.size();
    // 先按该屏逻辑尺寸布局 WebEngine 子视图，避免混合缩放留下黑边。
    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen->name() == target.value("name").toString()) {
            setGeometry(screen->geometry());
            break;
        }
    }
#ifdef Q_OS_WIN
    const HWND host = desktopHost();
    if (!host)
        return false;
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    m_nativeWindow = reinterpret_cast<quintptr>(hwnd);
    m_hostWindow = reinterpret_cast<quintptr>(host);
    SetWindowLongPtrW(hwnd, GWL_STYLE, (GetWindowLongPtrW(hwnd, GWL_STYLE) & ~WS_POPUP) | WS_CHILD);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
        (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & ~WS_EX_APPWINDOW) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT);
    SetParent(hwnd, host);
    if (GetAncestor(hwnd, GA_PARENT) != host) {
        detachFromDesktop();
        return false;
    }
    // 禁用原生窗口树输入，将鼠标命中交回资源管理器。
    EnableWindow(hwnd, FALSE);
    show();
    POINT origin{bounds.x(), bounds.y()};
    SetLastError(ERROR_SUCCESS);
    if (!MapWindowPoints(nullptr, host, &origin, 1) && GetLastError() != ERROR_SUCCESS) {
        detachFromDesktop();
        return false;
    }
    if (!SetWindowPos(hwnd, HWND_BOTTOM, origin.x, origin.y, bounds.width(), bounds.height(),
            SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW)) {
        detachFromDesktop();
        return false;
    }
    return true;
#else
    setGeometry(bounds);
    show();
    return true;
#endif
}

// 先撤回自有窗口再请求系统重画，不销毁或隐藏 Explorer 的 WorkerW。
void WallpaperWindow::detachFromDesktop()
{
    hide();
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(m_nativeWindow);
    const HWND host = reinterpret_cast<HWND>(m_hostWindow);
    if (IsWindow(hwnd)) {
        ShowWindow(hwnd, SW_HIDE);
        SetParent(hwnd, nullptr);
        SetWindowLongPtrW(hwnd, GWL_STYLE, (GetWindowLongPtrW(hwnd, GWL_STYLE) & ~WS_CHILD) | WS_POPUP);
    }
    if (IsWindow(host))
        RedrawWindow(host, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    if (const HWND progman = FindWindowW(L"Progman", nullptr))
        RedrawWindow(progman, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
#endif
    m_hostWindow = 0;
    m_nativeWindow = 0;
}

// 检查时不调用 winId，防止 Explorer 重启后意外创建新的顶层窗口。
bool WallpaperWindow::isAlive() const
{
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(m_nativeWindow);
    const HWND host = reinterpret_cast<HWND>(m_hostWindow);
    return IsWindow(hwnd) && IsWindow(host) && IsWindowVisible(hwnd) && GetAncestor(hwnd, GA_PARENT) == host;
#else
    return isVisible();
#endif
}
