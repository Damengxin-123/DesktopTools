#include "app/RenderingPolicy.h"
#include "app/WallpaperWindow.h"
#include "document_editor.h"
#include "bridge/AppBridge.h"
#include "services/ServiceResult.h"
#include "services/QrService.h"

#include <QFileInfo>
#include <QImageReader>

#include <QApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMetaMethod>
#include <QSet>
#include <QSettings>
#include <QTextDocument>
#include <QTcpSocket>
#include <QTimer>
#include <memory>
#include <utility>

// 只允许新工作台所需的接口，排除旧网页、画板及关卡编辑等入口。
static const QSet<QByteArray> AllowedMethods = {
    "getShortcuts", "saveShortcut", "deleteShortcuts", "moveShortcut", "saveShortcutCategory",
    "deleteShortcutCategory", "openShortcut", "getNotes", "getNote", "saveNote", "deleteNotes",
    "moveNote", "saveNoteCategory", "deleteNoteCategory", "getEmojis", "addEmoji", "saveEmoji",
    "pasteEmojiImage", "deleteEmojis", "saveEmojiCategory", "deleteEmojiCategory",
    "openEmojiDirectory", "copyEmojiFile", "getDownloads", "createDownload", "getDownloadFiles",
    "confirmDownloadFiles", "pauseDownload", "resumeDownload", "cancelDownload", "removeDownload",
    "openDownloadDirectory", "getClipboardHistory", "setClipboardTypes", "getClipboardItem",
    "copyClipboardItem", "pinClipboardItem", "deleteClipboardItems", "clearClipboardHistory",
    "openClipboardDirectory", "getWallpaper", "pasteWallpaperResource", "addWallpaper", "useWallpaper",
    "useWallpaperOnScreen", "clearScreenWallpaper", "setWallpaperDisplayMode", "removeWallpapers",
    "setWallpaperEnabled", "openWallpaperDirectory", "getQrHistory", "pasteQrImage", "decodeQr",
    "readQr", "removeQrHistory", "clearQrHistory", "getScreenshotHistory", "startScreenshot",
    "setScreenshotEnabled", "saveScreenshotHotkey", "getScreenshot", "deleteScreenshots",
    "clearScreenshotHistory", "openScreenshotDirectory", "copyScreenshot", "getRecordingHistory",
    "startRecording", "getRecording", "deleteRecordings", "clearRecordingHistory",
    "openRecordingDirectory", "copyRecording", "openRecordingPlayer", "listMediaDirectory",
    "openImageBrowser", "openVideoPlayer", "getMediaAssociation", "configureMediaDefaults",
    "getFileSearchStatus", "searchFiles", "rebuildFileSearchIndex", "enableFileSearchService",
    "openFilePath", "openFileLocation", "getSettings", "saveSettings", "resetSettings",
    "getAppInfo", "openDataDirectory", "openGithubRepository"
};

// 返回侧栏计数，避免传输库中的完整缩略图。
static QVariantMap counts(const AppBridge& bridge, const QString& topic)
{
    QVariantMap result;
    const auto add = [&](const QString& name, auto snapshot) {
        if (!topic.isEmpty() && topic != name)
            return;
        const auto response = snapshot().toMap();
        if (!response.value("ok").toBool())
            return;
        const auto data = response.value("data").toMap();
        result.insert(name, data.value("total", data.value("items").toList().size()));
    };
    add("shortcuts", [&] { return QVariant(bridge.getShortcuts()); });
    add("notes", [&] { return QVariant(bridge.getNotes()); });
    add("emoji", [&] { return QVariant(bridge.getEmojis({})); });
    add("downloads", [&] { return QVariant(bridge.getDownloads()); });
    add("clipboard", [&] { return QVariant(bridge.getClipboardHistory({})); });
    add("wallpaper", [&] { return QVariant(bridge.getWallpaper()); });
    add("qr", [&] { return QVariant(bridge.getQrHistory()); });
    add("screenshot", [&] { return QVariant(bridge.getScreenshotHistory()); });
    add("recording", [&] { return QVariant(bridge.getRecordingHistory()); });
    add("filesearch", [&] { return QVariant(bridge.getFileSearchStatus()); });
    return ServiceResult::success(result);
}

// 在 C++ 中读取、校验并编码二维码预览图片。
static QVariantMap loadQrImage(const QString& path)
{
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
    const auto size = reader.size();
    if (size.isValid() && qint64(size.width()) * size.height() > 20000000)
        return ServiceResult::failure(QStringLiteral("图片超过 2000 万像素。"));
    const auto image = QrService::encodeBoundedImage(reader.read());
    if (image.isEmpty())
        return ServiceResult::failure(QStringLiteral("图片无法读取或体积过大。"));
    return ServiceResult::success(QVariantMap{{"image", image}, {"source", QFileInfo(path).fileName().left(200)}});
}

// 使用独立的 Windows 自启项，避免更改旧程序的启动命令。
static QVariantMap startup(const QJsonArray& values)
{
    QSettings registry(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
                       QSettings::NativeFormat);
    const auto key = QStringLiteral("DesktopToolGodot");
    if (values.isEmpty())
        return ServiceResult::success(QVariantMap{{"enabled", registry.contains(key)}});
    if (values.size() != 2 || !values.at(0).isBool() || !values.at(1).isString())
        return ServiceResult::failure(QStringLiteral("自启设置无效。"));
    const bool enabled = values.at(0).toBool();
    const auto command = values.at(1).toString();
    if (enabled && command.isEmpty())
        return ServiceResult::failure(QStringLiteral("找不到 Godot 工作台的启动命令。"));
    if (enabled)
        registry.setValue(key, command);
    else
        registry.remove(key);
    registry.sync();
    if (registry.status() != QSettings::NoError)
        return ServiceResult::failure(QStringLiteral("无法写入当前用户的开机启动项。"));
    return ServiceResult::success(QVariantMap{{"enabled", enabled}});
}

// 校验调用名称和参数类型，再在 C++ 服务进程中执行。
static QVariantMap dispatch(AppBridge& bridge, const QJsonObject& request)
{
    const auto name = request.value("method").toString().toLatin1();
    const auto values = request.value("args").toArray();
    if (name == "saveNote" && values.size() == 1 && values.at(0).isObject()) {
        auto note = values.at(0).toObject().toVariantMap();
        const auto content = note.value("html").toString();
        if (!Qt::mightBeRichText(content))
            note.insert("html", Qt::convertFromPlainText(content, Qt::WhiteSpaceNormal));
        return bridge.saveNote(note);
    }
    if (name == "startup")
        return startup(values);
    if (name == "getCounts")
        return counts(bridge, values.isEmpty() ? QString() : values.at(0).toString());
    if (name == "loadQrImage" && values.size() == 1)
        return loadQrImage(values.at(0).toString());
    if (!AllowedMethods.contains(name))
        return ServiceResult::failure(QStringLiteral("未知工作台操作。"));
    const QMetaObject& meta = AppBridge::staticMetaObject;
    for (int index = meta.methodOffset(); index < meta.methodCount(); ++index) {
        const auto method = meta.method(index);
        if (method.name() != name || method.methodType() != QMetaMethod::Method ||
            method.parameterCount() != values.size() || values.size() > 4)
            continue;

        QVariant converted[4];
        QGenericArgument args[4];
        const auto types = method.parameterTypes();
        for (int arg = 0; arg < values.size(); ++arg) {
            converted[arg] = values.at(arg).toVariant();
            if (!converted[arg].convert(method.parameterMetaType(arg)))
                return ServiceResult::failure(QStringLiteral("操作参数无效。"));
            args[arg] = QGenericArgument(types.at(arg).constData(), converted[arg].constData());
        }
        QVariantMap result;
        if (method.invoke(&bridge, Qt::DirectConnection, QGenericReturnArgument("QVariantMap", &result),
                          args[0], args[1], args[2], args[3]))
            return result;
    }
    return ServiceResult::failure(QStringLiteral("操作签名无效。"));
}

// 通过本机套接字收发逐行 JSON，Godot 退出后结束工作进程。
int main(int argc, char** argv)
{
    RenderingPolicy::configure();
    WallpaperWindow::registerMediaScheme();
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setApplicationName(QStringLiteral("DesktopToolNative"));
    app.setOrganizationName(QStringLiteral("DesktopToolGodot"));
    app.setApplicationVersion(QStringLiteral(DESKTOPTOOL_VERSION));

    const QStringList args = app.arguments();
    if (args.size() != 4 && args.size() != 5)
        return 2;

    QTcpSocket socket;
    socket.connectToHost(QStringLiteral("127.0.0.1"), args.at(1).toUShort());
    if (!socket.waitForConnected(5000))
        return 3;

    // 会话令牌和数据锁分别保护连接和同一目录的并发写入。
    const auto send = [&socket](const QJsonObject& message) {
        socket.write(QJsonDocument(message).toJson(QJsonDocument::Compact));
        socket.write("\n");
    };
    const QString dataRoot = QDir(args.at(3)).absolutePath();
    QDir().mkpath(dataRoot);
    QLockFile lock(QDir(dataRoot).filePath(QStringLiteral(".godot-backend.lock")));
    lock.setStaleLockTime(0);
    const bool locked = lock.tryLock();
    send({{"token", args.at(2)}, {"error", locked ? QString() : QStringLiteral("数据目录正在使用或无法写入。")}});
    if (!locked) {
        if (socket.bytesToWrite())
            socket.waitForBytesWritten(1000);
        return 4;
    }

    AppBridge bridge(dataRoot, nullptr, !args.contains(QStringLiteral("--isolated")));
    const auto event = [&send](const QString& name, const QVariantMap& payload = {}) {
        send({{"event", name}, {"data", QJsonObject::fromVariantMap(payload)}});
    };
    // 编辑器独立于主窗口，主界面关闭后仍可保存已打开的文档。
    std::unique_ptr<DocumentEditor> editor;
    QObject::connect(&socket, &QTcpSocket::disconnected, &app, [&] {
        if (!editor || !editor->isVisible())
            app.quit();
    });
    const auto handle = [&](const QJsonObject& request) {
        const auto method = request.value("method").toString();
        const auto values = request.value("args").toArray();
        if ((method == QStringLiteral("openDocument") && values.size() == 3)
            || (method == QStringLiteral("importDocument") && values.size() == 2)) {
            if (!editor) {
                editor = std::make_unique<DocumentEditor>(bridge);
                QObject::connect(editor.get(), &DocumentEditor::editorClosed, &app, [&] {
                    if (socket.state() == QAbstractSocket::UnconnectedState)
                        app.quit();
                });
            }
            if (method == QStringLiteral("importDocument"))
                return editor->importPackage(values.at(0).toString(), values.at(1).toString());
            return editor->openDocument(values.at(0).toString(), values.at(1).toString(), values.at(2).toString());
        }
        if (method == QStringLiteral("setDocumentAppearance") && values.size() == 1) {
            if (editor)
                editor->setAppearance(values.at(0).toString());
            return ServiceResult::success();
        }
        return dispatch(bridge, request);
    };
    QObject::connect(&bridge, &AppBridge::fileSearchResults, &app,
        [&event](const QVariantMap& result) { event("fileSearchResults", result); });

    // 合并连续的进度信号，避免高频下载和索引刷新淹没界面。
    QSet<QString> changed;
    QTimer update;
    update.setSingleShot(true);
    update.setInterval(180);
    QObject::connect(&update, &QTimer::timeout, &app, [&] {
        const auto pending = std::exchange(changed, {});
        for (const auto& name : pending)
            event(name);
    });
    const auto watch = [&](auto signal, const QString& name) {
        QObject::connect(&bridge, signal, &app, [&, name] {
            changed.insert(name);
            if (!update.isActive())
                update.start();
        });
    };
    watch(&AppBridge::shortcutsChanged, "shortcuts");
    watch(&AppBridge::notesChanged, "notes");
    watch(&AppBridge::emojisChanged, "emoji");
    watch(&AppBridge::downloadsChanged, "downloads");
    watch(&AppBridge::clipboardChanged, "clipboard");
    watch(&AppBridge::wallpaperChanged, "wallpaper");
    watch(&AppBridge::qrHistoryChanged, "qr");
    watch(&AppBridge::screenshotsChanged, "screenshot");
    watch(&AppBridge::recordingsChanged, "recording");
    watch(&AppBridge::fileSearchStatusChanged, "fileSearchStatus");
    QObject::connect(&bridge, &AppBridge::activateWindowRequested, &app,
        [&event] { event("activate"); });

    const auto read = [&] {
        constexpr qint64 Limit = 48 * 1024 * 1024;
        while (socket.canReadLine()) {
            const auto line = socket.readLine(Limit);
            if (!line.endsWith('\n')) {
                socket.disconnectFromHost();
                return;
            }
            const auto request = QJsonDocument::fromJson(line).object();
            const auto result = handle(request);
            send({{"id", request.value("id")}, {"result", QJsonObject::fromVariantMap(result)}});
        }
        if (socket.bytesAvailable() >= Limit)
            socket.disconnectFromHost();
    };
    QObject::connect(&socket, &QTcpSocket::readyRead, &app, read);
    read();
    return app.exec();
}
