#include "level/LevelWindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QMessageBox>

// 独立启动矢量编辑器，仅依赖 Qt Widgets，不初始化网页和主程序后台服务。
int main(int argc, char* argv[])
{
    QApplication app(argc,argv);
    app.setApplicationName("DesktopToolLevelEditor"); app.setOrganizationName("DesktopTool");
    QCommandLineParser parser; parser.addHelpOption();
    parser.addOption({"data-dir",QStringLiteral("与主程序共享的数据目录"),"path"});
    parser.addPositionalArgument("project",QStringLiteral("可选的 .level.json 项目路径")); parser.process(app);
    const QString root=parser.isSet("data-dir")?QDir(parser.value("data-dir")).absolutePath():QDir(app.applicationDirPath()).filePath("data");
    auto* window=new LevelWindow(root);
    if (!parser.positionalArguments().isEmpty()) {
        QString error;
        if (!window->openProject(parser.positionalArguments().first(),error)) {
            QMessageBox::warning(nullptr,QStringLiteral("打开关卡项目失败"),error); delete window; return 1;
        }
    }
    window->show(); return app.exec();
}
