#include "InterfaceLanguage.h"
#include "WebWindow.h"
#include "PaintWindow.h"
#include "level/LevelWindow.h"
#include "BookmarkWindow.h"

#include "bridge/AppBridge.h"
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QTimer>
#include <memory>
#include <QSystemTrayIcon>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEnginePermission>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>

namespace {
// 限制资源只能来自内置页面和内存图片，避免便签访问外部或本地文件。
class LocalResources final : public QWebEngineUrlRequestInterceptor
{
public:
    // 由浏览器配置对象管理拦截器生命周期。
    explicit LocalResources(QObject* parent) : QWebEngineUrlRequestInterceptor(parent) {}
    // 拦截所有网络及 file 资源请求。
    void interceptRequest(QWebEngineUrlRequestInfo& info) override
    {
        const auto scheme = info.requestUrl().scheme();
        if (scheme != "qrc" && scheme != "data" && scheme != "about" && scheme != "blob")
            info.block(true);
    }
};

// 只允许加载随程序打包的应用入口，不给其他页面桥接权限。
class ApplicationPage final : public QWebEnginePage
{
public:
    // 使用窗口独有的离线浏览器配置。
    ApplicationPage(QWebEngineProfile* profile, QObject* parent) : QWebEnginePage(profile, parent) {}
protected:
    // 阻止外部导航，快捷方式由明确的后端接口打开。
    bool acceptNavigationRequest(const QUrl& url, NavigationType type, bool isMainFrame) override
    {
        Q_UNUSED(type)
        return isMainFrame && url.scheme() == "qrc" && url.path() == "/web/index.html";
    }
};
}

WebWindow::WebWindow(const QString& dataRoot, bool nativeIntegration, QWidget* parent)
    : QMainWindow(parent), m_view(new QWebEngineView(this)),
      m_bridge(new AppBridge(dataRoot, this, nativeIntegration))
{
    InterfaceLanguage::bind(this, "windowTitle", QStringLiteral("桌面小工具"));
    setWindowIcon(QIcon(QStringLiteral(":/images/app_icon.ico")));
    resize(1180, 780);
    setMinimumSize(860, 600);
    setCentralWidget(m_view);

    // 非持久化浏览器配置，业务数据统一由 Qt 服务保存。
    auto* profile = new QWebEngineProfile(this);
    profile->setUrlRequestInterceptor(new LocalResources(profile));
    auto* page = new ApplicationPage(profile, m_view);
    InterfaceLanguage::registerView(m_view);
    m_view->setPage(page);
    page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    page->settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    page->settings()->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, false);
    m_view->setContextMenuPolicy(Qt::NoContextMenu);
    connect(page, &QWebEnginePage::permissionRequested, this, [](QWebEnginePermission permission) {
        permission.deny();
    });
    auto* channel = new QWebChannel(page);
    channel->registerObject(QStringLiteral("backend"), m_bridge);
    page->setWebChannel(channel);
    // 主页面重载或导航期间隐藏原生书签，避免遮挡尚未就绪的应用页面。
    connect(m_view, &QWebEngineView::loadStarted, this, [this]() {
        m_bridge->setBookmarkViewport(false, 0, 0, 0, 0);
    });
    // 持久化设置变化同步到主页面、书签面板与独立地形窗口。
    const auto syncLanguage = [this]() {
        const QString language = m_bridge->getSettings().value(QStringLiteral("data")).toMap()
            .value(QStringLiteral("language"), QStringLiteral("zh-CN")).toString();
        InterfaceLanguage::setLanguage(language);
        InterfaceLanguage::bind(this, "windowTitle", language == QStringLiteral("en") ? QStringLiteral("DesktopTool") : QStringLiteral("桌面小工具"));
    };
    connect(m_bridge, &AppBridge::settingsChanged, this, syncLanguage);
    syncLanguage();
    connect(m_bridge, &AppBridge::activateWindowRequested, this, &WebWindow::activate);
    if (nativeIntegration && QSystemTrayIcon::isSystemTrayAvailable())
        createTray();
    m_view->load(QUrl(QStringLiteral("qrc:/web/index.html")));
}

void WebWindow::activate()
{
    if (isMinimized())
        showNormal();
    else
        show();
    raise();
    activateWindow();
}

// 命令行或第二实例转发的本地文件：交给桥接层分流打开。
void WebWindow::openLocalFile(const QString& path)
{
    m_bridge->openLocalFile(path);
}

void WebWindow::requestQuit()
{
    if (m_quitPending)
        return;
    m_quitPending = true;
    // 确认对话框需要可见父窗口，否则托盘退出时可能被其他应用遮挡。
    activate();
    QPointer<WebWindow> self(this);
    // 页面渲染进程无响应时也必须结束等待；迟到回调只允许处理一次。
    auto completed = std::make_shared<bool>(false);
    auto* timeout = new QTimer(this);
    const auto finish = [self, completed, timeout](const QVariant& allowed) {
            if (*completed)
                return;
            *completed = true;
            if (!self)
                return;
            timeout->stop();
            timeout->deleteLater();
            self->m_quitPending = false;
            if (!allowed.isValid() || allowed.metaType().id() != QMetaType::Bool) {
                const QString detail = self->m_bridge->hasActiveDownloads()
                    ? InterfaceLanguage::text(QStringLiteral("页面没有响应，无法确认未保存内容。正在进行的下载会暂停，可在下次启动后继续。仍要退出吗？"))
                    : InterfaceLanguage::text(QStringLiteral("页面没有响应，无法确认是否有未保存内容。仍要退出吗？"));
                if (QMessageBox::warning(self, InterfaceLanguage::text(QStringLiteral("退出桌面小工具")),
                        detail,
                        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
                    return;
            } else if (!allowed.toBool()) {
                return;
            }
            // 内嵌浏览器可能仍在下载，退出应用前沿用书签下载确认。
            for (auto* bookmarks : self->findChildren<BookmarkWindow*>()) {
                if (!bookmarks->close()) return;
            }
            // 托盘退出也必须保护独立画板中尚未保存的作品。
            for (auto* paint : self->findChildren<PaintWindow*>()) {
                if (!paint->close()) return;
            }
            // 主程序退出时先保护关卡编辑器的未保存对象。
            for (auto* editor : self->findChildren<LevelWindow*>()) {
                if (!editor->close()) return;
            }
            self->m_canQuit = true;
            self->close();
            QApplication::quit();
        };
    // 用户正在处理保存确认框时继续等待，避免把正常确认误判为页面卡死。
    connect(timeout, &QTimer::timeout, this, [finish]() {
        if (!QApplication::activeModalWidget())
            finish(QVariant());
    });
    timeout->start(3000);
    m_view->page()->runJavaScript(QStringLiteral(
        "typeof window.desktopToolCanClose === 'function' ? window.desktopToolCanClose() : null"), finish);
}

void WebWindow::closeEvent(QCloseEvent* event)
{
    if (m_canQuit) {
        event->accept();
    } else if (m_tray && m_tray->isVisible()) {
        hide();
        event->ignore();
    } else {
        event->ignore();
        requestQuit();
    }
}

void WebWindow::createTray()
{
    m_tray = new QSystemTrayIcon(windowIcon(), this);
    auto* menu = new QMenu(this);
    auto* showAction = menu->addAction(InterfaceLanguage::text(QStringLiteral("打开桌面小工具")));
    InterfaceLanguage::bind(showAction, "text", QStringLiteral("打开桌面小工具"));
    auto* exitAction = menu->addAction(InterfaceLanguage::text(QStringLiteral("退出")));
    InterfaceLanguage::bind(exitAction, "text", QStringLiteral("退出"));
    connect(showAction, &QAction::triggered, this, &WebWindow::activate);
    connect(exitAction, &QAction::triggered, this, &WebWindow::requestQuit);
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::DoubleClick || reason == QSystemTrayIcon::Trigger)
            activate();
    });
    InterfaceLanguage::bind(m_tray, "toolTip", QStringLiteral("桌面小工具"));
    m_tray->setContextMenu(menu);
    m_tray->show();
}
