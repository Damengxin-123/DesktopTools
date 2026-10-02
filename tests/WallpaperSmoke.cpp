#include "app/WallpaperWindow.h"
#include "services/WallpaperService.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QFile>
#include <QJsonDocument>
#include <QMediaPlayer>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoSink>
#include <QVideoWidget>
#include <QWebEngineView>
#include <cstdio>
#include <memory>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
// 同步等待一次页面求值，超时回调不引用已销毁栈变量。
QVariant evaluate(QWebEnginePage* page, const QString& script)
{
    QEventLoop loop;
    auto result = std::make_shared<QVariant>();
    QPointer<QEventLoop> guarded(&loop);
    page->runJavaScript(script, [result, guarded](const QVariant& value) {
        *result = value;
        if (guarded)
            guarded->quit();
    });
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    return *result;
}

// 获取本进程现存壁纸实例，不依赖服务的私有容器。
QList<WallpaperWindow*> windows()
{
    QList<WallpaperWindow*> result;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* window = qobject_cast<WallpaperWindow*>(widget))
            result.append(window);
    }
    return result;
}

// 验证完整物理矩形、禁用输入且位于不含桌面图标的 WorkerW 内。
bool verifyWindow(WallpaperWindow* window, const QVariantList& screens)
{
    if (!window->isAlive())
        return false;
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    RECT actual{};
    GetWindowRect(hwnd, &actual);
    wchar_t hostClass[128]{};
    const HWND host = GetAncestor(hwnd, GA_PARENT);
    GetClassNameW(host, hostClass, 128);
    if (wcscmp(hostClass, L"WorkerW") != 0 || IsWindowEnabled(hwnd)
        || FindWindowExW(host, nullptr, L"SHELLDLL_DefView", nullptr))
        return false;
    bool matched = false;
    for (const QVariant& value : screens) {
        const auto screen = value.toMap();
        matched |= actual.left == screen.value("x").toInt() && actual.top == screen.value("y").toInt()
            && actual.right - actual.left == screen.value("width").toInt()
            && actual.bottom - actual.top == screen.value("height").toInt();
    }
    if (!matched) {
        std::printf("FAIL rectangle %ld,%ld %ldx%ld\n", actual.left, actual.top,
            actual.right - actual.left, actual.bottom - actual.top);
        return false;
    }
    // 原生子视图也必须铺满，外框正确但 WebEngine 尺寸错误同样失败。
    if (window->videoView())
        return window->videoView()->geometry() == window->rect();
    RECT content{};
    GetWindowRect(reinterpret_cast<HWND>(window->webView()->winId()), &content);
    if (!EqualRect(&actual, &content)) {
        std::printf("FAIL webview rectangle %ld,%ld %ldx%ld\n", content.left, content.top,
            content.right - content.left, content.bottom - content.top);
        return false;
    }
#endif
    return true;
}

// 等待事件处理，不用阻塞睡眠，确保解码和异步错误能继续交付。
void processFor(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

// 在真实 WorkerW 和全部屏幕验证视频首帧、非黑图像、循环及失败恢复。
bool verifyVideo(WallpaperService& service, const QVariantList& screens,
    const QString& path, const QString& invalidPath)
{
    for (const QVariant& value : screens) {
        if (!service.add({{"path", path}, {"screenId", value.toMap().value("id")}}).value("ok").toBool())
            return false;
    }
    if (!service.setEnabled(true).value("ok").toBool())
        return false;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 15000) {
        bool ready = windows().size() == screens.size();
        for (auto* window : windows())
            ready &= window->isMediaReady();
        if (ready)
            break;
        processFor(50);
    }
    if (windows().size() != screens.size()) {
        std::printf("FAIL video load: %s\n", QJsonDocument::fromVariant(service.snapshot()).toJson().constData());
        return false;
    }
    for (auto* window : windows()) {
        if (!window->isMediaReady() || !window->videoView() || !verifyWindow(window, screens))
            return false;
        const QImage frame = window->videoView()->videoSink()->videoFrame().toImage();
        int bright = 0;
        const QImage sample = frame.scaled(64, 36);
        for (int y = 0; y < sample.height(); ++y)
            for (int x = 0; x < sample.width(); ++x)
                bright += qGray(sample.pixel(x, y)) > 40;
        if (bright < 100)
            return false;
        auto* player = window->findChild<QMediaPlayer*>();
        if (!player || player->duration() <= 0 || player->audioOutput())
            return false;
        bool looped = false;
        qint64 previous = player->position();
        timer.restart();
        while (timer.elapsed() < player->duration() + 4000 && !looped) {
            processFor(50);
            const qint64 current = player->position();
            looped = current < previous;
            previous = current;
        }
        if (!looped)
            return false;
        std::printf("PASS native video: %dx%d decoded, non-black, looped, full monitor bounds\n", frame.width(), frame.height());
    }
    // 单屏错误不影响其他屏幕，且超过两次守护周期仍不重建失败窗口。
    const QString target = screens.first().toMap().value("id").toString();
    if (!service.add({{"path", invalidPath}, {"screenId", target}}).value("ok").toBool())
        return false;
    timer.restart();
    while (timer.elapsed() < 15000 && windows().size() == screens.size())
        processFor(50);
    processFor(6500);
    const auto snapshot = service.snapshot().value("data").toMap();
    const auto first = snapshot.value("screens").toList().first().toMap();
    if (windows().size() != screens.size() - 1 || first.value("playbackError").toString().isEmpty()
        || first.value("running").toBool())
        return false;
    // 主动选回有效资源恢复独立播放。
    if (!service.add({{"path", path}, {"screenId", target}}).value("ok").toBool())
        return false;
    timer.restart();
    while (timer.elapsed() < 15000) {
        bool ready = windows().size() == screens.size();
        for (auto* window : windows())
            ready &= window->isMediaReady();
        if (ready)
            break;
        processFor(50);
    }
    if (windows().size() != screens.size())
        return false;
    QList<quintptr> handles;
    for (auto* window : windows()) {
        if (!window->isMediaReady())
            return false;
        handles.append(window->winId());
    }
    if (!service.setEnabled(false).value("ok").toBool() || !windows().isEmpty())
        return false;
#ifdef Q_OS_WIN
    for (quintptr handle : handles)
        if (IsWindow(reinterpret_cast<HWND>(handle)))
            return false;
#endif
    std::puts("PASS corrupt video: per-screen error, no watchdog retry, recovery, immediate teardown");
    return true;
}
}

// 手动原生冒烟：临时资源在所有连接屏幕循环开启与关闭，不修改用户持久化设置。
int main(int argc, char* argv[])
{
    WallpaperWindow::registerMediaScheme();
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    const QVariantList screens = WallpaperWindow::screens();
    std::printf("screens: %s\n", QJsonDocument::fromVariant(screens).toJson(QJsonDocument::Compact).constData());
    if (screens.isEmpty())
        return 3;
    QTemporaryDir directory;
    if (!directory.isValid()) {
        std::printf("FAIL temporary directory: %s\n", directory.errorString().toUtf8().constData());
        return 3;
    }
    WallpaperService service(directory.path());
    for (qsizetype index = 0; index < screens.size(); ++index) {
        QImage image(640, 400, QImage::Format_RGB32);
        image.fill(index % 2 ? QColor("#a44888") : QColor("#2452c5"));
        const QString path = directory.filePath(QStringLiteral("screen-%1.png").arg(index));
        if (!image.save(path)) {
            std::printf("FAIL creating test image: %s\n", path.toUtf8().constData());
            return 3;
        }
        const auto added = service.add({{"path", path}, {"screenId", screens.at(index).toMap().value("id")}});
        if (!added.value("ok").toBool()) {
            std::printf("FAIL adding test image: %s\n", added.value("error").toString().toUtf8().constData());
            return 3;
        }
    }
    for (int cycle = 0; cycle < 3; ++cycle) {
        const auto enabled = service.setEnabled(true);
        if (!enabled.value("ok").toBool()) {
            std::printf("FAIL attach: %s\n", enabled.value("error").toString().toUtf8().constData());
            return 1;
        }
        if (windows().size() != screens.size())
            return 4;
        QList<quintptr> handles;
        for (WallpaperWindow* window : windows()) {
            QElapsedTimer timer;
            timer.start();
            bool loaded = false;
            while (!loaded && timer.elapsed() < 10000) {
                loaded = evaluate(window->webView()->page(), QStringLiteral(
                    "!!document.querySelector('#wallpaper-content img') && document.querySelector('#wallpaper-content img').naturalWidth > 0")).toBool();
            }
            if (!loaded || !verifyWindow(window, screens)) {
                std::puts("FAIL media or native window geometry/input");
                return 5;
            }
            handles.append(window->winId());
        }
        if (cycle == 0) {
            // 验证五种方式确实到达桌面页面，且修改方式没有替换或重新加载图片节点。
            WallpaperWindow* target = nullptr;
            for (WallpaperWindow* window : windows()) {
                if (evaluate(window->webView()->page(), QStringLiteral("document.querySelector('img').src.includes('screen-0.png')")).toBool())
                    target = window;
            }
            if (!target)
                return 8;
            evaluate(target->webView()->page(), QStringLiteral("document.querySelector('img').dataset.retained = 'yes'"));
            const QString screenId = screens.first().toMap().value("id").toString();
            for (const QString& mode : {QStringLiteral("fill"), QStringLiteral("fit"), QStringLiteral("stretch"), QStringLiteral("tile"), QStringLiteral("center")}) {
                if (!service.setDisplayMode(screenId, mode).value("ok").toBool())
                    return 8;
                const QString expected = mode == "fill" ? "cover" : mode == "fit" ? "contain" : mode == "stretch" ? "100% 100%" : QString();
                const QString check = expected.isEmpty()
                    ? QStringLiteral("Math.abs(parseFloat(c.style.backgroundSize) - 640*c.clientWidth/%1) < .01 && c.style.backgroundRepeat === '%2'")
                        .arg(screens.first().toMap().value("width").toInt()).arg(mode == "tile" ? "repeat" : "no-repeat")
                    : QStringLiteral("c.style.backgroundSize === '%1'").arg(expected);
                if (!evaluate(target->webView()->page(), QStringLiteral(
                        "(()=>{const c=document.getElementById('wallpaper-content'); return document.querySelector('img').dataset.retained === 'yes' && %1;})()")
                        .arg(check)).toBool()) {
                    std::printf("FAIL display mode %s\n", mode.toUtf8().constData());
                    return 8;
                }
            }
            if (!service.setDisplayMode(screenId, QStringLiteral("fill")).value("ok").toBool())
                return 8;
            std::puts("PASS five image modes and retained media node");
        }
        if (!service.setEnabled(false).value("ok").toBool() || !windows().isEmpty())
            return 6;
#ifdef Q_OS_WIN
        // 不处理 DeferredDelete，关闭 API 返回时原生窗口就必须已经销毁。
        for (quintptr handle : handles) {
            if (IsWindow(reinterpret_cast<HWND>(handle))) {
                std::puts("FAIL native window remained after disabling");
                return 7;
            }
        }
#endif
        std::printf("PASS cycle %d: %lld monitors, media, full bounds, input disabled, immediate teardown\n",
            cycle + 1, static_cast<long long>(screens.size()));
    }
    if (app.arguments().size() > 1) {
        QFile invalid(directory.filePath(QStringLiteral("broken.mp4")));
        if (!invalid.open(QIODevice::WriteOnly))
            return 9;
        invalid.write("invalid video");
        invalid.close();
        if (!verifyVideo(service, screens, app.arguments().at(1), invalid.fileName())) {
            std::puts("FAIL native video regression");
            return 9;
        }
    }
    return 0;
}
