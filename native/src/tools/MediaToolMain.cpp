#include "app/InterfaceLanguage.h"
#include "services/MediaService.h"
#ifdef DESKTOPTOOL_VIDEO_TOOL
#include "app/VideoPlayerWindow.h"
#else
#include "app/ImageBrowserWindow.h"
#endif

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QDebug>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>

namespace {
// 两个入口共用参数处理，编译时决定媒体类型，运行时不加载主程序。
#ifdef DESKTOPTOOL_VIDEO_TOOL
constexpr bool VideoTool = true;
#else
constexpr bool VideoTool = false;
#endif

// 只读取界面语言，不初始化主程序服务，也不创建或改写用户设置。
void loadLanguage(const QString& dataRoot)
{
    QFile settings(QDir(dataRoot).filePath(QStringLiteral("setting/system_config.json")));
    if (settings.open(QIODevice::ReadOnly))
        InterfaceLanguage::setLanguage(QJsonDocument::fromJson(settings.readAll()).object()
            .value(QStringLiteral("language")).toString());
}
}

// 独立媒体工具入口：支持直接双击、命令行路径和 Windows 默认打开方式。
int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("DesktopTool"));
    app.setApplicationName(VideoTool ? QStringLiteral("DesktopToolVideoPlayer")
                                    : QStringLiteral("DesktopToolImageBrowser"));
    app.setApplicationVersion(QStringLiteral(DESKTOPTOOL_VERSION));
    app.setWindowIcon(QIcon(QStringLiteral(":/images/app_icon.ico")));

    QCommandLineParser parser;
    parser.setApplicationDescription(VideoTool ? QStringLiteral("独立视频播放器")
                                               : QStringLiteral("独立图像浏览器"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption dataOption(QStringLiteral("data-dir"),
        QStringLiteral("读取指定数据目录中的界面语言。"), QStringLiteral("path"));
    parser.addOption(dataOption);
    const QCommandLineOption registerOption(QStringLiteral("register-associations"),
        QStringLiteral("登记两个独立工具的打开方式并退出，保留系统现有默认选择。"));
    parser.addOption(registerOption);
    parser.addPositionalArgument(QStringLiteral("file"), QStringLiteral("要打开的本地媒体文件。"),
        QStringLiteral("[file]"));
    parser.process(app);
    // 安装器直接调用独立工具完成登记，不需要启动主程序或读取用户数据。
    if (parser.isSet(registerOption)) {
        MediaService media(true);
        const auto result = media.prepareDefaultApplications(
            MediaService::imageExtensions() + MediaService::videoExtensions());
        if (!result.value(QStringLiteral("ok")).toBool()) {
            qCritical().noquote() << result.value(QStringLiteral("error")).toString();
            return 1;
        }
        return 0;
    }
    loadLanguage(parser.isSet(dataOption) ? parser.value(dataOption)
        : QDir(MediaService::applicationRootPath()).filePath(QStringLiteral("data")));

    const QString title = InterfaceLanguage::text(VideoTool ? QStringLiteral("视频播放器")
                                                           : QStringLiteral("图像浏览器"));
    if (parser.positionalArguments().size() > 1) {
        QMessageBox::warning(nullptr, title, InterfaceLanguage::text(QStringLiteral("请一次选择一个媒体文件。")));
        return 1;
    }
    QString path = parser.positionalArguments().value(0);
    const QStringList extensions = VideoTool ? MediaService::videoExtensions() : MediaService::imageExtensions();
    if (path.isEmpty()) {
        QStringList patterns;
        for (const QString& extension : extensions)
            patterns.append(QStringLiteral("*.") + extension);
        path = QFileDialog::getOpenFileName(nullptr, title, QString(),
            InterfaceLanguage::text(VideoTool ? QStringLiteral("视频文件 (%1);;所有文件 (*.*)")
                : QStringLiteral("图片文件 (%1);;所有文件 (*.*)")).arg(patterns.join(QLatin1Char(' '))));
        if (path.isEmpty())
            return 0;
    }
    const QFileInfo file(path);
    if (!file.isFile() || !file.isReadable()
        || !(VideoTool ? MediaService::isVideoFile(path) : MediaService::isImageFile(path))) {
        QMessageBox::warning(nullptr, title,
            InterfaceLanguage::text(QStringLiteral("文件不存在、无法读取或格式不支持：\n%1")).arg(path));
        return 1;
    }
    QStringList files = MediaService::filesWithExtensions(file.absolutePath(), extensions);
    int index = files.indexOf(file.absoluteFilePath());
    if (index < 0) {
        files.prepend(file.absoluteFilePath());
        index = 0;
    }
#ifdef DESKTOPTOOL_VIDEO_TOOL
    auto* window = VideoPlayerWindow::open(files, index, nullptr);
#else
    auto* window = ImageBrowserWindow::open(files, index, nullptr);
#endif
    if (!window)
        return 1;
    return app.exec();
}
