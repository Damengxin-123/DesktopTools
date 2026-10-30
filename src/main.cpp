#include "app/RenderingPolicy.h"
#include "app/WebWindow.h"
#include "services/MediaService.h"
#include "app/WallpaperWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QMessageBox>
#include <QSharedMemory>
#include <QStandardPaths>
#include <QTimer>

// 初始化原生进程、兼容数据目录与单实例唤醒，再进入 Qt 事件循环。
int main(int argc, char* argv[])
{
    // 图形策略须早于 QApplication 与任何 WebEngine/壁纸窗口初始化。
    RenderingPolicy::configure();
    // 壁纸媒体协议须在创建应用对象前注册。
    WallpaperWindow::registerMediaScheme();
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("DesktopTool"));
    app.setOrganizationName(QStringLiteral("DesktopTool"));
    app.setApplicationVersion(QStringLiteral(DESKTOPTOOL_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("HTML 界面与 Qt 后端的桌面工具"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption dataOption(QStringLiteral("data-dir"),
        QStringLiteral("指定数据目录，默认使用可执行文件旁的 data。"), QStringLiteral("path"));
    parser.addOption(dataOption);
    parser.process(app);
    const QString dataRoot = parser.isSet(dataOption)
        ? QDir(parser.value(dataOption)).absolutePath()
        : QDir(app.applicationDirPath()).filePath(QStringLiteral("data"));
    // 作为默认看图/播放程序时，资源管理器会把文件路径作为首个位置参数传入。
    const QString fileToOpen = parser.positionalArguments().isEmpty()
        ? QString() : parser.positionalArguments().first();

    // 兼容旧关联命令：直接转交独立工具，不创建主窗口或初始化后台服务。
    if (!fileToOpen.isEmpty() && (MediaService::isImageFile(fileToOpen) || MediaService::isVideoFile(fileToOpen))) {
        MediaService media(false);
        const auto result = media.launchFile(fileToOpen, dataRoot);
        if (!result.value(QStringLiteral("ok")).toBool()) {
            QMessageBox::warning(nullptr, QStringLiteral("资源浏览器"), result.value(QStringLiteral("error")).toString());
            return 1;
        }
        return 0;
    }

    // 与旧版本保持相同的唤醒服务名，避免新旧版本同时修改同一份数据。
    const QString serverName = QStringLiteral("DesktopTool_server");
    // 唤醒已运行实例；携带文件时通过 OPEN 消息转发，等待消息写出后才退出。
    const auto activateExisting = [&serverName](const QString& file) {
        QLocalSocket socket;
        socket.connectToServer(serverName);
        if (!socket.waitForConnected(600))
            return false;
        const QByteArray payload = file.isEmpty()
            ? QByteArrayLiteral("ACTIVATE")
            : QByteArrayLiteral("OPEN\t") + file.toUtf8();
        socket.write(payload);
        socket.flush();
        if (socket.bytesToWrite() > 0)
            socket.waitForBytesWritten(600);
        return true;
    };
    if (activateExisting(fileToOpen))
        return 0;

    const QString runtimePath = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QLockFile lock(QDir(runtimePath).filePath(QStringLiteral("DesktopTool.instance.lock")));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(1200)) {
        if (activateExisting(fileToOpen))
            return 0;
        QMessageBox::warning(nullptr, QStringLiteral("桌面小工具"), QStringLiteral("另一个实例正在启动，请稍后重试。"));
        return 1;
    }
    // 同时持有旧版共享内存标识，使后启动的旧版也能识别新版实例。
    QSharedMemory legacyGuard(QStringLiteral("DesktopTool_unique_key"));
    if (!legacyGuard.create(1)) {
        if (activateExisting(fileToOpen))
            return 0;
        QMessageBox::warning(nullptr, QStringLiteral("桌面小工具"),
            QStringLiteral("旧版实例正在启动，或无法创建兼容实例锁，请稍后重试。"));
        return 1;
    }
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server.listen(serverName)) {
        QMessageBox::warning(nullptr, QStringLiteral("桌面小工具"), QStringLiteral("无法创建单实例通信服务。"));
        return 1;
    }
    WebWindow window(dataRoot);
    QObject::connect(&server, &QLocalServer::newConnection, &window, [&server, &window]() {
        while (auto* socket = server.nextPendingConnection()) {
            // 消息很短且客户端写入后才退出，readyRead 时已完整可读。
            QObject::connect(socket, &QLocalSocket::readyRead, socket, [&window, socket]() {
                const QByteArray message = socket->readAll();
                if (message.startsWith("OPEN\t"))
                    window.openLocalFile(QString::fromUtf8(message.mid(5)));
                else
                    window.activate();
                socket->disconnectFromServer();
            });
            // 兜底：迟迟未发送消息的连接直接丢弃。
            QTimer::singleShot(2000, socket, &QObject::deleteLater);
        }
    });
    window.show();
    if (!fileToOpen.isEmpty())
        window.openLocalFile(fileToOpen);
    return app.exec();
}
