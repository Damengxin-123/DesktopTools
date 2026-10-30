#include "app/BookmarkWindow.h"
#include "app/InterfaceLanguage.h"
#include "services/BookmarkService.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLineEdit>
#include <QPointer>
#include <QWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <memory>

// 在真实 WebEngine + WebChannel 环境中驱动书签面板页：
// 验证内置资源可加载、面板与后端双向联通，以及点击书签能在右侧浏览区打开网址。
class BookmarkWindowSmokeTest final : public QObject
{
    Q_OBJECT
private slots:
    void init();    // 每个用例前复位窗口指针。
    void panelPipeline();
    void cleanup(); // 用例结束后关闭窗口，避免 WebEngine 在退出时拆卸崩溃。

private:
    // 同步等待一次短 JavaScript 求值，带超时防止测试挂起。
    QVariant evaluate(QWebEnginePage* page, const QString& source, int timeoutMs = 8000);
    // 轮询等待条件成立。
    bool waitFor(QWebEnginePage* page, const QString& condition, int timeoutMs);
    // 返回左侧面板页：grantChildren 会同时找到右侧浏览区的页面。
    QWebEnginePage* panelPage(QWidget* window);
    QPointer<BookmarkWindow> m_window;
};

void BookmarkWindowSmokeTest::init()
{
    m_window.clear();
}

void BookmarkWindowSmokeTest::cleanup()
{
    if (m_window) {
        m_window->close();
        m_window.clear();
        QTest::qWait(300);
    }
}

QVariant BookmarkWindowSmokeTest::evaluate(QWebEnginePage* page, const QString& source, int timeoutMs)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    auto state = std::make_shared<QVariant>();
    auto done = std::make_shared<bool>(false);
    QPointer<QEventLoop> guardedLoop(&loop);
    page->runJavaScript(source, [state, done, guardedLoop](const QVariant& value) {
        *state = value;
        *done = true;
        if (guardedLoop)
            guardedLoop->quit();
    });
    timeout.start(timeoutMs);
    if (!*done)
        loop.exec();
    return *done ? *state : QVariant();
}

bool BookmarkWindowSmokeTest::waitFor(QWebEnginePage* page, const QString& condition, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        const QVariant value = evaluate(page, condition, 3000);
        if (value.isValid() && value.toBool())
            return true;
        QTest::qWait(120);
    }
    return false;
}

QWebEnginePage* BookmarkWindowSmokeTest::panelPage(QWidget* window)
{
    for (QWebEnginePage* page : window->findChildren<QWebEnginePage*>()) {
        if (page->requestedUrl().scheme() == QStringLiteral("qrc"))
            return page;
    }
    // 页面尚未完成首次导航时按视图顺序兜底查找。
    return window->findChild<QWebEnginePage*>();
}

void BookmarkWindowSmokeTest::panelPipeline()
{
    QTemporaryDir data;
    QVERIFY(data.isValid());
    BookmarkService service(data.path());
    const auto first = service.save({{QStringLiteral("name"), QStringLiteral("第一个网站")},
        {QStringLiteral("url"), QStringLiteral("example.com")}});
    QVERIFY2(first.value(QStringLiteral("ok")).toBool(), qPrintable(first.value(QStringLiteral("error")).toString()));
    const auto second = service.save({{QStringLiteral("name"), QStringLiteral("第二个网站")},
        {QStringLiteral("url"), QStringLiteral("https://example.org/index.html")}});
    QVERIFY2(second.value(QStringLiteral("ok")).toBool(), qPrintable(second.value(QStringLiteral("error")).toString()));

    auto* window = BookmarkWindow::open(&service, data.path(), nullptr);
    QVERIFY(window);
    m_window = window;
    window->show();
    // 无父对象模式仅用于独立面板回归测试，实际入口由主窗口持有。
    QVERIFY(window->isWindow());
    auto* page = panelPage(window);
    QVERIFY(page);

    // 面板页内的资源必须由打包文件提供，并完成首次列表渲染。
    QVERIFY2(waitFor(page, QStringLiteral(
        "document.readyState === 'complete'"
        " && document.querySelectorAll('.bookmark-row').length === 2"), 30000), "书签面板页未完成初始化");
    InterfaceLanguage::setLanguage(QStringLiteral("en"));
    QVERIFY(waitFor(page, QStringLiteral("document.documentElement.lang === 'en'"), 10000));
    QCOMPARE(evaluate(page, QStringLiteral("document.querySelector('h1').textContent")).toString(), QStringLiteral("Bookmarks"));
    InterfaceLanguage::setLanguage(QStringLiteral("zh-CN"));
    QVERIFY(waitFor(page, QStringLiteral("document.documentElement.lang === 'zh-CN'"), 10000));
    QCOMPARE(evaluate(page, QStringLiteral(
        "document.querySelector('.bookmark-row .bookmark-name').textContent")).toString(),
        QStringLiteral("第一个网站"));
    QCOMPARE(evaluate(page, QStringLiteral(
        "document.querySelector('.bookmark-row .bookmark-url').textContent")).toString(),
        QStringLiteral("https://example.com"));

    // 点击列表项：右侧浏览区的地址栏应显示规范化后的地址。
    evaluate(page, QStringLiteral("document.querySelector('.bookmark-row .bookmark-content').click(), true"));
    QTRY_VERIFY_WITH_TIMEOUT(window->findChild<QLineEdit*>()->text() == QStringLiteral("https://example.com"), 15000);

    // 新建对话框：不受支持的协议立即返回错误，且允许手动输入名称后保存。
    evaluate(page, QStringLiteral("document.getElementById('bookmark-add').click(), true"));
    QVERIFY(waitFor(page, QStringLiteral("document.getElementById('bookmark-dialog').open"), 10000));
    evaluate(page, QStringLiteral(
        "document.getElementById('bookmark-url').value = 'ftp://example.net';"
        "document.getElementById('bookmark-url').dispatchEvent(new Event('input'));"
        "document.getElementById('bookmark-probe').click(); true"));
    QVERIFY2(waitFor(page, QStringLiteral(
        "document.getElementById('bookmark-probe-state').textContent.indexOf('无法自动获取名称') >= 0"), 15000),
        "不支持的协议未返回检测失败");
    evaluate(page, QStringLiteral(
        "document.getElementById('bookmark-url').value = 'https://example.net/new';"
        "document.getElementById('bookmark-name').value = '手动命名的网站';"
        "document.getElementById('bookmark-dialog-submit').click(); true"));
    QVERIFY2(waitFor(page, QStringLiteral(
        "!document.getElementById('bookmark-dialog').open"
        " && document.querySelectorAll('.bookmark-row').length === 3"), 15000), "书签未保存到面板列表");
    QCOMPARE(evaluate(page, QStringLiteral(
        "document.querySelectorAll('.bookmark-row .bookmark-name')[2].textContent")).toString(),
        QStringLiteral("手动命名的网站"));

    // 搜索按名称与网址过滤。
    evaluate(page, QStringLiteral(
        "const input = document.getElementById('bookmark-search');"
        "input.value = 'example.org';"
        "input.dispatchEvent(new Event('input')); true"));
    QVERIFY(waitFor(page, QStringLiteral("document.querySelectorAll('.bookmark-row').length === 1"), 10000));
    QCOMPARE(service.snapshot().value(QStringLiteral("data")).toMap()
        .value(QStringLiteral("items")).toList().size(), 3);

    // 非法地址保存失败后必须保留输入，修正后应真正更新持久化记录。
    evaluate(page, QStringLiteral(
        "document.querySelector('.bookmark-actions button').click();"
        "document.getElementById('bookmark-name').value = '修改后的名称';"
        "document.getElementById('bookmark-url').value = 'file:///private';"
        "document.getElementById('bookmark-dialog-submit').click(); true"));
    QVERIFY(waitFor(page, QStringLiteral("!document.getElementById('bookmark-dialog-error').hidden"), 10000));
    QVERIFY(evaluate(page, QStringLiteral("document.getElementById('bookmark-dialog').open")).toBool());
    evaluate(page, QStringLiteral(
        "document.getElementById('bookmark-url').value = 'https://example.org/edited';"
        "document.getElementById('bookmark-dialog-submit').click(); true"));
    QVERIFY(waitFor(page, QStringLiteral("!document.getElementById('bookmark-dialog').open && document.querySelector('.bookmark-name').textContent === '修改后的名称'"), 10000));
    BookmarkService reloaded(data.path());
    const auto savedItems = reloaded.snapshot().value("data").toMap().value("items").toList();
    QCOMPARE(savedItems.size(), 3);
    QCOMPARE(savedItems[1].toMap().value("name").toString(), QStringLiteral("修改后的名称"));

    // 筛选后批量删除只操作当前可见选择，保留隐藏的已选项。
    evaluate(page, QStringLiteral(
        "window.confirm = () => true;"
        "document.getElementById('bookmark-search').value = '';"
        "document.getElementById('bookmark-search').dispatchEvent(new Event('input'));"
        "document.getElementById('bookmark-select-all').click();"
        "document.getElementById('bookmark-search').value = 'example.org';"
        "document.getElementById('bookmark-search').dispatchEvent(new Event('input'));"
        "document.getElementById('bookmark-delete-selected').click(); true"));
    QVERIFY(waitFor(page, QStringLiteral("document.querySelectorAll('.bookmark-row').length === 0"), 10000));
    QCOMPARE(service.snapshot().value("data").toMap().value("items").toList().size(), 2);
}

QTEST_MAIN(BookmarkWindowSmokeTest)
#include "BookmarkWindowSmokeTest.moc"
