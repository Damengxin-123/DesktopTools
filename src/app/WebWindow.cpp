#include "WebWindow.h"

#include "bridge/AppBridge.h"
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
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
    setWindowTitle(QStringLiteral("桌面小工具"));
    setWindowIcon(QIcon(QStringLiteral(":/images/app_icon.ico")));
    resize(1180, 780);
    setMinimumSize(860, 600);
    setCentralWidget(m_view);

    // 非持久化浏览器配置，业务数据统一由 Qt 服务保存。
    auto* profile = new QWebEngineProfile(this);
    profile->setUrlRequestInterceptor(new LocalResources(profile));
    auto* page = new ApplicationPage(profile, m_view);
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

void WebWindow::requestQuit()
{
    if (m_quitPending)
        return;
    m_quitPending = true;
    // 确认对话框需要可见父窗口，否则托盘退出时可能被其他应用遮挡。
    activate();
    QPointer<WebWindow> self(this);
    m_view->page()->runJavaScript(QStringLiteral(
        "typeof window.desktopToolCanClose === 'function' ? window.desktopToolCanClose() : null"),
        [self](const QVariant& allowed) {
            if (!self)
                return;
            self->m_quitPending = false;
            if (!allowed.isValid() || allowed.metaType().id() != QMetaType::Bool) {
                const QString detail = self->m_bridge->hasActiveDownloads()
                    ? QStringLiteral("页面没有响应，无法确认未保存内容。正在进行的下载会暂停，可在下次启动后继续。仍要退出吗？")
                    : QStringLiteral("页面没有响应，无法确认是否有未保存内容。仍要退出吗？");
                if (QMessageBox::warning(self, QStringLiteral("退出桌面小工具"),
                        detail,
                        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
                    return;
            } else if (!allowed.toBool()) {
                return;
            }
            self->m_canQuit = true;
            self->close();
            QApplication::quit();
        });
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
    auto* showAction = menu->addAction(QStringLiteral("打开桌面小工具"));
    auto* exitAction = menu->addAction(QStringLiteral("退出"));
    connect(showAction, &QAction::triggered, this, &WebWindow::activate);
    connect(exitAction, &QAction::triggered, this, &WebWindow::requestQuit);
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::DoubleClick || reason == QSystemTrayIcon::Trigger)
            activate();
    });
    m_tray->setToolTip(QStringLiteral("桌面小工具"));
    m_tray->setContextMenu(menu);
    m_tray->show();
}
