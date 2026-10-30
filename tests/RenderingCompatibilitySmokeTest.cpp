#include "app/RenderingPolicy.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QVariantMap>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

// 在真实 Windows 图形会话中验证默认软件设备、WebGL2 绘制、双向心跳和正常拆卸。
int main(int argc, char* argv[])
{
    // 独立看门狗覆盖应用构造、主线程死锁与析构，避免 Qt 定时器也被阻塞后永久挂起。
    std::mutex completionMutex;
    std::condition_variable completionChanged;
    bool completed = false;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lock(completionMutex);
        if (!completionChanged.wait_for(lock, std::chrono::seconds(40), [&] { return completed; })) {
            std::cerr << "FAIL: rendering process exceeded 40 seconds (including shutdown)" << std::endl;
            std::_Exit(124);
        }
    });

    RenderingPolicy::configure();
    int result = 1;
    {
        QApplication app(argc, argv);
        app.setQuitOnLastWindowClosed(false);
        app.setApplicationName(QStringLiteral("DesktopToolRenderingCompatibilitySmokeTest"));
        // 无存储名称的独立配置为无痕模式，不访问应用的用户配置、缓存和历史记录。
        QWebEngineProfile profile;
        profile.setHttpCacheType(QWebEngineProfile::MemoryHttpCache);
        profile.setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
        QPointer<QWebEngineView> view = new QWebEngineView;
        view->setAttribute(Qt::WA_DeleteOnClose);
        view->setAttribute(Qt::WA_ShowWithoutActivating);
        view->setWindowTitle(QStringLiteral("软件渲染兼容性验证"));
        view->resize(420, 220);
        auto* page = new QWebEnginePage(&profile, view);
        view->setPage(page);

        QTimer heartbeat;
        QTimer poll;
        QTimer deadline;
        deadline.setSingleShot(true);
        QElapsedTimer elapsed;
        elapsed.start();
        int mainTicks = 0;
        int responses = 0;
        int pageTicks = 0;
        int previousPageTicks = -1;
        int progressingResponses = 0;
        bool pending = false;
        bool finishing = false;
        QString renderer;
        QVariantList pixel;
        // 所有退出路径先关闭并销毁网页窗口，再结束事件循环，使渲染器正常拆卸。
        const auto finish = [&](int code, const QString& message) {
            if (finishing)
                return;
            finishing = true;
            result = code;
            heartbeat.stop();
            poll.stop();
            deadline.stop();
            const QJsonObject report {
                {QStringLiteral("passed"), code == 0},
                {QStringLiteral("message"), message},
                {QStringLiteral("renderer"), renderer},
                {QStringLiteral("pixel"), QJsonValue::fromVariant(pixel)},
                {QStringLiteral("mainTicks"), mainTicks},
                {QStringLiteral("pageTicks"), pageTicks},
                {QStringLiteral("responses"), responses},
                {QStringLiteral("elapsedSeconds"), elapsed.elapsed() / 1000.0},
                {QStringLiteral("softwarePreference"), qEnvironmentVariable("QSG_RHI_PREFER_SOFTWARE_RENDERER")},
                {QStringLiteral("chromiumFlags"), qEnvironmentVariable("QTWEBENGINE_CHROMIUM_FLAGS")}
            };
            qInfo().noquote() << QJsonDocument(report).toJson(QJsonDocument::Compact);
            if (view)
                view->close();
        };
        QObject::connect(view, &QObject::destroyed, &app, [&] { app.exit(result); });
        QObject::connect(&heartbeat, &QTimer::timeout, &app, [&] { ++mainTicks; });
        QObject::connect(&deadline, &QTimer::timeout, &app, [&] {
            finish(2, QStringLiteral("网页或心跳验证超过 25 秒"));
        });
        QObject::connect(page, &QWebEnginePage::renderProcessTerminated, &app,
                         [&](QWebEnginePage::RenderProcessTerminationStatus, int exitCode) {
            finish(3, QStringLiteral("网页渲染进程异常退出：%1").arg(exitCode));
        });
        QObject::connect(&poll, &QTimer::timeout, &app, [&] {
            if (pending || finishing)
                return;
            pending = true;
            // 将主线程心跳传入页面，再读回页面心跳和绘制结果，验证双向交互持续可用。
            page->runJavaScript(QStringLiteral("window.probe(%1)").arg(mainTicks), [&](const QVariant& value) {
                pending = false;
                if (finishing)
                    return;
                ++responses;
                const QVariantMap data = value.toMap();
                renderer = data.value(QStringLiteral("renderer")).toString();
                pixel = data.value(QStringLiteral("pixel")).toList();
                if (!data.value(QStringLiteral("ok")).toBool()) {
                    finish(4, QStringLiteral("WebGL2 验证失败：%1").arg(data.value(QStringLiteral("error")).toString()));
                    return;
                }
                if (!renderer.contains(QStringLiteral("Microsoft Basic Render Driver"), Qt::CaseInsensitive)
                    && !renderer.contains(QStringLiteral("WARP"), Qt::CaseInsensitive)) {
                    finish(5, QStringLiteral("实际渲染器不是 Microsoft WARP 软件设备"));
                    return;
                }
                if (pixel != QVariantList {51, 102, 204, 255}) {
                    finish(6, QStringLiteral("WebGL2 像素读回值不正确"));
                    return;
                }
                pageTicks = data.value(QStringLiteral("pageTicks")).toInt();
                if (pageTicks > previousPageTicks
                    && data.value(QStringLiteral("echo")).toInt() > 0)
                    ++progressingResponses;
                previousPageTicks = pageTicks;
                if (elapsed.elapsed() >= 3000 && mainTicks >= 20 && progressingResponses >= 10)
                    finish(0, QStringLiteral("WARP WebGL2、像素读回和双向心跳通过"));
            });
        });
        QObject::connect(page, &QWebEnginePage::loadFinished, &app, [&](bool ok) {
            if (finishing)
                return;
            if (!ok)
                finish(7, QStringLiteral("内存测试页面加载失败"));
            else
                poll.start(200);
        });
        // 页面禁止外部资源，仅使用内存 HTML、内联脚本和本地 WebGL2 绘制。
        const QString html = QString::fromUtf8(R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta http-equiv="Content-Security-Policy"
content="default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'">
<title>软件渲染兼容性验证</title></head><body>
<p>正在验证 WARP WebGL2、像素读回和双向心跳……</p><canvas id="canvas" width="16" height="16"></canvas>
<script>
// 页面计数与接收到的主线程计数用于验证事件循环和双向通信持续推进。
let pageTicks = 0;
setInterval(() => { ++pageTicks; }, 80);
// 每次请求都重新提交绘制并读回像素，避免仅验证初始化后的静态结果。
window.probe = function(mainTicks) {
    try {
        const gl = document.getElementById('canvas').getContext('webgl2', {preserveDrawingBuffer: true});
        if (!gl) throw new Error('无法创建 WebGL2 上下文');
        const debug = gl.getExtension('WEBGL_debug_renderer_info');
        if (!debug) throw new Error('无法查询实际图形设备');
        const renderer = gl.getParameter(debug.UNMASKED_RENDERER_WEBGL);
        gl.clearColor(0.2, 0.4, 0.8, 1.0);
        gl.clear(gl.COLOR_BUFFER_BIT);
        const pixel = new Uint8Array(4);
        gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, pixel);
        const error = gl.getError();
        if (error !== gl.NO_ERROR) throw new Error('WebGL 错误 ' + error);
        // 成功状态、设备名、像素、页面心跳、主线程回显构成每次验证报告。
        return {ok: true, renderer, pixel: Array.from(pixel), pageTicks, echo: mainTicks};
    } catch (error) {
        // 失败状态和具体错误供 C++ 输出明确诊断。
        return {ok: false, error: String(error), pageTicks, echo: mainTicks};
    }
};
</script></body></html>)HTML");
        heartbeat.start(100);
        deadline.start(25000);
        view->setHtml(html);
        view->show();
        app.exec();
        if (view)
            delete view;
    }
    {
        std::lock_guard<std::mutex> lock(completionMutex);
        completed = true;
    }
    completionChanged.notify_one();
    watchdog.join();
    std::cout << "Rendering shutdown completed; exit code " << result << std::endl;
    return result;
}
