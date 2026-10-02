#include "app/WebWindow.h"
#include "app/GlobalHotkey.h"
#include "bridge/AppBridge.h"
#include "services/ClipboardService.h"
#include "services/EmojiService.h"
#include "DownloadPageServer.h"
#include "TorrentTestPeer.h"

#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QImage>
#include <QPainter>
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
    // 使用本机磁力做种端验证解析、勾选弹窗及确认后只下载选中文件。
    void magnetsThroughPage();
    // 下载历史损坏时只禁用下载操作，既有页面仍然可用。
    void damagedDownloadHistoryIsIsolated();
    // 验证剪贴板类型设置、卡片、搜索、置顶、详情和批量删除。
    void clipboardThroughPage();
    // 剪贴板索引损坏时保留原文件，其他页面继续可用。
    void damagedClipboardHistoryIsIsolated();
    // 验证表情页卡片、关键字搜索、分类筛选、编辑、复制文件与删除。
    void emojiThroughPage();
    // 表情库损坏时仅禁用表情操作，其他页面继续可用。
    void damagedEmojiLibraryIsIsolated();
    // 验证屏幕示意图、切换目标、单屏应用和恢复系统壁纸的真实桥接。
    void wallpaperThroughPage();
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
        evaluate(page, QStringLiteral("document.querySelector('[data-page=settings]').click(); document.querySelector('.about-card').scrollIntoView(); true"));
        QTest::qWait(200);
        QVERIFY(window.grab().save(capturePath + ".settings.png"));
        evaluate(page, QStringLiteral("document.querySelector('[data-page=gridmap]').click(); true"));
        QTest::qWait(400);
        evaluate(page, QStringLiteral("const row = document.querySelector('.gridmap-row'); row && row.dispatchEvent(new MouseEvent('dblclick', { bubbles: true })); true"));
        QTest::qWait(500);
        QVERIFY(window.grab().save(capturePath + ".gridmap.png"));
        // 橡皮擦选中态与空格框选预览各截一张，便于人工检查工具状态。
        evaluate(page, QStringLiteral("document.querySelector('#gridmap-eraser').click(); true"));
        QTest::qWait(200);
        QVERIFY(window.grab().save(capturePath + ".gridmap-eraser.png"));
        evaluate(page, QStringLiteral("document.querySelector('#gridmap-eraser').click();"
            "document.dispatchEvent(new KeyboardEvent('keydown', { key: ' ', bubbles: true }));"
            "const c = document.querySelector('#gridmap-canvas'); const r = c.getBoundingClientRect();"
            "const opts = (x, y) => ({ bubbles: true, cancelable: true, button: 0, clientX: r.left + r.width / 2 + x, clientY: r.top + r.height / 2 + y, pointerId: 9 });"
            "c.dispatchEvent(new PointerEvent('pointerdown', opts(-60, -40)));"
            "c.dispatchEvent(new PointerEvent('pointermove', opts(80, 40))); true"));
        QTest::qWait(300);
        QVERIFY(window.grab().save(capturePath + ".gridmap-box.png"));
        evaluate(page, QStringLiteral("document.dispatchEvent(new KeyboardEvent('keyup', { key: ' ', bubbles: true }));"
            "document.querySelector('#gridmap-canvas').dispatchEvent(new PointerEvent('pointerup', { bubbles: true, button: 0, pointerId: 9 })); true"));
        QTest::qWait(200);
        // 画笔半径：涂出一个圆形色块并悬停显示笔刷范围，便于人工检查。
        evaluate(page, QStringLiteral("(() => { const radius = document.querySelector('#gridmap-brush-radius'); "
            "radius.value = '3'; radius.dispatchEvent(new Event('change', { bubbles: true }));"
            "const c = document.querySelector('#gridmap-canvas'); const r = c.getBoundingClientRect();"
            "const at = (x, y) => ({ bubbles: true, cancelable: true, button: 0, clientX: r.left + r.width / 2 + x, clientY: r.top + r.height / 2 + y, pointerId: 12 });"
            "c.dispatchEvent(new PointerEvent('pointerdown', at(-150, -120)));"
            "c.dispatchEvent(new PointerEvent('pointerup', at(-150, -120)));"
            "c.dispatchEvent(new PointerEvent('pointermove', at(60, -140))); })(); true"));
        QTest::qWait(300);
        QVERIFY(window.grab().save(capturePath + ".gridmap-radius.png"));
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
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(page, QStringLiteral("document.documentElement && document.documentElement.dataset.ready === 'true'")).toBool(), 20000);
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

void WebSmokeTest::magnetsThroughPage()
{
    QTemporaryDir data;
    QVERIFY(data.isValid());
    const QString source = data.filePath(QStringLiteral("seed"));
    const QString destination = data.filePath(QStringLiteral("downloads"));
    QVERIFY(QDir().mkpath(destination));
    TorrentTestPeer peer(source);
    QVERIFY2(peer.isValid(), qPrintable(peer.error()));
    TorrentTestPeer secondPeer(data.filePath(QStringLiteral("second-seed")), 1);
    QVERIFY2(secondPeer.isValid(), qPrintable(secondPeer.error()));
    WebWindow window(data.filePath(QStringLiteral("app-data")), false);
    window.show();
    auto* bridge = window.findChild<AppBridge*>();
    QVERIFY(bridge);
    auto* page = window.webView()->page();
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(page, QStringLiteral("document.documentElement && document.documentElement.dataset.ready === 'true'")).toBool(), 20000);
    const QJsonObject fixture = QJsonObject::fromVariantMap({
        {QStringLiteral("magnet"), peer.magnet()}, {QStringLiteral("directory"), destination},
        {QStringLiteral("files"), peer.files()}, {QStringLiteral("holdForInspection"), true},
        {QStringLiteral("magnetSecond"), secondPeer.magnet()}});
    evaluate(page, QStringLiteral("window.__magnetFixture = ")
        + QString::fromUtf8(QJsonDocument(fixture).toJson(QJsonDocument::Compact)) + QLatin1Char(';'));
    QFile script(QStringLiteral(":/tests/magnet-smoke.js"));
    QVERIFY(script.open(QIODevice::ReadOnly));
    evaluate(page, QString::fromUtf8(script.readAll()));
    QElapsedTimer timer;
    timer.start();
    QVariantMap outcome;
    bool inspected = false; // 确认前只检查一次磁盘与离屏弹窗。
    while (timer.elapsed() < 60000) {
        const auto ready = evaluate(page, QStringLiteral("window.__magnetSelectionReady || null")).toMap();
        if (!inspected && !ready.isEmpty()) {
            QVariantMap waiting;
            const auto items = bridge->getDownloads().value(QStringLiteral("data")).toMap().value(QStringLiteral("items")).toList();
            for (const auto& item : items) {
                if (item.toMap().value(QStringLiteral("id")) == ready.value(QStringLiteral("id")))
                    waiting = item.toMap();
            }
            QCOMPARE(waiting.value(QStringLiteral("status")).toString(), QStringLiteral("awaiting_selection"));
            QVERIFY(!waiting.value(QStringLiteral("selectionConfirmed")).toBool());
            QCOMPARE(waiting.value(QStringLiteral("bytesReceived")).toLongLong(), qint64(0));
            for (const auto& item : peer.files())
                QVERIFY(!QFileInfo::exists(QDir(waiting.value(QStringLiteral("filePath")).toString()).filePath(item.toMap().value(QStringLiteral("path")).toString())));
            const QString capturePath = qEnvironmentVariable("DESKTOPTOOL_TEST_CAPTURE");
            if (!capturePath.isEmpty()) {
                window.resize(1180, 960); // 离屏模式通过尺寸变化请求 Chromium 合成最新弹窗帧。
                QTest::qWait(500);
                QVERIFY(window.grab().save(capturePath + QStringLiteral(".magnet-selection.png")));
            }
            inspected = true;
            evaluate(page, QStringLiteral("window.__magnetAllowConfirm = true; true"));
        }
        outcome = evaluate(page, QStringLiteral("window.__magnetSmokeResult || null")).toMap();
        if (!outcome.isEmpty())
            break;
        QTest::qWait(50);
    }
    QVERIFY2(!outcome.isEmpty(), "磁力下载网页测试超时");
    QVERIFY2(outcome.value(QStringLiteral("ok")).toBool(), qPrintable(outcome.value(QStringLiteral("error")).toString()));
    QVERIFY(inspected);
    const QString id = outcome.value(QStringLiteral("id")).toString();
    const auto filesResult = bridge->getDownloadFiles(id);
    QVERIFY(filesResult.value(QStringLiteral("ok")).toBool());
    QVariantMap completed;
    for (const auto& item : bridge->getDownloads().value(QStringLiteral("data")).toMap().value(QStringLiteral("items")).toList()) {
        if (item.toMap().value(QStringLiteral("id")).toString() == id)
            completed = item.toMap();
    }
    QCOMPARE(completed.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
    QCOMPARE(completed.value(QStringLiteral("selectedCount")).toInt(), 1);
    for (const auto& item : filesResult.value(QStringLiteral("data")).toMap().value(QStringLiteral("files")).toList()) {
        const auto file = item.toMap();
        const QString path = QDir(completed.value(QStringLiteral("filePath")).toString()).filePath(file.value(QStringLiteral("path")).toString());
        if (file.value(QStringLiteral("selected")).toBool()) {
            QFile actual(path), expected(QDir(source).filePath(file.value(QStringLiteral("path")).toString()));
            QVERIFY(actual.open(QIODevice::ReadOnly));
            QVERIFY(expected.open(QIODevice::ReadOnly));
            QCOMPARE(actual.readAll(), expected.readAll());
        } else {
            QVERIFY2(!QFileInfo::exists(path), qPrintable(path));
        }
    }
    qInfo().noquote() << outcome.value(QStringLiteral("summary")).toString();
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
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(page, QStringLiteral("document.documentElement && document.documentElement.dataset.ready === 'true'")).toBool(), 20000);
    QVERIFY(evaluate(page, QStringLiteral("!document.querySelector('#workspace').inert && document.querySelector('#connection-error').hidden")).toBool());
    evaluate(page, QStringLiteral("document.querySelector('[data-page=downloads]').click(); true"));
    QVERIFY(evaluate(page, QStringLiteral("!document.querySelector('#download-list-error').hidden && document.querySelector('#download-list-error').textContent.length > 0")).toBool());
    evaluate(page, QStringLiteral("document.querySelector('[data-page=shortcuts]').click(); true"));
    QVERIFY(evaluate(page, QStringLiteral("!document.querySelector('#page-shortcuts').hidden")).toBool());
    QVERIFY(history.open(QIODevice::ReadOnly));
    QCOMPARE(history.readAll(), QByteArray("{broken"));
}

void WebSmokeTest::clipboardThroughPage()
{
    QTemporaryDir directory;
    WebWindow window(directory.path(), false);
    window.show();
    auto* page = window.webView()->page();
    auto* service = window.findChild<ClipboardService*>();
    QVERIFY(service);
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(page, QStringLiteral("document.documentElement.dataset.ready === 'true'")).toBool(), 20000);
    evaluate(page, QStringLiteral("document.querySelector('[data-page=clipboard]').click(); document.querySelector('#clipboard-types input[value=text]').click(); true"));
    QTRY_VERIFY(service->snapshot().value("data").toMap().value("types").toStringList().contains("text"));
    QTRY_VERIFY(!evaluate(page, QStringLiteral("document.querySelector('#clipboard-types').disabled")).toBool());
    evaluate(page, QStringLiteral("document.querySelector('#clipboard-types input[value=image]').click(); true"));
    QTRY_VERIFY(service->snapshot().value("data").toMap().value("types").toStringList().contains("image"));
    QTRY_VERIFY(!evaluate(page, QStringLiteral("document.querySelector('#clipboard-types').disabled")).toBool());
    evaluate(page, QStringLiteral("document.querySelector('#clipboard-types input[value=files]').click(); true"));
    QTRY_VERIFY(service->snapshot().value("data").toMap().value("types").toStringList().contains("files"));
    QMimeData text;
    text.setText(QStringLiteral("今天的灵感\n把重要的内容留在这里。<script>window.clipboardInjected = true</script>"));
    QVERIFY(service->capture(&text).value("ok").toBool());
    QImage image(200, 100, QImage::Format_RGB32);
    image.fill(QColor("#6385ee"));
    QMimeData picture;
    picture.setImageData(image);
    QVERIFY(service->capture(&picture).value("ok").toBool());
    QMimeData file;
    file.setUrls({QUrl::fromLocalFile(directory.filePath(QStringLiteral("项目资料/需求说明.pdf")))});
    QVERIFY(service->capture(&file).value("ok").toBool());
    QTRY_COMPARE(evaluate(page, QStringLiteral("document.querySelectorAll('.clipboard-card').length")).toInt(), 3);
    QVERIFY(!evaluate(page, QStringLiteral("Boolean(window.clipboardInjected)")).toBool());
    QVERIFY(evaluate(page, QStringLiteral("[...document.querySelectorAll('.clipboard-card time')].every(n => n.dateTime && n.textContent.includes(':'))")).toBool());
    QTRY_VERIFY(evaluate(page, QStringLiteral("document.querySelector('.clipboard-thumbnail').naturalWidth > 0")).toBool());
    const QString capture = qEnvironmentVariable("DESKTOPTOOL_CLIPBOARD_CAPTURE");
    if (!capture.isEmpty()) {
        // 等待离屏合成帧，避免 DOM 已更新但截图仍显示上一帧的空列表。
        QTest::qWait(400);
        QVERIFY(window.grab().save(capture));
        window.resize(860, 600);
        QTest::qWait(300);
        QVERIFY(evaluate(page, QStringLiteral("document.documentElement.scrollWidth <= window.innerWidth")).toBool());
        QVERIFY(window.grab().save(capture + ".narrow.png"));
        window.resize(1180, 780);
    }
    QFile script(QStringLiteral(":/tests/clipboard-smoke.js"));
    QVERIFY(script.open(QIODevice::ReadOnly));
    evaluate(page, QString::fromUtf8(script.readAll()));
    QTRY_VERIFY_WITH_TIMEOUT(!evaluate(page, QStringLiteral("window.__clipboardResult || null")).toMap().isEmpty(), 20000);
    const auto result = evaluate(page, QStringLiteral("window.__clipboardResult")).toMap();
    QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString()));
}

void WebSmokeTest::damagedClipboardHistoryIsIsolated()
{
    QTemporaryDir directory;
    QFile history(directory.filePath("clipboard-history.v1.json"));
    QVERIFY(history.open(QIODevice::WriteOnly));
    history.write("{broken");
    history.close();
    WebWindow window(directory.path(), false);
    window.show();
    auto* page = window.webView()->page();
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(page, QStringLiteral("document.documentElement.dataset.ready === 'true'")).toBool(), 20000);
    QVERIFY(evaluate(page, QStringLiteral("!document.querySelector('#clipboard-error').hidden && document.querySelector('#clipboard-types').disabled && !document.querySelector('#workspace').inert")).toBool());
    QVERIFY(history.open(QIODevice::ReadOnly));
    QCOMPARE(history.readAll(), QByteArray("{broken"));
}

void WebSmokeTest::emojiThroughPage()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QDir images(directory.filePath(QStringLiteral("stickers")));
    QVERIFY(QDir().mkpath(images.absolutePath()));
    QImage first(64, 48, QImage::Format_RGB32);
    first.fill(QColor("#f2b04e"));
    const QString firstPath = images.filePath(QStringLiteral("笑脸.png"));
    QVERIFY(first.save(firstPath, "PNG"));
    QImage second(80, 80, QImage::Format_RGB32);
    second.fill(QColor("#6385ee"));
    const QString secondPath = images.filePath(QStringLiteral("蓝星.png"));
    QVERIFY(second.save(secondPath, "PNG"));
    WebWindow window(directory.path(), false);
    window.show();
    auto* page = window.webView()->page();
    auto* clipboardService = window.findChild<ClipboardService*>();
    QVERIFY(clipboardService);
    auto* emojiService = window.findChild<EmojiService*>();
    QVERIFY(emojiService);
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(page, QStringLiteral("document.documentElement && document.documentElement.dataset.ready === 'true'")).toBool(), 20000);
    // 通过真实桥接服务添加表情，页面依靠变更信号刷新。
    QVERIFY(emojiService->add({{QStringLiteral("path"), firstPath}, {QStringLiteral("categoryId"), QStringLiteral("default")},
        {QStringLiteral("keywords"), QVariantList{QStringLiteral("开心"), QStringLiteral("笑脸")}}}).value("ok").toBool());
    QVERIFY(emojiService->add({{QStringLiteral("path"), secondPath}, {QStringLiteral("categoryId"), QStringLiteral("default")},
        {QStringLiteral("keywords"), QVariantList{QStringLiteral("蓝星")}}}).value("ok").toBool());
    QString firstId;
    QString secondId;
    for (const auto& value : emojiService->snapshot().value("data").toMap().value("items").toList()) {
        const auto item = value.toMap();
        if (item.value("name").toString() == QStringLiteral("笑脸.png"))
            firstId = item.value("id").toString();
        if (item.value("name").toString() == QStringLiteral("蓝星.png"))
            secondId = item.value("id").toString();
    }
    QVERIFY(!firstId.isEmpty() && !secondId.isEmpty());
    QVERIFY(clipboardService->setTypes({"files", "image"}).value("ok").toBool());
    const QJsonObject fixture{{QStringLiteral("firstId"), firstId}, {QStringLiteral("secondId"), secondId}};
    evaluate(page, QStringLiteral("window.__emojiFixture = ")
        + QString::fromUtf8(QJsonDocument(fixture).toJson(QJsonDocument::Compact)) + QStringLiteral("; true"));
    QFile script(QStringLiteral(":/tests/emoji-smoke.js"));
    QVERIFY(script.open(QIODevice::ReadOnly));
    evaluate(page, QString::fromUtf8(script.readAll()));
    QTRY_VERIFY_WITH_TIMEOUT(!evaluate(page, QStringLiteral("window.__emojiResult || null")).toMap().isEmpty(), 20000);
    const auto result = evaluate(page, QStringLiteral("window.__emojiResult")).toMap();
    QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString()));
    // 复制文件同时提供文件引用与图像内容，但没有记录进剪贴板历史。
    QTest::qWait(120);
    const auto* mime = QGuiApplication::clipboard()->mimeData();
    QVERIFY(mime && mime->hasUrls() && mime->hasImage());
    QCOMPARE(mime->urls().first(), QUrl::fromLocalFile(firstPath));
    QVERIFY(clipboardService->snapshot().value("data").toMap().value("items").toList().isEmpty());
    // 删除表情只移除记录，原文件仍然保留。
    QVERIFY(QFileInfo::exists(firstPath) && QFileInfo::exists(secondPath));
    const QString capturePath = qEnvironmentVariable("DESKTOPTOOL_EMOJI_CAPTURE");
    if (!capturePath.isEmpty()) {
        window.resize(1180, 860);
        QTest::qWait(400);
        QVERIFY(window.grab().save(capturePath));
        window.resize(1180, 780);
    }
}

void WebSmokeTest::damagedEmojiLibraryIsIsolated()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString library = directory.filePath(QStringLiteral("emoji/emoji-library.v1.json"));
    QVERIFY(QDir().mkpath(QFileInfo(library).absolutePath()));
    QFile libraryFile(library);
    QVERIFY(libraryFile.open(QIODevice::WriteOnly));
    libraryFile.write("{broken");
    libraryFile.close();
    WebWindow window(directory.path(), false);
    window.show();
    auto* page = window.webView()->page();
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(page, QStringLiteral("document.documentElement && document.documentElement.dataset.ready === 'true'")).toBool(), 20000);
    QVERIFY(evaluate(page, QStringLiteral("!document.querySelector('#emoji-error').hidden && document.querySelector('#emoji-error').textContent.length > 0 && !document.querySelector('#workspace').inert")).toBool());
    QVERIFY(libraryFile.open(QIODevice::ReadOnly));
    QCOMPARE(libraryFile.readAll(), QByteArray("{broken"));
}

void WebSmokeTest::wallpaperThroughPage()
{
    QTemporaryDir directory;
    WebWindow window(directory.path(), false);
    window.resize(1400, 1000);
    window.show();
    auto* page = window.webView()->page();
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(page, QStringLiteral("document.documentElement.dataset.ready === 'true'")).toBool(), 20000);
    auto* bridge = window.findChild<AppBridge*>();
    QVERIFY(bridge);
    const auto screens = bridge->getWallpaper().value("data").toMap().value("screens").toList();
    QVERIFY(!screens.isEmpty());
    QImage image(640, 640, QImage::Format_RGB32);
    image.fill(QColor("#3d78c8"));
    // 方形色块壁纸让宽屏适应、填充和平铺的差别可见。
    {
        QPainter painter(&image);
        painter.fillRect(0, 0, 640, 150, QColor("#f7d36d"));
        painter.fillRect(100, 220, 440, 200, QColor("#bce5ed"));
        painter.fillRect(0, 490, 640, 150, QColor("#153b70"));
    }
    const QString firstPath = directory.filePath(QStringLiteral("主屏海洋.png"));
    QVERIFY(image.save(firstPath));
    image.fill(QColor("#bc7d49"));
    const QString secondPath = directory.filePath(QStringLiteral("副屏夕阳.png"));
    QVERIFY(image.save(secondPath));
    const QString firstScreen = screens.first().toMap().value("id").toString();
    const QString lastScreen = screens.last().toMap().value("id").toString();
    const auto first = bridge->addWallpaper({{"path", firstPath}, {"screenId", firstScreen}});
    QVERIFY(first.value("ok").toBool());
    const auto second = bridge->addWallpaper({{"path", secondPath}, {"screenId", lastScreen}});
    QVERIFY(second.value("ok").toBool());
    evaluate(page, QStringLiteral("document.querySelector('[data-page=wallpaper]').click()"));
    QTRY_COMPARE_WITH_TIMEOUT(evaluate(page, QStringLiteral("document.querySelectorAll('.wallpaper-monitor').length")).toInt(), screens.size(), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(evaluate(page, QStringLiteral("document.querySelectorAll('.wallpaper-card').length")).toInt(), 2, 10000);
    evaluate(page, QStringLiteral("document.querySelectorAll('.wallpaper-monitor')[0].click()"));
    QVERIFY(evaluate(page, QStringLiteral("document.querySelectorAll('.wallpaper-monitor')[0].getAttribute('aria-pressed') === 'true'")).toBool());
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(page, QStringLiteral("document.querySelector('.wallpaper-monitor-preview').style.backgroundImage.startsWith('url(\"data:image/png;base64,')")).toBool(), 10000);
    evaluate(page, QStringLiteral("document.querySelector('input[name=wallpaper-mode][value=fit]').click()"));
    QTRY_COMPARE_WITH_TIMEOUT(bridge->getWallpaper().value("data").toMap().value("screens").toList().first().toMap().value("displayMode").toString(), QStringLiteral("fit"), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(page, QStringLiteral("!document.querySelector('#wallpaper-display-modes').disabled && document.querySelector('.wallpaper-monitor-preview').style.backgroundSize === 'contain'")).toBool(), 10000);
    evaluate(page, QStringLiteral("document.querySelector('input[name=wallpaper-mode][value=tile]').click()"));
    QTRY_COMPARE_WITH_TIMEOUT(bridge->getWallpaper().value("data").toMap().value("screens").toList().first().toMap().value("displayMode").toString(), QStringLiteral("tile"), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(page, QStringLiteral("!document.querySelector('#wallpaper-display-modes').disabled && document.querySelector('.wallpaper-monitor-preview').style.backgroundRepeat === 'repeat' && document.querySelectorAll('input[name=wallpaper-mode]:checked').length === 1")).toBool(), 10000);
    if (screens.size() > 1)
        QCOMPARE(bridge->getWallpaper().value("data").toMap().value("screens").toList().last().toMap().value("displayMode").toString(), QStringLiteral("fill"));
    // 最近添加的第二条历史应用到第一屏，验证界面传递了正确的目标标识。
    evaluate(page, QStringLiteral("document.querySelector('.wallpaper-card .emoji-card-actions button').click()"));
    const QString secondId = second.value("data").toMap().value("id").toString();
    QTRY_COMPARE_WITH_TIMEOUT(bridge->getWallpaper().value("data").toMap().value("screens").toList().first().toMap().value("activeId").toString(), secondId, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(page, QStringLiteral("!document.querySelector('#clear-screen-wallpaper').disabled")).toBool(), 10000);
    evaluate(page, QStringLiteral("document.querySelector('#clear-screen-wallpaper').click()"));
    QTRY_VERIFY_WITH_TIMEOUT(bridge->getWallpaper().value("data").toMap().value("screens").toList().first().toMap().value("activeId").toString().isEmpty(), 10000);
    if (screens.size() > 1)
        QCOMPARE(bridge->getWallpaper().value("data").toMap().value("screens").toList().last().toMap().value("activeId").toString(), secondId);
    // 布局检查来自测试窗口自身渲染，不读取或控制用户桌面。
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(page, QStringLiteral("document.querySelector('.wallpaper-monitor').getBoundingClientRect().width > 20")).toBool(), 10000);
    const QString screenshot = qEnvironmentVariable("DESKTOPTOOL_WALLPAPER_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        QVERIFY(bridge->useWallpaperOnScreen(first.value("data").toMap().value("id").toString(), firstScreen).value("ok").toBool());
        QVERIFY(bridge->setWallpaperDisplayMode(firstScreen, "fit").value("ok").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(evaluate(page, QStringLiteral("document.querySelector('input[name=wallpaper-mode][value=fit]').checked && document.querySelector('.wallpaper-monitor-preview').style.backgroundSize === 'contain'")).toBool(), 10000);
        // 等待异步页面刷新和绘制完成，避免捕获上一帧的空列表。
        QTRY_COMPARE_WITH_TIMEOUT(evaluate(page, QStringLiteral("document.querySelector('#wallpaper-count').textContent")).toString(), QStringLiteral("2"), 10000);
        QTest::qWait(200);
        QVERIFY(window.grab().save(screenshot));
    }
}

// 初始化测试用 Qt 应用；不会启用托盘、系统热键或单实例服务。
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("DesktopToolWebTest"));
    app.setApplicationVersion(QStringLiteral("2.2.1-test"));
    WebSmokeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "WebSmokeTest.moc"
