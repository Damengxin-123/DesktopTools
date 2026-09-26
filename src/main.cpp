#include "app/WebWindow.h"

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

    // 与旧版本保持相同的唤醒服务名，避免新旧版本同时修改同一份数据。
    const QString serverName = QStringLiteral("DesktopTool_server");
    // 唤醒已运行实例，等待消息写出后才退出，避免 socket 析构吞掉通知。
    const auto activateExisting = [&serverName]() {
        QLocalSocket socket;
        socket.connectToServer(serverName);
        if (!socket.waitForConnected(600))
            return false;
        socket.write("ACTIVATE");
        socket.flush();
        if (socket.bytesToWrite() > 0)
            socket.waitForBytesWritten(600);
        return true;
    };
    if (activateExisting())
        return 0;

    const QString runtimePath = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QLockFile lock(QDir(runtimePath).filePath(QStringLiteral("DesktopTool.instance.lock")));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(1200)) {
        if (activateExisting())
            return 0;
        QMessageBox::warning(nullptr, QStringLiteral("桌面小工具"), QStringLiteral("另一个实例正在启动，请稍后重试。"));
        return 1;
    }
    // 同时持有旧版共享内存标识，使后启动的旧版也能识别新版实例。
    QSharedMemory legacyGuard(QStringLiteral("DesktopTool_unique_key"));
    if (!legacyGuard.create(1)) {
        if (activateExisting())
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
            window.activate();
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            socket->disconnectFromServer();
            QTimer::singleShot(1000, socket, &QObject::deleteLater);
        }
    });
    window.show();
    return app.exec();
}
