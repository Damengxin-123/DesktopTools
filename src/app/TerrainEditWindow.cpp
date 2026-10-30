#include "InterfaceLanguage.h"
#include "TerrainEditWindow.h"

#include "bridge/TerrainBridge.h"
#include "services/TerrainService.h"

#include <QAbstractButton>
#include <QCloseEvent>
#include <QMessageBox>
#include <QPointer>
#include <QTimer>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>

QHash<QString, QPointer<TerrainEditWindow>> TerrainEditWindow::s_open;

namespace {
// 只允许加载随程序打包的地形编辑页面，禁止外部与本地文件资源。
class TerrainResources final : public QWebEngineUrlRequestInterceptor
{
public:
    explicit TerrainResources(QObject* parent) : QWebEngineUrlRequestInterceptor(parent) {}
    void interceptRequest(QWebEngineUrlRequestInfo& info) override
    {
        const auto scheme = info.requestUrl().scheme();
        if (scheme != "qrc" && scheme != "data" && scheme != "about" && scheme != "blob")
            info.block(true);
    }
};

// 主文档只接受本项目的 terrain.html，防止网页导航到其他页面获取桥接权限。
class TerrainPage final : public QWebEnginePage
{
public:
    TerrainPage(QWebEngineProfile* profile, QObject* parent) : QWebEnginePage(profile, parent) {}
protected:
    bool acceptNavigationRequest(const QUrl& url, NavigationType type, bool isMainFrame) override
    {
        Q_UNUSED(type)
        return isMainFrame && url.scheme() == "qrc" && url.path() == "/web/terrain.html";
    }
};
}

TerrainEditWindow* TerrainEditWindow::open(TerrainService* service, const QString& projectId,
                                           QWidget* parent)
{
    // 同一项目重复打开时优先激活已打开的编辑窗口。
    const auto existing = s_open.constFind(projectId);
    if (existing != s_open.constEnd() && existing->data()) {
        (*existing)->activateWindow();
        (*existing)->raise();
        return existing->data();
    }
    const QVariantMap meta = service->readMeta(projectId);
    if (!meta.value(QStringLiteral("ok")).toBool()) {
        QMessageBox::warning(parent, InterfaceLanguage::text(QStringLiteral("地形编辑")),
            meta.value(QStringLiteral("error")).toString());
        return nullptr;
    }
    return new TerrainEditWindow(service, projectId,
        meta.value(QStringLiteral("data")).toMap().value(QStringLiteral("title")).toString(), parent);
}

bool TerrainEditWindow::isProjectOpen(const QString& projectId)
{
    const auto it = s_open.constFind(projectId);
    return it != s_open.constEnd() && it->data();
}

void TerrainEditWindow::activateExisting(const QString& projectId)
{
    const auto it = s_open.constFind(projectId);
    if (it != s_open.constEnd() && it->data()) {
        (*it)->activateWindow();
        (*it)->raise();
    }
}

TerrainEditWindow::TerrainEditWindow(TerrainService* service, const QString& projectId,
                                     const QString& title, QWidget* parent)
    // 独立顶级窗口：主窗口最小化时不随之最小化；parent 仅用于失败提示弹窗。
    : QMainWindow(nullptr), m_view(new QWebEngineView(this)),
      m_bridge(new TerrainBridge(service, projectId, this, this))
{
    Q_UNUSED(parent)
    InterfaceLanguage::bind(this, "windowTitle", title + QStringLiteral(" — 地形编辑"));
    setWindowIcon(QIcon(QStringLiteral(":/images/app_icon.ico")));
    resize(1440, 880);
    setMinimumSize(1024, 640);
    setCentralWidget(m_view);

    auto* profile = new QWebEngineProfile(this);
    profile->setUrlRequestInterceptor(new TerrainResources(profile));
    auto* page = new TerrainPage(profile, m_view);
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
    // 网页保存完成后主动请求关闭；等待期间关闭保护保持生效。
    connect(m_bridge, &TerrainBridge::closeRequested, this, [this]() { close(); });
    s_open.insert(projectId, this);
    // 关闭窗口时从注册表移除（WA_DeleteOnClose 自毁后指针自动失效，这里负责清理键）。
    connect(this, &QObject::destroyed, this, [projectId]() { s_open.remove(projectId); });
    m_view->load(QUrl(QStringLiteral("qrc:/web/terrain.html?id=") + projectId));
    setAttribute(Qt::WA_DeleteOnClose);
}

void TerrainEditWindow::closeEvent(QCloseEvent* event)
{
    if (!m_bridge->dirty()) {
        event->accept();
        return;
    }
    if (m_saveWaiting) {
        // 网页正在回传保存数据；保存成功后网页会再次请求关闭。
        event->ignore();
        return;
    }
    activateWindow();
    raise();
    QMessageBox box(this);
    InterfaceLanguage::bind(&box, "windowTitle", QStringLiteral("关闭地形编辑"));
    InterfaceLanguage::bind(&box, "text", QStringLiteral("“%1”存在未保存的修改，是否保存？")
        .arg(windowTitle().section(QStringLiteral(" — "), 0, 0)));
    box.setStandardButtons(QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    InterfaceLanguage::bind(box.button(QMessageBox::Save), "text", QStringLiteral("保存并关闭"));
    InterfaceLanguage::bind(box.button(QMessageBox::Discard), "text", QStringLiteral("放弃修改"));
    InterfaceLanguage::bind(box.button(QMessageBox::Cancel), "text", QStringLiteral("取消"));
    const int choice = box.exec();
    if (choice == QMessageBox::Cancel) {
        event->ignore();
        return;
    }
    if (choice == QMessageBox::Discard) {
        m_bridge->setDirty(false);
        event->accept();
        return;
    }
    // 保存并关闭：触发网页收集数据并提交；设置超时避免页面无响应时永久卡住。
    m_saveWaiting = true;
    event->ignore();
    QPointer<TerrainEditWindow> self(this);
    QTimer::singleShot(30000, this, [this]() {
        if (m_saveWaiting) {
            m_saveWaiting = false;
            m_bridge->clearSavePending();
        }
    });
    m_view->page()->runJavaScript(QStringLiteral(
        "typeof window.terrainEditor === 'object' && window.terrainEditor !== null"
        " && typeof window.terrainEditor.requestSaveAndClose === 'function'"
        " ? window.terrainEditor.requestSaveAndClose() : null"),
        [self](const QVariant& result) {
            if (!self)
                return;
            if (!result.isValid()) {
                // 页面没有响应：解除等待状态，让用户可以再次选择放弃修改。
                self->m_saveWaiting = false;
                self->m_bridge->clearSavePending();
            }
        });
}
