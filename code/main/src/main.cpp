#include <iostream>
#include <QApplication>
#include <QFile>
#include <QLocalSocket>
#include <QLocalServer>
#include "LxSingleInstanceGuard.h"
#include "customizeObj/inc/LxCustomWindow.h"


int main(int argc, char* argv[]) {
    int ret = 0;
    {
        QApplication app(argc, argv);

        // 进行单例判定
        SingleInstanceGuard guard("DesktopTool_unique_key");
        if (guard.isRunning()) {
            // 已有实例，通知并退出
            QLocalSocket socket;
            socket.connectToServer("DesktopTool_server");
            if (socket.waitForConnected(500)) {
                socket.write("ACTIVATE");
                socket.flush();
            }
            return 0;
        }
        // 第一个实例，启动监听
        QLocalServer server;
        server.removeServer("DesktopTool_server");
        server.listen("DesktopTool_server");
        LxCustomWindow mainWindow;

        QObject::connect(&server, &QLocalServer::newConnection, [&]() {
            auto socket = server.nextPendingConnection();
            socket->readAll();

            // 显示并激活窗口
            mainWindow.showNormal();
            mainWindow.raise();
            mainWindow.activateWindow();
            });

        // 初始化 
        // 加载qss样式
        QFile file(":/qss/qss.css");
        if (file.open(QFile::ReadOnly)) {
            QString styleSheet = QLatin1String(file.readAll());
            app.setStyleSheet(styleSheet);
            file.close();
        }


        
        mainWindow.show();
        ret = app.exec();
    }

    return ret;
}