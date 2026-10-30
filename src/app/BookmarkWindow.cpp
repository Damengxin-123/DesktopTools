#include "InterfaceLanguage.h"
#include "BookmarkWindow.h"

#include "bridge/BookmarkBridge.h"
#include "services/BookmarkService.h"

#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSizePolicy>
#include <QSplitter>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QWebChannel>
#include <QWebEngineDownloadRequest>
#include <QWebEngineHistory>
#include <QWebEnginePage>
#include <QWebEnginePermission>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>


namespace {
/** 左侧面板只读取打包资源，禁止网页访问本地文件或外部网络。 */
class PanelResources final : public QWebEngineUrlRequestInterceptor
{
public:
    explicit PanelResources(QObject* parent) : QWebEngineUrlRequestInterceptor(parent) {}
    void interceptRequest(QWebEngineUrlRequestInfo& info) override
    {
        const auto scheme = info.requestUrl().scheme();
        if (scheme != "qrc" && scheme != "data" && scheme != "about" && scheme != "blob")
            info.block(true);
    }
};

/** 面板主文档只接受 bookmarks.html，避免其他页面取得桥接权限。 */
class PanelPage final : public QWebEnginePage
{
public:
    PanelPage(QWebEngineProfile* profile, QObject* parent) : QWebEnginePage(profile, parent) {}
protected:
    bool acceptNavigationRequest(const QUrl& url, NavigationType type, bool isMainFrame) override
    {
        Q_UNUSED(type)
        return isMainFrame && url.scheme() == "qrc" && url.path() == "/web/bookmarks.html";
    }
};

/** 浏览区页面：window.open 与 target=_blank 转到简易弹出窗口。 */
class BrowsingPage final : public QWebEnginePage
{
public:
    BrowsingPage(QWebEngineProfile* profile, QObject* parent) : QWebEnginePage(profile, parent) {}

protected:
    QWebEnginePage* createWindow(WebWindowType type) override;
};

/** 弹出结果显示窗口：与浏览区共用配置，下载同样落到下载目录。 */
class PopupWindow final : public QMainWindow
{
public:
    explicit PopupWindow(QWebEngineProfile* profile)
        : QMainWindow(nullptr), m_view(new QWebEngineView(this))
    {
        InterfaceLanguage::bind(this, "windowTitle", QStringLiteral("网页书签 · 新窗口"));
        setWindowIcon(QIcon(QStringLiteral(":/images/app_icon.ico")));
        setAttribute(Qt::WA_DeleteOnClose);
        resize(1024, 700);
        setCentralWidget(m_view);
        auto* page = new BrowsingPage(profile, m_view);
        m_view->setPage(page);
        connect(page, &QWebEnginePage::titleChanged, this, [this](const QString& title) {
            if (!title.isEmpty())
                InterfaceLanguage::bind(this, "windowTitle", title + QStringLiteral(" — 网页书签"));
        });
        connect(page, &QWebEnginePage::permissionRequested, this, [](QWebEnginePermission permission) {
            permission.deny();
        });
    }

    QWebEnginePage* page() const { return m_view->page(); }

private:
    QWebEngineView* m_view;
};

QWebEnginePage* BrowsingPage::createWindow(WebWindowType type)
{
    if (type != QWebEnginePage::WebBrowserWindow && type != QWebEnginePage::WebBrowserTab
        && type != QWebEnginePage::WebDialog)
        return nullptr;
    auto* window = new PopupWindow(profile());
    window->show();
    return window->page();
}

/** 清理下载文件名，禁止路径穿越与系统保留字符。 */
QString safeFileName(const QString& suggested)
{
    QString name = suggested.trimmed();
    name.replace(QRegularExpression(QStringLiteral("[/\\\\:*?\"<>|\\x{0000}-\\x{001F}]")), QStringLiteral("_"));
    static const QRegularExpression dots(QStringLiteral("^\\.+"));
    name.remove(dots);
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
        name = QStringLiteral("download");
    return name.left(180);
}

/** 生成不覆盖已有文件的目标路径。 */
QString uniqueTarget(const QString& directory, const QString& name)
{
    const QFileInfo base(QDir(directory).filePath(name));
    if (!base.exists())
        return QDir(directory).filePath(name);
    const QString suffix = base.completeSuffix();
    const QString stem = base.completeBaseName();
    for (int index = 1; index < 10000; ++index) {
        const QString candidate = suffix.isEmpty()
            ? QStringLiteral("%1 (%2)").arg(stem).arg(index)
            : QStringLiteral("%1 (%2).%3").arg(stem).arg(index).arg(suffix);
        if (!QFileInfo::exists(QDir(directory).filePath(candidate)))
            return QDir(directory).filePath(candidate);
    }
    return QDir(directory).filePath(name);
}

/** 把字节数显示为 KB / MB / GB。 */
QString readableSize(qint64 bytes)
{
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024);
    if (bytes < 1024 * 1024 * 1024)
        return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

/** 浏览区未加载网页时的提示页。 */
const QString& emptyPageHtml()
{
    static const QString html = QStringLiteral(
        "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\"><style>"
        "html,body{height:100%;margin:0;background:#f7f9fc;color:#7d8798;"
        "font-family:'Segoe UI','Microsoft YaHei',sans-serif}"
        "body{display:flex;align-items:center;justify-content:center;text-align:center;flex-direction:column;gap:10px}"
        ".icon{font-size:40px}.title{font-size:15px;color:#657186}.hint{font-size:12px;color:#9aa4b4;line-height:1.9}"
        "</style></head><body><div class=\"icon\"><svg width=\"40\" height=\"40\" viewBox=\"0 0 24 24\" "
        "fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.65\" stroke-linejoin=\"round\">"
        "<path d=\"M6 4h12v17l-6-4-6 4z\"/></svg></div>"
        "<div class=\"title\">从左侧选择一个书签</div>"
        "<div class=\"hint\">网页会在这里打开，可以正常登录、上传和下载文件。<br>也可以在上方地址栏输入网址后按回车访问。</div>"
        "</body></html>");
    return html;
}
}

BookmarkWindow* BookmarkWindow::open(BookmarkService* service, const QString& downloadDirectory,
                                     QWidget* parent)
{
    return new BookmarkWindow(service, downloadDirectory, parent);
}

BookmarkWindow::BookmarkWindow(BookmarkService* service, const QString& downloadDirectory, QWidget* parent)
    : QMainWindow(parent, parent ? Qt::Widget : Qt::Window)
    , m_splitter(new QSplitter(Qt::Horizontal, this))
    , m_panel(new QWebEngineView(this))
    , m_web(new QWebEngineView(this))
    , m_bridge(new BookmarkBridge(service, this))
    , m_downloadDirectory(downloadDirectory)
{
    // QMainWindow 构造时会补充窗口标志，必须显式改回普通子控件。
    if (parent) setWindowFlags(Qt::Widget);
    InterfaceLanguage::bind(this, "windowTitle", QStringLiteral("网页书签"));
    setWindowIcon(QIcon(QStringLiteral(":/images/app_icon.ico")));
    setAttribute(Qt::WA_DeleteOnClose, !parent);
    resize(1100, 720);
    setMinimumSize(0, 0);

    // ---- 左侧书签面板：独立离线配置，只允许加载打包资源 ----
    auto* panelProfile = new QWebEngineProfile(this);
    panelProfile->setUrlRequestInterceptor(new PanelResources(panelProfile));
    auto* panelPage = new PanelPage(panelProfile, m_panel);
    InterfaceLanguage::registerView(m_panel);
    m_panel->setPage(panelPage);
    panelPage->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    panelPage->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    panelPage->settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    panelPage->settings()->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, false);
    m_panel->setContextMenuPolicy(Qt::NoContextMenu);
    connect(panelPage, &QWebEnginePage::permissionRequested, this, [](QWebEnginePermission permission) {
        permission.deny();
    });
    auto* channel = new QWebChannel(panelPage);
    channel->registerObject(QStringLiteral("backend"), m_bridge);
    panelPage->setWebChannel(channel);

    // ---- 右侧浏览区：持久化配置，保留登录状态并支持上传下载 ----
    m_webProfile = new QWebEngineProfile(QStringLiteral("desktoptool-bookmarks"), this);
    auto* webPage = new BrowsingPage(m_webProfile, m_web);
    m_web->setPage(webPage);
    auto* settings = webPage->settings();
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    settings->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, true);
    settings->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, true);
    settings->setAttribute(QWebEngineSettings::LocalStorageEnabled, true);
    settings->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, true);
    settings->setAttribute(QWebEngineSettings::PluginsEnabled, false);
    connect(webPage, &QWebEnginePage::permissionRequested, this, [](QWebEnginePermission permission) {
        permission.deny();
    });
    connect(m_webProfile, &QWebEngineProfile::downloadRequested, this, &BookmarkWindow::handleDownload);

    // 导航工具栏。
    m_toolbarContainer = new QWidget(this);
    m_toolbarContainer->setObjectName(QStringLiteral("siteToolbar"));
    m_toolbarContainer->setStyleSheet(QStringLiteral(
        "#siteToolbar{background:#ffffff;border-bottom:1px solid #e7ebf1;}"
        "#siteToolbar QPushButton{background:#ffffff;border:1px solid #dfe5ee;border-radius:8px;"
        "padding:6px 11px;font-size:12px;color:#576278;font-weight:600;}"
        "#siteToolbar QPushButton:hover{background:#f2f5fb;border-color:#cad6ec;}"
        "#siteToolbar QPushButton:disabled{color:#a9b2c1;border-color:#e7ebf1;}"
        "#siteToolbar QLineEdit{border:1px solid #dfe5ee;border-radius:8px;padding:7px 11px;"
        "background:#fafbfe;color:#52617a;font-size:12px;selection-background-color:#315ee7;}"
        "#siteToolbar QLineEdit:focus{border-color:#98b0ff;background:#ffffff;}"));
    auto* toolbarLayout = new QVBoxLayout(m_toolbarContainer);
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    auto* bar = new QHBoxLayout;
    toolbarLayout->addLayout(bar);
    bar->setContentsMargins(12, 10, 12, 10);
    bar->setSpacing(8);
    m_back = new QPushButton(InterfaceLanguage::text(QStringLiteral("后退")), m_toolbarContainer);
    InterfaceLanguage::bind(m_back, "text", QStringLiteral("后退"));
    m_forward = new QPushButton(InterfaceLanguage::text(QStringLiteral("前进")), m_toolbarContainer);
    InterfaceLanguage::bind(m_forward, "text", QStringLiteral("前进"));
    m_reload = new QPushButton(InterfaceLanguage::text(QStringLiteral("刷新")), m_toolbarContainer);
    InterfaceLanguage::bind(m_reload, "text", QStringLiteral("刷新"));
    m_stop = new QPushButton(InterfaceLanguage::text(QStringLiteral("停止")), m_toolbarContainer);
    InterfaceLanguage::bind(m_stop, "text", QStringLiteral("停止"));
    m_address = new QLineEdit(m_toolbarContainer);
    InterfaceLanguage::bind(m_address, "placeholderText", QStringLiteral("输入网址后按回车，或在左侧选择书签"));
    m_address->setClearButtonEnabled(true);
    auto* external = new QPushButton(InterfaceLanguage::text(QStringLiteral("系统浏览器打开")), m_toolbarContainer);
    InterfaceLanguage::bind(external, "text", QStringLiteral("系统浏览器打开"));
    auto* openDirectory = new QPushButton(InterfaceLanguage::text(QStringLiteral("下载目录")), m_toolbarContainer);
    InterfaceLanguage::bind(openDirectory, "text", QStringLiteral("下载目录"));
    bar->addWidget(m_back);
    bar->addWidget(m_forward);
    bar->addWidget(m_reload);
    bar->addWidget(m_stop);
    bar->addWidget(m_address, 1);
    auto* actions = new QHBoxLayout;
    actions->setContentsMargins(12, 0, 12, 6);
    actions->addWidget(external);
    actions->addWidget(openDirectory);
    actions->addStretch();
    toolbarLayout->addLayout(actions);

    auto* rightSide = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(rightSide);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);
    rightLayout->addWidget(m_toolbarContainer);
    rightLayout->addWidget(m_web, 1);

    m_panel->setMinimumWidth(260);
    rightSide->setMinimumWidth(0);
    m_address->setMinimumWidth(60);
    m_splitter->addWidget(m_panel);
    m_splitter->addWidget(rightSide);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({360, 1040});
    setCentralWidget(m_splitter);

    m_status = new QLabel(QString(), this);
    m_status->setStyleSheet(QStringLiteral("background:#ffffff;border-top:1px solid #e7ebf1;"
                                           "padding:6px 12px;color:#7d8798;font-size:11px;"));
    m_status->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    rightLayout->addWidget(m_status);

    m_back->setEnabled(false);
    m_forward->setEnabled(false);
    m_stop->setEnabled(false);
    updateNavigationButtons();

    connect(m_back, &QPushButton::clicked, this, [this]() { m_web->back(); });
    connect(m_forward, &QPushButton::clicked, this, [this]() { m_web->forward(); });
    connect(m_reload, &QPushButton::clicked, this, [this]() { m_web->reload(); });
    connect(m_stop, &QPushButton::clicked, this, [this]() { m_web->stop(); });
    connect(m_address, &QLineEdit::returnPressed, this, [this]() {
        const QString text = m_address->text().trimmed();
        if (!text.isEmpty())
            load(text);
    });
    connect(external, &QPushButton::clicked, this, [this]() {
        const QUrl url = m_web->url();
        if (url.isValid() && !url.isEmpty())
            QDesktopServices::openUrl(url);
    });
    connect(openDirectory, &QPushButton::clicked, this, [this]() {
        // 显式使用成员函数：构造函数参数同名会遮蔽名称查找。
        QDesktopServices::openUrl(QUrl::fromLocalFile(this->downloadDirectory()));
    });

    connect(m_web, &QWebEngineView::loadStarted, this, [this]() { m_stop->setEnabled(true); });
    connect(m_web, &QWebEngineView::loadFinished, this, [this]() {
        m_stop->setEnabled(false);
        updateNavigationButtons();
    });
    connect(m_web, &QWebEngineView::urlChanged, this, [this](const QUrl& url) {
        // 内置提示页使用 data:，不把整段 HTML 暴露到用户地址栏。
        if (url.isEmpty() || (url.scheme() != "http" && url.scheme() != "https"))
            return;
        const QString text = url.toString();
        if (m_address->text() != text)
            m_address->setText(text);
        m_address->setCursorPosition(0);
        InterfaceLanguage::bind(this, "windowTitle", text.isEmpty() ? QStringLiteral("网页书签") : text + QStringLiteral(" — 网页书签"));
        pushUrlToPanel();
    });
    connect(m_web, &QWebEngineView::titleChanged, this, [this](const QString& title) {
        updateNavigationButtons();
        if (!title.isEmpty())
            InterfaceLanguage::bind(this, "windowTitle", title + QStringLiteral(" — 网页书签"));
    });

    connect(m_bridge, &BookmarkBridge::openRequested, this, &BookmarkWindow::load);
    connect(m_panel, &QWebEngineView::loadFinished, this, [this](bool succeeded) {
        m_panelReady = succeeded;
        if (succeeded)
            pushUrlToPanel();
    });
    m_panel->load(QUrl(QStringLiteral("qrc:/web/bookmarks.html")));
    showEmptyPage();
}

void BookmarkWindow::showEmptyPage()
{
    m_web->setHtml(emptyPageHtml());
    m_address->clear();
}

void BookmarkWindow::pushUrlToPanel()
{
    if (!m_panelReady)
        return;
    const QUrl current = m_web->url();
    const QString url = current.scheme().startsWith(QStringLiteral("http")) ? current.toString() : QString();
    const QString json = QString::fromUtf8(
        QJsonDocument(QJsonObject{{QStringLiteral("url"), url}}).toJson(QJsonDocument::Compact));
    m_panel->page()->runJavaScript(QStringLiteral(
        "window.desktopBookmarkPanel && typeof window.desktopBookmarkPanel.setCurrentUrl === 'function'"
        " ? window.desktopBookmarkPanel.setCurrentUrl(%1.url) : null").arg(json));
}

void BookmarkWindow::setDownloadDirectory(const QString& directory)
{
    if (!directory.trimmed().isEmpty())
        m_downloadDirectory = directory.trimmed();
}

QString BookmarkWindow::downloadDirectory() const
{
    return m_downloadDirectory.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) : m_downloadDirectory;
}

void BookmarkWindow::updateNavigationButtons()
{
    m_back->setEnabled(m_web->history()->canGoBack());
    m_forward->setEnabled(m_web->history()->canGoForward());
}

void BookmarkWindow::load(const QString& url)
{
    const QVariantMap address = BookmarkService::normalize(url);
    if (!address.value(QStringLiteral("ok")).toBool()) {
        // 地址栏输入或面板请求的地址不受支持时，在状态条给出原因。
        m_status->setText(address.value(QStringLiteral("error")).toString());
        return;
    }
    const QUrl target(address.value(QStringLiteral("data")).toMap().value(QStringLiteral("url")).toString());
    m_address->setText(target.toString());
    m_address->setCursorPosition(0);
    m_web->setUrl(target);
}

void BookmarkWindow::handleDownload(QWebEngineDownloadRequest* download)
{
    if (download->state() != QWebEngineDownloadRequest::DownloadRequested)
        return;
    const QString directory = downloadDirectory();
    if (!QDir().mkpath(directory)) {
        InterfaceLanguage::bind(m_status, "text", QStringLiteral("无法创建下载目录：") + directory);
        download->cancel();
        return;
    }
    const QString target = uniqueTarget(directory, safeFileName(download->suggestedFileName()));
    download->setDownloadDirectory(QFileInfo(target).absolutePath());
    download->setDownloadFileName(QFileInfo(target).fileName());
    m_downloads.append(download);
    connect(download, &QWebEngineDownloadRequest::receivedBytesChanged, this, &BookmarkWindow::updateDownloadStatus);
    connect(download, &QWebEngineDownloadRequest::totalBytesChanged, this, &BookmarkWindow::updateDownloadStatus);
    connect(download, &QWebEngineDownloadRequest::stateChanged, this, &BookmarkWindow::updateDownloadStatus);
    download->accept();
    updateDownloadStatus();
}

void BookmarkWindow::updateDownloadStatus()
{
    // 先清理已结束的下载，并记录其结果文案。
    for (auto iterator = m_downloads.begin(); iterator != m_downloads.end();) {
        auto* download = iterator->data();
        if (!download) {
            iterator = m_downloads.erase(iterator);
            continue;
        }
        const QString name = download->downloadFileName();
        switch (download->state()) {
        case QWebEngineDownloadRequest::DownloadCompleted:
            m_lastDownloadPath = QDir(download->downloadDirectory()).filePath(name);
            m_lastDownloadMessage = InterfaceLanguage::text(QStringLiteral("已完成：")) + m_lastDownloadPath;
            iterator = m_downloads.erase(iterator);
            continue;
        case QWebEngineDownloadRequest::DownloadCancelled:
            m_lastDownloadMessage = InterfaceLanguage::text(QStringLiteral("已取消下载 ")) + name;
            iterator = m_downloads.erase(iterator);
            continue;
        case QWebEngineDownloadRequest::DownloadInterrupted:
            m_lastDownloadMessage = InterfaceLanguage::text(QStringLiteral("下载中断："))
                + (download->interruptReasonString().isEmpty() ? InterfaceLanguage::text(QStringLiteral("未知原因"))
                    : download->interruptReasonString());
            iterator = m_downloads.erase(iterator);
            continue;
        default:
            ++iterator;
            break;
        }
    }
    if (m_downloads.isEmpty()) {
        m_status->setText(m_lastDownloadMessage);
        return;
    }
    auto* download = m_downloads.first().data();
    const qint64 received = download->receivedBytes();
    const qint64 total = download->totalBytes();
    QString progress = InterfaceLanguage::text(QStringLiteral("正在下载 %1")).arg(download->downloadFileName())
        + (m_downloads.size() > 1 ? InterfaceLanguage::text(QStringLiteral("（还有 %1 个任务）")).arg(m_downloads.size() - 1) : QString());
    if (total > 0)
        progress += QStringLiteral("：%1%（%2 / %3）").arg(qint64(received * 100.0 / total))
            .arg(readableSize(received), readableSize(total));
    else
        progress += InterfaceLanguage::text(QStringLiteral("：已接收 %1")).arg(readableSize(received));
    m_status->setText(progress);
}

void BookmarkWindow::closeEvent(QCloseEvent* event)
{
    // 清理已结束的下载，只确认仍在进行的任务。
    updateDownloadStatus();
    if (!m_downloads.isEmpty()) {
        if (QMessageBox::question(this, InterfaceLanguage::text(QStringLiteral("网页书签")),
                InterfaceLanguage::text(QStringLiteral("还有 %1 个下载正在进行，关闭窗口会中断它们。确定关闭吗？")).arg(m_downloads.size()),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        for (const auto& download : m_downloads) {
            if (download)
                download->cancel();
        }
        m_downloads.clear();
    }
    event->accept();
}
