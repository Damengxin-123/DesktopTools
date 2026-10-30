#include "app/TerrainEditWindow.h"
#include "app/WebWindow.h"
#include "bridge/AppBridge.h"
#include "services/TerrainService.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QWidgetList>
#include <QPixmap>
#include <QScreen>
#include <QVBoxLayout>
#include <QWebChannel>
#include <QWebEngineProfile>
#include <QJsonObject>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <iostream>

// 真实桌面环境的地形编辑冒烟程序：验证 Qt WebEngine 中 WebGL2 渲染、
// 程序化笔划、撤销/重做、保存与 16 位 PNG 导出，并抓取窗口截图。
// 结果写入 out/terrain-smoke-result.json，退出码 0 表示全部通过。
namespace {
// 捕获页面控制台输出的探针页面（javaScriptConsoleMessage 是保护虚方法）。
class ProbePage final : public QWebEnginePage
{
public:
    using QWebEnginePage::QWebEnginePage;
    QStringList messages;
protected:
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level, const QString& message,
                                  int lineNumber, const QString& sourceID) override
    {
        if (level >= WarningMessageLevel)
            messages.append(QStringLiteral("%1:%2 %3")
                .arg(sourceID, QString::number(lineNumber), message));
    }
};

QVariant evaluate(QWebEnginePage* page, const QString& source, int timeoutMs = 20000)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QVariant result;
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    page->runJavaScript(source, [&loop, &result](const QVariant& value) {
        result = value;
        loop.quit();
    });
    timeout.start(timeoutMs);
    loop.exec();
    return result;
}

bool waitFor(QWebEnginePage* page, const QString& condition, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        const QVariant value = evaluate(page, condition, 3000);
        if (value.isValid() && value.toBool())
            return true;
        QApplication::processEvents(QEventLoop::AllEvents, 80);
    }
    return false;
}
}

int main(int argc, char* argv[])
{
    // 冒烟进程常不在交互前台，禁用 Windows 原生遮挡计算，
    // 避免 Chromium 把可见窗口误判为遮挡而暂停 rAF 与合成。
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS",
            QByteArrayLiteral("--disable-features=CalculateNativeWinOcclusion"
                               " --disable-backgrounding-occluded-windows"
                               " --disable-renderer-backgrounding"
                               " --disable-background-timer-throttling"));
    QApplication app(argc, argv);
    QTemporaryDir data;
    if (!data.isValid())
        return 2;

    TerrainService service(data.path());
    const auto created = service.create(QStringLiteral("桌面冒烟地形"), 256, 256, 2, 0);
    if (!created.value("ok").toBool())
        return 2;
    const QString projectId = created.value("data").toMap().value("id").toString();

    auto* window = TerrainEditWindow::open(&service, projectId, nullptr);
    if (!window)
        return 2;
    // 置顶显示：冒烟进程不在前台时，遮挡会导致 Chromium 暂停 rAF 与合成。
    window->setWindowFlag(Qt::WindowStaysOnTopHint, true);
    window->show();
    window->raise();
    window->activateWindow();
    auto* page = window->findChild<QWebEnginePage*>();
    if (!page)
        return 2;

    QJsonObject result;
    const auto ready = waitFor(page, QStringLiteral("window.terrainReady === true"), 60000);
    result.insert(QStringLiteral("pageReady"), ready);
    if (!ready) {
        result.insert(QStringLiteral("bootError"), evaluate(page,
            QStringLiteral("window.terrainBootError || ''")).toString());
    }

    if (ready) {
        // WebGL2 是否真实可用（这是离屏测试无法覆盖的部分）。
        const QJsonObject stats = QJsonDocument::fromJson(evaluate(page,
            QStringLiteral("JSON.stringify(window.terrainEditor.getStats())")).toString().toUtf8())
            .object();
        result.insert(QStringLiteral("webgl"), stats.value("webgl").toBool());
        result.insert(QStringLiteral("grid"), QString("%1x%2")
            .arg(stats.value("gridWidth").toInt()).arg(stats.value("gridDepth").toInt()));
        result.insert(QStringLiteral("canvasPixels"), QString("%1x%2")
            .arg(stats.value("canvasWidth").toInt()).arg(stats.value("canvasHeight").toInt()));
        result.insert(QStringLiteral("canvasClient"), QString("%1x%2")
            .arg(stats.value("canvasClientWidth").toInt())
            .arg(stats.value("canvasClientHeight").toInt()));
        result.insert(QStringLiteral("visibilityState"), evaluate(page,
            QStringLiteral("document.visibilityState")).toString());
        // rAF 活性：500 毫秒内应触发多帧。
        result.insert(QStringLiteral("rafFrames"), evaluate(page, QStringLiteral(R"js(
            (function () {
                return new Promise(function (resolve) {
                    let frames = 0;
                    const tick = function () { frames += 1; if (frames < 30) requestAnimationFrame(tick); };
                    requestAnimationFrame(tick);
                    setTimeout(function () { resolve(frames); }, 500);
                });
            })()
        )js"), 3000).toInt());

        // 一次程序化笔划 + 抓图（含三维画面）。
        evaluate(page, QStringLiteral(
            "(function () {"
            "  const points = [];"
            "  for (let index = 0; index <= 15; ++index)"
            "    points.push({ x: 60 + index * 8, z: 128 });"
            "  return window.terrainEditor.applyStroke('mountain', points, 40);"
            "})()"), 30000);
        QApplication::processEvents(QEventLoop::AllEvents, 600);
        // 尺寸变化强制新的合成帧，随后等待 rAF 重新调度。
        window->resize(window->size() + QSize(24, 24));
        for (int i = 0; i < 20; ++i)
            QApplication::processEvents(QEventLoop::AllEvents, 50);
        result.insert(QStringLiteral("rafFrames"), evaluate(page, QStringLiteral(R"js(
            (function () {
                return new Promise(function (resolve) {
                    let frames = 0;
                    const tick = function () { frames += 1; if (frames < 30) requestAnimationFrame(tick); };
                    requestAnimationFrame(tick);
                    setTimeout(function () { resolve(frames); }, 500);
                });
            })()
        )js"), 3000).toInt());
        // 用屏幕抓取替代控件抓取：WebEngine 的合成帧可能滞后于控件抓取。
        if (QGuiApplication::primaryScreen()) {
            const QPixmap shot = QGuiApplication::primaryScreen()->grabWindow(window->winId());
            const QString shotPath = QDir(QCoreApplication::applicationDirPath())
                .filePath(QStringLiteral("../../../../out/terrain-smoke-window.png"));
            shot.save(QDir::cleanPath(shotPath));
            result.insert(QStringLiteral("screenshot"), QDir::cleanPath(shotPath));
        }
        // 从 WebGL 画布同步导出当前帧，直接验证三维地形确实被绘制。
        const QString canvasData = evaluate(page,
            QStringLiteral("window.terrainEditor.captureView()")).toString();
        if (canvasData.startsWith(QStringLiteral("data:image/png;base64,"))) {
            const QByteArray png = QByteArray::fromBase64(
                canvasData.mid(QString(QStringLiteral("data:image/png;base64,")).size()).toLatin1());
            const QString canvasPath = QDir(QCoreApplication::applicationDirPath())
                .filePath(QStringLiteral("../../../../out/terrain-smoke-canvas.png"));
            QFile canvasFile(QDir::cleanPath(canvasPath));
            if (canvasFile.open(QIODevice::WriteOnly)) {
                canvasFile.write(png);
                result.insert(QStringLiteral("canvas"), QDir::cleanPath(canvasPath));
                result.insert(QStringLiteral("canvasBytes"), png.size());
            }
        }

        // 真实指针事件链路：合成 pointerdown/move/up 直接派发到画布，
        // 验证 事件 → 拾取 → 落笔 → 数据变化 → 渲染循环上屏 全部贯通。
        const qint64 pointerBefore = evaluate(page,
            QStringLiteral("window.terrainEditor.checksum()")).toLongLong();
        const qint64 framesBefore = evaluate(page,
            QStringLiteral("window.terrainEditor.frameCount()")).toLongLong();
        evaluate(page, QStringLiteral(R"js(
            (function () {
                const canvas = document.getElementById('terrain-canvas');
                const rect = canvas.getBoundingClientRect();
                const startX = rect.left + rect.width / 2;
                const startY = rect.top + rect.height / 2;
                const base = { bubbles: true, cancelable: true, pointerId: 7,
                    isPrimary: true, button: 0, pointerType: 'mouse' };
                canvas.dispatchEvent(new PointerEvent('pointerdown',
                    Object.assign({}, base, { clientX: startX, clientY: startY, buttons: 1 })));
                for (let step = 1; step <= 6; ++step) {
                    canvas.dispatchEvent(new PointerEvent('pointermove',
                        Object.assign({}, base, { clientX: startX + step * 8,
                            clientY: startY, buttons: 1 })));
                }
                canvas.dispatchEvent(new PointerEvent('pointerup',
                    Object.assign({}, base, { clientX: startX + 48, clientY: startY, buttons: 0 })));
                return true;
            })()
        )js"), 10000);
        // 渲染循环应已响应交互（画笔圈 + 地形变化至少各提交若干帧）。
        const bool framesAdvanced = waitFor(page,
            QStringLiteral("window.terrainEditor.frameCount() > %1").arg(framesBefore), 10000);
        result.insert(QStringLiteral("pointerFramesAdvanced"), framesAdvanced);
        const qint64 pointerAfter = evaluate(page,
            QStringLiteral("window.terrainEditor.checksum()")).toLongLong();
        result.insert(QStringLiteral("pointerStrokeChanged"), pointerAfter != pointerBefore);
        // 抓取指针笔划后的画布帧，人工核对画笔圈与地形隆起是否可见。
        const QString pointerCanvas = evaluate(page,
            QStringLiteral("window.terrainEditor.captureView()")).toString();
        if (pointerCanvas.startsWith(QStringLiteral("data:image/png;base64,"))) {
            const QByteArray png = QByteArray::fromBase64(
                pointerCanvas.mid(QString(QStringLiteral("data:image/png;base64,")).size()).toLatin1());
            QFile canvasFile(QDir::cleanPath(QDir(QCoreApplication::applicationDirPath())
                .filePath(QStringLiteral("../../../../out/terrain-smoke-pointer.png"))));
            if (canvasFile.open(QIODevice::WriteOnly)) canvasFile.write(png);
        }
        // 先撤销指针笔划，恢复到程序化笔划后的状态再继续。
        evaluate(page, QStringLiteral("window.terrainEditor.undo(), true"));
        QApplication::processEvents(QEventLoop::AllEvents, 200);

        // 海平面显示选项：勾选后切换画笔仍持续显示，取消勾选后恢复默认。
        {
            const QString seaResult = evaluate(page, QStringLiteral(R"js(
                (function () {
                    const results = {};
                    document.getElementById('tool-coast').click();
                    results.coastShown = window.terrainEditor.seaPlaneVisible();
                    const checkbox = document.getElementById('coast-show-sea-level');
                    checkbox.checked = true;
                    checkbox.dispatchEvent(new Event('change'));
                    document.getElementById('tool-detail').click();
                    results.pinnedShown = window.terrainEditor.seaPlaneVisible();
                    document.getElementById('tool-coast').click();
                    checkbox.checked = false;
                    checkbox.dispatchEvent(new Event('change'));
                    document.getElementById('tool-detail').click();
                    results.unpinnedHidden = !window.terrainEditor.seaPlaneVisible();
                    return JSON.stringify(results);
                })()
            )js")).toString();
            const QJsonObject sea = QJsonDocument::fromJson(seaResult.toUtf8()).object();
            result.insert(QStringLiteral("seaPlaneCoastShown"), sea.value(QStringLiteral("coastShown")).toBool());
            result.insert(QStringLiteral("seaPlanePinnedShown"), sea.value(QStringLiteral("pinnedShown")).toBool());
            result.insert(QStringLiteral("seaPlaneUnpinnedHidden"), sea.value(QStringLiteral("unpinnedHidden")).toBool());
        }

        // 撤销 → 重做 → 保存 → 导出。
        const qint64 after = evaluate(page,
            QStringLiteral("window.terrainEditor.checksum()")).toLongLong();
        evaluate(page, QStringLiteral("window.terrainEditor.undo(), true"));
        const qint64 undone = evaluate(page,
            QStringLiteral("window.terrainEditor.checksum()")).toLongLong();
        evaluate(page, QStringLiteral("window.terrainEditor.redo(), true"));
        const qint64 redone = evaluate(page,
            QStringLiteral("window.terrainEditor.checksum()")).toLongLong();
        result.insert(QStringLiteral("undoRestore"), undone != after);
        result.insert(QStringLiteral("redoRestore"), redone == after);

        evaluate(page, QStringLiteral(
            "window.terrainEditor.save().then(() => { window._ok = true; })"
            ".catch((error) => { window._err = String(error); }), true"));
        result.insert(QStringLiteral("saved"), waitFor(page,
            QStringLiteral("window._ok === true || typeof window._err === 'string'"), 30000)
            && evaluate(page, QStringLiteral("window._err === undefined")).toBool());

        const QString pngPath = QString(data.path() + QStringLiteral("/桌面冒烟导出.png"))
            .replace(QChar(92), QChar(47));
        evaluate(page, QString("window.terrainEditor.exportHeightmap('%1', 4), true").arg(pngPath));
        const bool exported = waitFor(page, QStringLiteral(
            "window.document.getElementById('status-message').textContent.indexOf('已导出') >= 0"),
            30000);
        result.insert(QStringLiteral("exported"), exported);
        result.insert(QStringLiteral("exportPngExists"), QFile::exists(pngPath));
        result.insert(QStringLiteral("exportPath"), pngPath);
    }

    window->close();
    QApplication::processEvents(QEventLoop::AllEvents, 400);

    // 主页面探针：加载真实 index.html + AppBridge，捕获页面就绪状态与 JS 错误。
    {
        QTemporaryDir mainData;
        // 自建浏览器环境：与 WebWindow 相同的离线配置，但可捕获页面控制台输出。
        QWidget mainHost;
        QVBoxLayout* mainLayout = new QVBoxLayout(&mainHost);
        mainLayout->setContentsMargins(0, 0, 0, 0);
        QWebEngineView* mainView = new QWebEngineView(&mainHost);
        mainLayout->addWidget(mainView);
        QWebEngineProfile* mainProfile = new QWebEngineProfile(&mainHost);
        ProbePage* mainProbe = new ProbePage(mainProfile, mainView);
        mainView->setPage(mainProbe);
        AppBridge* mainBridge = new AppBridge(mainData.path(), &mainHost, false);
        QWebChannel* mainChannel = new QWebChannel(mainProbe);
        mainChannel->registerObject(QStringLiteral("backend"), mainBridge);
        mainProbe->setWebChannel(mainChannel);
        mainHost.resize(1180, 780);
        mainHost.show();
        mainView->load(QUrl(QStringLiteral("qrc:/web/index.html")));
        auto* mainWebPage = static_cast<QWebEnginePage*>(mainProbe);
        const bool mainReady = waitFor(mainWebPage,
            QStringLiteral("document.documentElement && document.documentElement.dataset.ready === 'true'"),
            30000);
        std::cout << "MAIN-PAGE READY: " << (mainReady ? "true" : "false") << std::endl;
        {
            // 无论成功与否，都输出捕获到的警告与错误，便于定位页面初始化问题。
            for (const QString& message : mainProbe->messages) {
                const QByteArray utf8 = message.toUtf8();
                std::cout << "CONSOLE: " << utf8.constData() << std::endl;
            }
        }
        if (mainReady) {
            // 主页面 → 新建项目 → 列表渲染 → 打开独立编辑窗口：完整用户路径。
            // 发起创建，通过窗口变量与列表 DOM 轮询结果（避免依赖 Promise 回传）。
            evaluate(mainWebPage, QStringLiteral(R"js(
                (function () {
                    window.desktopBridge.call("createTerrain", "冒烟链路项目", 128, 128, 2, 0)
                        .then(function (item) {
                            window._mainCreated = JSON.stringify({ ok: true, id: item.id,
                                adjusted: item.adjusted, size: item.sizeWidth });
                        })
                        .catch(function (error) {
                            window._mainCreated = JSON.stringify({ ok: false,
                                error: String(error && error.message || error) });
                        });
                    return true;
                })()
            )js"), 5000);
            const bool createdDone = waitFor(mainWebPage,
                QStringLiteral("typeof window._mainCreated === 'string'"), 20000);
            const QVariant created = createdDone ? evaluate(mainWebPage,
                QStringLiteral("window._mainCreated")) : QVariant();
            QJsonObject createdObject = QJsonDocument::fromJson(
                created.toString().toUtf8()).object();
            if (createdObject.value(QStringLiteral("ok")).toBool()) {
                createdObject.insert(QStringLiteral("rows"), waitFor(mainWebPage,
                    QStringLiteral("document.querySelectorAll('#terrain-list .terrain-row').length === 1"),
                    10000) ? 1 : 0);
            }
            result.insert(QStringLiteral("mainCreate"), createdObject);
            if (createdObject.value(QStringLiteral("ok")).toBool()) {
                const QString projectId = createdObject.value(QStringLiteral("id")).toString();
                evaluate(mainWebPage, QString(
                    "window.desktopBridge.call('openTerrainEditor', '%1'), true").arg(projectId));
                // 等待编辑窗口出现并完成数据加载。
                QPointer<TerrainEditWindow> editor;
                QElapsedTimer waitTimer;
                waitTimer.start();
                while (!editor && waitTimer.elapsed() < 15000) {
                    QApplication::processEvents(QEventLoop::AllEvents, 100);
                    const QWidgetList topLevels = QApplication::topLevelWidgets();
                    for (QWidget* widget : topLevels) {
                        if (auto* terrainWindow = qobject_cast<TerrainEditWindow*>(widget))
                            editor = terrainWindow;
                    }
                }
                result.insert(QStringLiteral("editorOpened"), !editor.isNull());
                if (editor) {
                    auto* editorPage = editor->findChild<QWebEnginePage*>();
                    const bool editorReady = editorPage && waitFor(editorPage,
                        QStringLiteral("window.terrainReady === true"), 30000);
                    result.insert(QStringLiteral("editorReady"), editorReady);
                    if (editorPage && editorReady) {
                        result.insert(QStringLiteral("editorGrid"), evaluate(editorPage,
                            QStringLiteral("window.terrainEditor.getStats().gridWidth")).toInt());
                    }
                    editor->close();
                    QApplication::processEvents(QEventLoop::AllEvents, 300);
                }
            }
        }

        if (!mainReady) {
            // 页面失败时连接错误区域包含可读原因。
            const QString diagnostics = evaluate(mainWebPage, QStringLiteral(R"js(
                (function () {
                    const errorBox = document.getElementById('connection-error');
                    return JSON.stringify({
                        hasTerrainApi: typeof window.desktopTerrain,
                        errorHidden: errorBox ? errorBox.hidden : null,
                        errorText: errorBox ? errorBox.textContent.trim() : '',
                        bodyText: document.body ? document.body.innerText.slice(0, 200) : ''
                    });
                })()
            )js")).toString();
            std::cout << "MAIN-PAGE DIAG: " << diagnostics.toUtf8().constData() << std::endl;
        }
    }

    const bool pass = ready && result.value("webgl").toBool()
        && result.value("undoRestore").toBool() && result.value("redoRestore").toBool()
        && result.value("saved").toBool() && result.value("exported").toBool()
        && result.value("exportPngExists").toBool()
        // 主页面路径：项目创建、列表渲染与编辑窗口打开。
        && result.value("mainCreate").toObject().value("ok").toBool()
        && result.value("editorOpened").toBool()
        && result.value("editorReady").toBool()
        && result.value("pointerStrokeChanged").toBool()
        && result.value("pointerFramesAdvanced").toBool()
        && result.value("seaPlaneCoastShown").toBool()
        && result.value("seaPlanePinnedShown").toBool()
        && result.value("seaPlaneUnpinnedHidden").toBool();
    const QString resultPath = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("../../../../out/terrain-smoke-result.json"));
    QFile resultFile(QDir::cleanPath(resultPath));
    if (resultFile.open(QIODevice::WriteOnly))
        resultFile.write(QJsonDocument(result).toJson(QJsonDocument::Indented));
    std::cout << (pass ? "TERRAIN-SMOKE PASS" : "TERRAIN-SMOKE FAIL") << std::endl;
    return pass ? 0 : 1;
}
