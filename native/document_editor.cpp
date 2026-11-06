#include "document_editor.h"
#include "document_package.h"
#include <QFile>
#include <QInputDialog>
#include <QLineEdit>

#include "bridge/AppBridge.h"
#include "services/ServiceResult.h"

#include <QAbstractButton>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QPalette>
#include <QRegularExpression>
#include <QSaveFile>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>
#include <utility>

namespace {
// 仅允许内置编辑器资源、WebChannel 客户端和文档内嵌的图片。
bool allowedResource(const QUrl& url)
{
    return url.scheme() == QStringLiteral("qrc")
        && (url.path().startsWith(QStringLiteral("/document/"))
            || url.path() == QStringLiteral("/qtwebchannel/qwebchannel.js"));
}

// 即使正文中出现外部引用，也不允许页面访问网络或本地文件。
class DocumentResources final : public QWebEngineUrlRequestInterceptor
{
public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;

    // 只放行应用资源和 data: 图片。
    void interceptRequest(QWebEngineUrlRequestInfo& info) override
    {
        const auto url = info.requestUrl();
        const bool inlineImage = url.scheme() == QStringLiteral("data")
            && info.resourceType() == QWebEngineUrlRequestInfo::ResourceTypeImage;
        info.block(!allowedResource(url) && !inlineImage);
    }
};

// 阻止文档链接改变承载本地接口的页面。
class DocumentPage final : public QWebEnginePage
{
public:
    using QWebEnginePage::QWebEnginePage;

protected:
    // 主框架只允许加载内置编辑器。
    bool acceptNavigationRequest(const QUrl& url, NavigationType, bool isMainFrame) override
    {
        return isMainFrame && url == QUrl(QStringLiteral("qrc:/document/editor.html"));
    }
};
}

DocumentEditor::DocumentEditor(AppBridge& bridge)
    : m_bridge(bridge)
{
    setWindowIcon(QIcon(QStringLiteral(":/images/app_icon.png")));
    setMinimumSize(860, 550);
    resize(1280, 820);

    auto* view = new QWebEngineView(this);
    auto* profile = new QWebEngineProfile(view);
    profile->setUrlRequestInterceptor(new DocumentResources(profile));
    auto* page = new DocumentPage(profile, view);
    // 确保页面先于无持久化配置的独立 profile 销毁。
    connect(view, &QObject::destroyed, page, [page] { delete page; });
    view->setPage(page);
    view->setContextMenuPolicy(Qt::NoContextMenu);
    auto* channel = new QWebChannel(page);
    channel->registerObject(QStringLiteral("documentHost"), this);
    page->setWebChannel(channel);
    auto* settings = page->settings();
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    settings->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    settings->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, false);
    setCentralWidget(view);
    view->load(QUrl(QStringLiteral("qrc:/document/editor.html")));
}

QVariantMap DocumentEditor::openDocument(const QString& id, const QString& category, const QString& theme)
{
    setAppearance(theme);
    if (isVisible() && !id.isEmpty() && id == m_document.value("id").toString()) {
        focusEditor();
        return ServiceResult::success();
    }

    if (isVisible() && !mayDiscard())
        return ServiceResult::success();
    const auto catalog = m_bridge.getNotes();
    if (!catalog.value("ok").toBool())
        return catalog;
    auto categories = catalog.value("data").toMap().value("categories").toList();
    QVariantMap document;
    if (id.isEmpty()) {
        document.insert("categoryId", category);
    } else {
        auto result = m_bridge.getNote(id);
        if (result.value("requiresPassword").toBool()) {
            bool accepted = false;
            const auto password = QInputDialog::getText(this, QStringLiteral("解锁文档"), QStringLiteral("请输入文档密码："), QLineEdit::Password, {}, &accepted);
            if (!accepted) return ServiceResult::success();
            const auto path = result.value("packagePath").toString();
            const auto unlocked = DocumentPackage::read(path, password);
            if (!unlocked.value("ok").toBool()) return unlocked;
            DocumentPackage::remember(path, password);
            result = m_bridge.getNote(id);
        }
        if (!result.value("ok").toBool()) return result;
        document = result.value("data").toMap();
    }
    if (id != m_document.value("id").toString())
        DocumentPackage::forget(m_document.value("packagePath").toString());

    m_categories = std::move(categories);
    m_document = std::move(document);
    m_dirty = false;
    ++m_revision;
    const auto title = m_document.value("title").toString();
    setWindowTitle(title.isEmpty() ? QStringLiteral("新建文档 · DesktopTool")
                                  : QStringLiteral("%1 · 文档").arg(title));
    emit documentChanged(state());
    focusEditor();
    return ServiceResult::success();
}

void DocumentEditor::focusEditor()
{
    setWindowState(windowState() & ~Qt::WindowMinimized);
    show();
    raise();
    activateWindow();
}

void DocumentEditor::setAppearance(const QString& theme)
{
    const auto next = theme == QStringLiteral("gold") ? theme : QStringLiteral("dark");
    if (next == m_appearance)
        return;
    m_appearance = next;
    const bool light = next == QStringLiteral("gold");
    QPalette colors;
    colors.setColor(QPalette::Window, QColor(light ? "#fff9eb" : "#121c30"));
    colors.setColor(QPalette::WindowText, QColor(light ? "#352817" : "#eaf0ff"));
    colors.setColor(QPalette::Button, QColor(light ? "#f7edd9" : "#17233a"));
    colors.setColor(QPalette::ButtonText, colors.color(QPalette::WindowText));
    setPalette(colors);
    emit appearanceChanged(m_appearance);
}

QVariantMap DocumentEditor::state() const
{
    return {{"revision", m_revision}, {"document", m_document}, {"categories", m_categories},
            {"theme", m_appearance}};
}

QVariantMap DocumentEditor::saveDocument(int revision, const QVariantMap& input)
{
    if (revision != m_revision || !isVisible())
        return ServiceResult::failure(QStringLiteral("当前编辑会话已关闭。"));
    auto result = m_bridge.saveNote({{"id", m_document.value("id")}, {"title", input.value("title")},
                                    {"categoryId", input.value("categoryId")}, {"html", input.value("html")},
                                    {"styles", input.value("styles")}});
    if (!result.value("ok").toBool())
        return result;
    m_document = result.value("data").toMap();
    setWindowTitle(QStringLiteral("%1 · 文档").arg(m_document.value("title").toString()));
    // 正文保留当前 DOM 和撤销历史；只把保存后的元数据交回页面。
    auto summary = m_document;
    summary.remove("html");
    result.insert("data", summary);
    return result;
}

QVariantMap DocumentEditor::exportPackage(int revision)
{
    if (revision != m_revision || m_dirty || m_document.value("id").toString().isEmpty())
        return ServiceResult::failure(QStringLiteral("请先保存文档，再导出。"));
    const auto result = m_bridge.getNote(m_document.value("id").toString());
    if (!result.value("ok").toBool())
        return result;
    const auto document = result.value("data").toMap();
    auto name = document.value("title").toString();
    name.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    auto path = QFileDialog::getSaveFileName(this, QStringLiteral("导出文档"), name + QStringLiteral(".DaDocH"),
                                             QStringLiteral("DaDocH 文档 (*.DaDocH)"));
    if (path.isEmpty())
        return ServiceResult::success();
    if (!path.endsWith(QStringLiteral(".DaDocH"), Qt::CaseInsensitive))
        path += QStringLiteral(".DaDocH");
    const auto sourcePath = document.value("packagePath").toString();
    QByteArray bytes;
    if (!sourcePath.isEmpty()) {
        QFile source(sourcePath);
        if (!source.open(QIODevice::ReadOnly) || source.size() > 64 * 1024 * 1024)
            return ServiceResult::failure(QStringLiteral("无法读取已保存的文档包。"));
        bytes = source.readAll();
    } else {
        QString error;
        bytes = DocumentPackage::encode(document.value("html").toString(), {{"title", name}, {"styles", document.value("styles")}}, {}, error);
        if (bytes.isEmpty()) return ServiceResult::failure(error);
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return ServiceResult::failure(QStringLiteral("无法导出文档：") + file.errorString());
    return ServiceResult::success(QVariantMap{{"path", path}});
}

QVariantMap DocumentEditor::configurePassword(int revision)
{
    if (revision != m_revision || m_dirty || m_document.value("id").toString().isEmpty())
        return ServiceResult::failure(QStringLiteral("请先保存文档，再设置密码锁。"));
    bool accepted = false;
    const auto password = QInputDialog::getText(this, QStringLiteral("文档密码锁"),
        QStringLiteral("输入新密码，留空可解除密码锁："), QLineEdit::Password, {}, &accepted);
    if (!accepted) return ServiceResult::success();
    if (!password.isEmpty()) {
        const auto confirm = QInputDialog::getText(this, QStringLiteral("确认密码"),
            QStringLiteral("请再次输入新密码："), QLineEdit::Password, {}, &accepted);
        if (!accepted) return ServiceResult::success();
        if (confirm != password) return ServiceResult::failure(QStringLiteral("两次输入的密码不一致。"));
    }
    auto input = m_document;
    input.insert("password", password);
    const auto result = m_bridge.saveNote(input);
    if (!result.value("ok").toBool()) return result;
    m_document = result.value("data").toMap();
    return ServiceResult::success(QVariantMap{{"locked", m_document.value("locked")}});
}

QVariantMap DocumentEditor::importPackage(const QString& category, const QString& theme)
{
    if (isVisible() && !mayDiscard()) return ServiceResult::success();
    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("导入文档"), {},
        QStringLiteral("DaDocH 文档 (*.DaDocH *.dadoch)"));
    if (path.isEmpty()) return ServiceResult::success();
    QString password;
    auto result = DocumentPackage::read(path, password);
    if (result.value("requiresPassword").toBool()) {
        bool accepted = false;
        password = QInputDialog::getText(this, QStringLiteral("解锁文档"),
            QStringLiteral("请输入文档密码："), QLineEdit::Password, {}, &accepted);
        if (!accepted) return ServiceResult::success();
        result = DocumentPackage::read(path, password);
    }
    if (!result.value("ok").toBool()) return result;
    auto input = result.value("data").toMap();
    input.remove("id");
    input.insert("categoryId", category.isEmpty() ? QStringLiteral("default") : category);
    input.insert("password", password);
    if (input.value("title").toString().trimmed().isEmpty()) input.insert("title", QFileInfo(path).completeBaseName());
    const auto saved = m_bridge.saveNote(input);
    if (!saved.value("ok").toBool()) return saved;
    m_dirty = false;
    return openDocument(saved.value("data").toMap().value("id").toString(), category, theme);
}

void DocumentEditor::setDirty(int revision, bool dirty)
{
    if (revision == m_revision)
        m_dirty = dirty;
}

bool DocumentEditor::mayDiscard()
{
    if (!m_dirty)
        return true;
    QMessageBox confirm(QMessageBox::Question, QStringLiteral("未保存的文档"),
        QStringLiteral("当前文档有未保存的修改，是否放弃？"),
        QMessageBox::Discard | QMessageBox::Cancel, this);
    confirm.button(QMessageBox::Discard)->setText(QStringLiteral("放弃修改"));
    confirm.button(QMessageBox::Cancel)->setText(QStringLiteral("继续编辑"));
    confirm.setDefaultButton(QMessageBox::Cancel);
    return confirm.exec() == QMessageBox::Discard;
}

void DocumentEditor::closeEvent(QCloseEvent* event)
{
    if (!mayDiscard()) {
        event->ignore();
        return;
    }
    DocumentPackage::forget(m_document.value("packagePath").toString());
    m_dirty = false;
    ++m_revision;
    event->accept();
    emit editorClosed();
}
