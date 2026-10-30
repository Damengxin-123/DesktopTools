#include "services/MediaService.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>
#include <QUuid>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
// 提取服务调用结果。
bool ok(const QVariantMap& result) { return result.value("ok").toBool(); }
QVariantMap data(const QVariantMap& result) { return result.value("data").toMap(); }
// 图像/视频浏览器的 ProgId，与服务内部常量保持一致。
const QString kImageProgId = QStringLiteral("DesktopTool.ImageBrowser");
const QString kVideoProgId = QStringLiteral("DesktopTool.VideoPlayer");
// 写入指定内容，用于构造测试媒体文件。
bool writeFile(const QString& path, const QByteArray& content)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}
} // namespace

// 覆盖媒体分类、目录清单与默认打开方式关联的注册表读写（注入存储）。
class MediaServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    // 按扩展名分类，目录清单只含图片与视频并统计数量。
    void classificationAndListing();
    // 登记独立工具的命令及选中格式，不覆盖系统默认关联。
    void associationToggle();
    // 在隔离的临时注册表键中验证原生打开命令布局，不触碰真实文件关联。
    void nativeCommandLayout();
    // 登记独立工具时保留 Explorer 的 UserChoice 和其他程序关联。
    void associationPreservesForeignHandlers();
    // 扩展名生效状态与名称解析：本程序、其他程序与未关联。
    void extensionHandlerReporting();
    // 非原生环境（无注册表存储）返回明确错误且状态不受支持。
    void unsupportedEnvironment();
};

void MediaServiceTest::classificationAndListing()
{
    QVERIFY(MediaService::isImageFile(QStringLiteral("照片.PNG")));
    QVERIFY(MediaService::isImageFile(QStringLiteral("/tmp/a.jpeg")));
    QVERIFY(!MediaService::isImageFile(QStringLiteral("clip.mp4")));
    QVERIFY(MediaService::isVideoFile(QStringLiteral("clip.MP4")));
    QVERIFY(!MediaService::isVideoFile(QStringLiteral("notes.txt")));

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(writeFile(root.filePath(QStringLiteral("a.png")), "x"));
    QVERIFY(writeFile(root.filePath(QStringLiteral("b.webp")), "x"));
    QVERIFY(writeFile(root.filePath(QStringLiteral("c.mp4")), "x"));
    QVERIFY(writeFile(root.filePath(QStringLiteral("readme.txt")), "x"));
    const QDir dir(root.path());

    // 同目录图片按文件名排序，可作为浏览器内切换列表。
    const QStringList images = MediaService::filesWithExtensions(dir.absolutePath(),
        MediaService::imageExtensions());
    QCOMPARE(images.size(), 2);
    QCOMPARE(images.first(), dir.absoluteFilePath(QStringLiteral("a.png")));

    MediaService service(false, this);
    const auto listing = service.listDirectory(dir.absolutePath());
    QVERIFY(ok(listing));
    const auto items = data(listing).value("items").toList();
    QCOMPARE(items.size(), 3);
    QCOMPARE(data(listing).value("imageCount").toInt(), 2);
    QCOMPARE(data(listing).value("videoCount").toInt(), 1);
    QCOMPARE(data(listing).value("truncated").toBool(), false);
    const auto first = items.first().toMap();
    QCOMPARE(first.value("name").toString(), QStringLiteral("a.png"));
    QCOMPARE(first.value("kind").toString(), QStringLiteral("image"));
    QCOMPARE(first.value("bytes").toLongLong(), qint64(1));

    // 缺失目录返回明确错误；空目录返回空清单。
    QVERIFY(!ok(service.listDirectory(root.filePath(QStringLiteral("不存在")))));
}

void MediaServiceTest::associationToggle()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QSettings classes(root.filePath(QStringLiteral("classes.ini")), QSettings::IniFormat);
    QSettings fileExts(root.filePath(QStringLiteral("fileexts.ini")), QSettings::IniFormat);
    MediaService service(this, &classes, &fileExts);

    const auto before = service.associationStatus();
    QVERIFY(ok(before));
    QCOMPARE(data(before).value("supported").toBool(), true);
    QCOMPARE(data(before).value("imageDefault").toBool(), false);
    QCOMPARE(data(before).value("videoDefault").toBool(), false);

    // 登记所选格式不会直接改变系统默认值，也不会为未选格式登记能力。
    const auto prepared = service.prepareDefaultApplications({"PNG", ".jpg", "png"});
    QVERIFY(ok(prepared));
    QCOMPARE(data(prepared).value("selected").toStringList(), QStringList({"png", "jpg"}));
    QCOMPARE(classes.value("DesktopTool.ImageBrowser/Capabilities/FileAssociations/.png").toString(), kImageProgId);
    QVERIFY(!classes.contains("DesktopTool.ImageBrowser/Capabilities/FileAssociations/.gif"));
    QVERIFY(!classes.contains(".png"));
    QCOMPARE(classes.value(kImageProgId + "/shell/open/command/Default").toString(),
        QStringLiteral("\"%1\" \"%2\"").arg(QDir::toNativeSeparators(MediaService::toolExecutablePath(false)), QStringLiteral("%1")));
    const QString imageCommand = classes.value(kImageProgId + "/shell/open/command/Default").toString();
    QCOMPARE(classes.value("Applications/DesktopToolImageBrowser.exe/shell/open/command/Default").toString(), imageCommand);
    QVERIFY(!classes.contains("DesktopTool/Capabilities"));
    QVERIFY(!classes.contains("Applications/DesktopTool.exe/shell/open/command/Default"));
    QVERIFY(classes.contains("Applications/DesktopTool.exe/NoOpenWith"));
    QVERIFY(!data(service.associationStatus()).value("imageDefault").toBool());
    QVERIFY(!ok(service.prepareDefaultApplications({})));
    QVERIFY(!ok(service.prepareDefaultApplications({"png", "exe"})));
    QVERIFY(classes.contains("DesktopTool.ImageBrowser/Capabilities/FileAssociations/.jpg"));
    QVERIFY(ok(service.prepareDefaultApplications({"mp4"})));
    QVERIFY(!classes.contains("DesktopTool.ImageBrowser/Capabilities/FileAssociations/.png"));
    QCOMPARE(classes.value("DesktopTool.VideoPlayer/Capabilities/FileAssociations/.mp4").toString(), kVideoProgId);
    QCOMPARE(classes.value(kVideoProgId + "/shell/open/command/Default").toString(),
        QStringLiteral("\"%1\" \"%2\"").arg(QDir::toNativeSeparators(MediaService::toolExecutablePath(true)), QStringLiteral("%1")));
    QVERIFY(ok(service.prepareDefaultApplications({"png", "mp4"})));
    QCOMPARE(classes.value("DesktopTool.ImageBrowser/Capabilities/FileAssociations/.png").toString(), kImageProgId);
    QCOMPARE(classes.value("DesktopTool.VideoPlayer/Capabilities/FileAssociations/.mp4").toString(), kVideoProgId);
}

void MediaServiceTest::nativeCommandLayout()
{
#ifdef Q_OS_WIN
    QTemporaryDir root;
    const QString key = QStringLiteral("Software\\DesktopToolTests\\") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSettings classes(QStringLiteral("HKEY_CURRENT_USER\\") + key, QSettings::NativeFormat);
    QSettings fileExts(root.filePath("fileexts.ini"), QSettings::IniFormat);
    MediaService service(this, &classes, &fileExts);
    const auto prepared = service.prepareDefaultApplications({"png"});
    const QString commandKey = key + QStringLiteral("\\DesktopTool.ImageBrowser\\shell\\open\\command");
    wchar_t buffer[4096] = {};
    DWORD bytes = sizeof(buffer);
    const LSTATUS result = RegGetValueW(HKEY_CURRENT_USER,
        reinterpret_cast<PCWSTR>(commandKey.utf16()), nullptr, RRF_RT_REG_SZ, nullptr, buffer, &bytes);
    const QString actual = QString::fromWCharArray(buffer);
    // 无论断言是否成功，先清理本测试独占的 UUID 子键。
    classes.remove(QString());
    classes.sync();
    QVERIFY(ok(prepared));
    QCOMPARE(result, LSTATUS(ERROR_SUCCESS));
    QCOMPARE(actual, QStringLiteral("\"%1\" \"%2\"").arg(
        QDir::toNativeSeparators(MediaService::toolExecutablePath(false)), QStringLiteral("%1")));
#else
    QSKIP("仅 Windows 提供注册表");
#endif
}

void MediaServiceTest::associationPreservesForeignHandlers()
{
    QTemporaryDir root;
    QSettings classes(root.filePath("classes.ini"), QSettings::IniFormat);
    QSettings fileExts(root.filePath("fileexts.ini"), QSettings::IniFormat);
    classes.setValue(".png", "OtherViewer");
    fileExts.setValue(".png/UserChoice/ProgId", "OtherApp");
    fileExts.setValue(".png/UserChoice/Hash", "protected-hash");
    MediaService service(this, &classes, &fileExts);
    QVERIFY(ok(service.prepareDefaultApplications({"png"})));
    QVERIFY(!ok(service.setImageBrowserDefault(true)));
    QVERIFY(!ok(service.setImageBrowserDefault(false)));
    QCOMPARE(classes.value(".png").toString(), QStringLiteral("OtherViewer"));
    QCOMPARE(fileExts.value(".png/UserChoice/ProgId").toString(), QStringLiteral("OtherApp"));
    QCOMPARE(fileExts.value(".png/UserChoice/Hash").toString(), QStringLiteral("protected-hash"));
}

void MediaServiceTest::extensionHandlerReporting()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QSettings classes(root.filePath(QStringLiteral("classes.ini")), QSettings::IniFormat);
    QSettings fileExts(root.filePath(QStringLiteral("fileexts.ini")), QSettings::IniFormat);
    MediaService service(this, &classes, &fileExts);

    // 未关联时：progId 为空、名称回退为“其他程序”。
    const auto none = service.extensionHandler(QStringLiteral(".png"));
    QCOMPARE(none.value(QStringLiteral("progId")).toString(), QString());
    QCOMPARE(none.value(QStringLiteral("ours")).toBool(), false);
    QCOMPARE(none.value(QStringLiteral("name")).toString(), QStringLiteral("其他程序"));

    // 开启后：生效 ProgId 是本程序，名称取自 FriendlyTypeName。
    QVERIFY(ok(service.prepareDefaultApplications({"png"})));
    fileExts.setValue(".png/UserChoice/ProgId", kImageProgId);
    const auto ours = service.extensionHandler(QStringLiteral(".png"));
    QCOMPARE(ours.value(QStringLiteral("progId")).toString(), kImageProgId);
    QCOMPARE(ours.value(QStringLiteral("ours")).toBool(), true);
    QCOMPARE(ours.value(QStringLiteral("name")).toString(), QStringLiteral("DesktopTool 图像浏览器"));

    // 从“选择其他应用”直接选择工具 exe 时，也正确识别为本工具。
    fileExts.setValue(".jpg/UserChoice/ProgId", "Applications\\DesktopToolImageBrowser.exe");
    QVERIFY(service.extensionHandler(QStringLiteral(".jpg")).value("ours").toBool());

    // 其他程序的 UserChoice 优先于用户类默认值，名称尽力解析。
    classes.setValue(QStringLiteral("OtherViewer"), QStringLiteral("别的看图软件"));
    fileExts.setValue(QStringLiteral(".webp/UserChoice/ProgId"), QStringLiteral("OtherViewer"));
    const auto foreign = service.extensionHandler(QStringLiteral(".webp"));
    QCOMPARE(foreign.value(QStringLiteral("progId")).toString(), QStringLiteral("OtherViewer"));
    QCOMPARE(foreign.value(QStringLiteral("ours")).toBool(), false);
    QCOMPARE(foreign.value(QStringLiteral("name")).toString(), QStringLiteral("别的看图软件"));

    // AppX 处理程序无可读类名，用统一说明代替长串标识符。
    fileExts.setValue(QStringLiteral(".gif/UserChoice/ProgId"),
        QStringLiteral("AppXqj98qxeaynz6dv4459ayz6bnqxbyaqcs"));
    const auto appx = service.extensionHandler(QStringLiteral(".gif"));
    QCOMPARE(appx.value(QStringLiteral("name")).toString(), QStringLiteral("Windows 内置或商店应用"));
}

void MediaServiceTest::unsupportedEnvironment()
{
    // 无注册表存储（非 Windows 或关闭原生集成）时，状态不可用且设置报错。
    MediaService service(false, this);
    const auto status = service.associationStatus();
    QVERIFY(ok(status));
    QCOMPARE(data(status).value("supported").toBool(), false);
    QCOMPARE(data(status).value("imageDefault").toBool(), false);
    QVERIFY(!ok(service.setImageBrowserDefault(true)));
    QVERIFY(!ok(service.setVideoPlayerDefault(true)));
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("DesktopToolMediaTest"));
    MediaServiceTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "MediaServiceTest.moc"
