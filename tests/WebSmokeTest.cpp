#include "app/WebWindow.h"
#include "app/GlobalHotkey.h"
#include "bridge/AppBridge.h"
#include "DownloadPageServer.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <memory>

// 在真实 WebEngine 与 WebChannel 中验证网页和 Qt 服务的完整调用链。
class WebSmokeTest final : public QObject
{
    Q_OBJECT
private slots:
    // 使用独立临时数据目录测试页面交互，不访问用户数据。
    void htmlAndBackend();
    // 检查两阶段热键切换以及配置写入失败后的旧组合保留。
    void settingsTransaction();
    // 从下载页面创建任务，验证目录回退、多任务和控制按钮的真实桥接。
    void downloadsThroughPage();
    // 下载历史损坏时只禁用下载操作，既有页面仍然可用。
    void damagedDownloadHistoryIsIsolated();
private:
    // 同步等待一次短 JavaScript 求值，带超时防止测试挂起。
    static QVariant evaluate(QWebEnginePage* page, const QString& source);
};

void WebSmokeTest::settingsTransaction()
{
    GlobalHotkey hotkey(false);
    QString error;
    QVERIFY(hotkey.setShortcut(0, 0x77, &error));
    QVERIFY(hotkey.prepareShortcut(2, 0x78, &error));
    QCOMPARE(hotkey.key(), 0x77);
    hotkey.cancelShortcut();
    QCOMPARE(hotkey.key(), 0x77);
    QVERIFY(hotkey.prepareShortcut(2, 0x78, &error));
    hotkey.commitShortcut();
    QCOMPARE(hotkey.key(), 0x78);
    QCOMPARE(hotkey.modifier(), 2);

    QTemporaryDir data;
    QVERIFY(data.isValid());
    QWidget owner;
    AppBridge bridge(data.path(), &owner, false);
    QVariantMap previous = bridge.getSettings().value("data").toMap();
    previous.insert(QStringLiteral("downloadDirectory"), data.path());
    QVERIFY(bridge.saveSettings(previous).value("ok").toBool());
    const QString config = data.filePath("setting/system_config.json");
    QVERIFY(QFile::rename(config, config + ".original"));
    QVERIFY(QDir().mkpath(config));
    const auto failed = bridge.saveSettings({{"fontSize", 20}, {"hotkeyModifier", 2}, {"hotkeyKey", 0x78}});
    QVERIFY(!failed.value("ok").toBool());
    QCOMPARE(bridge.getSettings().value("data").toMap(), previous);
    auto* active = bridge.findChild<GlobalHotkey*>();
    QVERIFY(active);
    QCOMPARE(active->key(), previous.value("hotkeyKey").toInt());
    QCOMPARE(active->modifier(), previous.value("hotkeyModifier").toInt());
}

QVariant WebSmokeTest::evaluate(QWebEnginePage* page, const QString& source)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QVariant result;
    bool finished = false;
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    // 回调上下文放入共享状态，避免超时后访问已经退出的栈变量。
    auto state = std::make_shared<QVariant>();
    auto done = std::make_shared<bool>(false);
    QPointer<QEventLoop> guardedLoop(&loop);
    page->runJavaScript(source, [state, done, guardedLoop](const QVariant& value) {
        *state = value;
        *done = true;
        if (guardedLoop)
            guardedLoop->quit();
    });
    timeout.start(8000);
    if (!*done)
        loop.exec();
    finished = *done;
    if (finished)
        result = *state;
    return result;
}

void WebSmokeTest::htmlAndBackend()
{
    QTemporaryDir data;
    QVERIFY(data.isValid());
    WebWindow window(data.path(), false);
    window.show();
    auto* page = window.webView()->page();
    QElapsedTimer timer;
    timer.start();
    bool ready = false;
    while (timer.elapsed() < 20000) {
        ready = evaluate(page, QStringLiteral("document.readyState === 'complete' && typeof window.desktopToolCanClose === 'function'")).toBool();
        if (ready)
            break;
        QTest::qWait(100);
    }
    QVERIFY2(ready, "HTML 页面未能完成初始化");
    QFile script(QStringLiteral(":/tests/web-smoke.js"));
    QVERIFY(script.open(QIODevice::ReadOnly));
    evaluate(page, QString::fromUtf8(script.readAll()));
    timer.restart();
    QVariantMap outcome;
    while (timer.elapsed() < 45000) {
        outcome = evaluate(page, QStringLiteral("window.__smokeResult || null")).toMap();
        if (!outcome.isEmpty())
            break;
        QTest::qWait(100);
    }
    QVERIFY2(!outcome.isEmpty(), "网页集成测试超时");
    QVERIFY2(outcome.value("ok").toBool(), qPrintable(outcome.value("error").toString()));
    qInfo().noquote() << outcome.value("summary").toString();
    const QString capturePath = qEnvironmentVariable("DESKTOPTOOL_TEST_CAPTURE");
    if (!capturePath.isEmpty()) {
        QTest::qWait(500);
        evaluate(page, QStringLiteral("document.querySelector('#toast-region').replaceChildren(); true"));
        QVERIFY(window.grab().save(capturePath));
        evaluate(page, QStringLiteral("document.querySelector('[data-page=notes]').click(); document.querySelector('.note-row-title').click(); true"));
        QTest::qWait(200);
        QVERIFY(window.grab().save(capturePath + ".notes.png"));
        evaluate(page, QStringLiteral("document.querySelector('[data-page=settings]').click(); true"));
        QTest::qWait(100);
        QVERIFY(window.grab().save(capturePath + ".settings.png"));
    }
}

void WebSmokeTest::downloadsThroughPage()
{
    QTemporaryDir data;
    QVERIFY(data.isValid());
    const QString defaultDirectory = data.filePath(QStringLiteral("default-downloads"));
    const QString customDirectory = data.filePath(QStringLiteral("custom-downloads"));
    QVERIFY(QDir().mkpath(defaultDirectory));
    QVERIFY(QDir().mkpath(customDirectory));
    QFile blocked(data.filePath(QStringLiteral("not-a-directory")));
    QVERIFY(blocked.open(QIODevice::WriteOnly));
    blocked.write("existing user file");
    blocked.close();
    DownloadPageServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    WebWindow window(data.filePath(QStringLiteral("app-data")), false);
    window.show();
    auto* page = window.webView()->page();
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(page, QStringLiteral("document.documentElement.dataset.ready === 'true'")).toBool(), 20000);
    const QJsonObject fixture{
        {QStringLiteral("baseUrl"), QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort())},
        {QStringLiteral("defaultDirectory"), defaultDirectory},
        {QStringLiteral("customDirectory"), customDirectory},
        {QStringLiteral("invalidDirectory"), blocked.fileName()}
    };
    evaluate(page, QStringLiteral("window.__downloadFixture = ")
        + QString::fromUtf8(QJsonDocument(fixture).toJson(QJsonDocument::Compact)) + QStringLiteral("; true"));
    QFile script(QStringLiteral(":/tests/download-smoke.js"));
    QVERIFY(script.open(QIODevice::ReadOnly));
    evaluate(page, QString::fromUtf8(script.readAll()));
    QVariantMap outcome;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 45000) {
        outcome = evaluate(page, QStringLiteral("window.__downloadSmokeResult || null")).toMap();
        if (!outcome.isEmpty())
            break;
        QTest::qWait(100);
    }
    QVERIFY2(!outcome.isEmpty(), "下载页面测试超时");
    QVERIFY2(outcome.value("ok").toBool(), qPrintable(outcome.value("error").toString()));
    for (const auto& value : outcome.value(QStringLiteral("completed")).toList()) {
        const auto task = value.toMap();
        QFile downloaded(task.value(QStringLiteral("filePath")).toString());
        QVERIFY(downloaded.open(QIODevice::ReadOnly));
        const QByteArray expected = DownloadPageServer::body(QUrl(task.value(QStringLiteral("url")).toString()).path().toUtf8());
        QCOMPARE(downloaded.readAll(), expected);
    }
    QVERIFY(blocked.open(QIODevice::ReadOnly));
    QCOMPARE(blocked.readAll(), QByteArray("existing user file"));
    const QString capturePath = qEnvironmentVariable("DESKTOPTOOL_TEST_CAPTURE");
    if (!capturePath.isEmpty()) {
        window.resize(1180, 1260);
        evaluate(page, QStringLiteral("document.querySelector('#toast-region').replaceChildren(); document.querySelector('#main-content').scrollTop = 0; window.scrollTo(0, 0); true"));
        QTest::qWait(200);
        QVERIFY(window.grab().save(capturePath + ".downloads.png"));
        evaluate(page, QStringLiteral("document.querySelector('[data-page=settings]').click(); true"));
        QTest::qWait(100);
        QVERIFY(window.grab().save(capturePath + ".download-settings.png"));
    }
}

void WebSmokeTest::damagedDownloadHistoryIsIsolated()
{
    QTemporaryDir data;
    QVERIFY(data.isValid());
    QFile history(data.filePath(QStringLiteral("download-tasks.v1.json")));
    QVERIFY(history.open(QIODevice::WriteOnly));
    history.write("{broken");
    history.close();
    WebWindow window(data.path(), false);
    window.show();
    auto* page = window.webView()->page();
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(page, QStringLiteral("document.documentElement.dataset.ready === 'true'")).toBool(), 20000);
    QVERIFY(evaluate(page, QStringLiteral("!document.querySelector('#workspace').inert && document.querySelector('#connection-error').hidden")).toBool());
    evaluate(page, QStringLiteral("document.querySelector('[data-page=downloads]').click(); true"));
    QVERIFY(evaluate(page, QStringLiteral("!document.querySelector('#download-list-error').hidden && document.querySelector('#download-list-error').textContent.length > 0")).toBool());
    evaluate(page, QStringLiteral("document.querySelector('[data-page=shortcuts]').click(); true"));
    QVERIFY(evaluate(page, QStringLiteral("!document.querySelector('#page-shortcuts').hidden")).toBool());
    QVERIFY(history.open(QIODevice::ReadOnly));
    QCOMPARE(history.readAll(), QByteArray("{broken"));
}

// 初始化测试用 Qt 应用；不会启用托盘、系统热键或单实例服务。
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("DesktopToolWebTest"));
    app.setApplicationVersion(QStringLiteral("2.1.0-test"));
    WebSmokeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "WebSmokeTest.moc"
