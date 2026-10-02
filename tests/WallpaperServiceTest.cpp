#include "services/WallpaperService.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

namespace {
// 提取服务的成功状态。
bool ok(const QVariantMap& result) { return result.value("ok").toBool(); }
// 提取服务返回的数据对象。
QVariantMap data(const QVariantMap& result) { return result.value("data").toMap(); }
// 获取当前历史记录列表。
QVariantList items(const WallpaperService& service) { return data(service.snapshot()).value("items").toList(); }
// 在临时目录生成一张 PNG 图片，失败时返回空路径。
QString makeImage(const QDir& directory, const QString& name)
{
    QImage image(64, 36, QImage::Format_RGB32);
    image.fill(QColor("#f2b04e"));
    const QString path = directory.filePath(name);
    if (!image.save(path, "PNG"))
        return QString();
    return path;
}
// 生成一个扩展名为视频的占位文件；类型判定只依赖扩展名，不校验内容。
QString makeVideo(const QDir& directory, const QString& name)
{
    QFile file(directory.filePath(name));
    if (!file.open(QIODevice::WriteOnly))
        return QString();
    file.write("fake video bytes");
    file.close();
    return file.fileName();
}
// 构造含负坐标副屏的模拟拓扑，不改变计算机显示设置。
QVariantList twoScreens()
{
    return {QVariantMap{{"id", "monitor-a"}, {"name", "DISPLAY1"}, {"number", 1},
                {"primary", true}, {"x", 0}, {"y", 0}, {"width", 3840}, {"height", 2160}},
        QVariantMap{{"id", "monitor-b"}, {"name", "DISPLAY2"}, {"number", 2},
                {"primary", false}, {"x", -1920}, {"y", -200}, {"width", 1920}, {"height", 1080}}};
}
// 按稳定屏幕标识读取当前壁纸，避免测试依赖枚举顺序。
QString assigned(const WallpaperService& service, const QString& screenId)
{
    for (const QVariant& value : data(service.snapshot()).value("screens").toList()) {
        const auto screen = value.toMap();
        if (screen.value("id").toString() == screenId)
            return screen.value("activeId").toString();
    }
    return {};
}
}

// 覆盖添加去重、开关持久化、资源丢失提示、删除和损坏索引保护。
class WallpaperServiceTest final : public QObject
{
    Q_OBJECT
private slots:
    // 添加生成记录并设为当前，同一路径去重，非法文件被拒绝。
    void addAndUse();
    // 开启状态持久化，切换使用更新当前标识。
    void enableAndReload();
    // 原文件丢失时使用与开启都返回明确错误，记录保留。
    void missingResourceReports();
    // 删除当前记录停止显示，删除其他记录不影响显示。
    void removeRecords();
    // 损坏索引拒绝读取与写入，原文件保持不变。
    void corruptionProtection();
    // 两屏独立配置、重载、清除与删除，不串屏。
    void independentScreens();
    // 拔屏保留设置，重连恢复，断开屏幕的异步写入被拒绝。
    void reconnectScreens();
    // 旧索引兼容迁移以及写入失败时保留此前配置。
    void migrationAndWriteFailure();
    // 显示方式按屏幕保存、旧配置默认为填充，非法值及保存失败不改变状态。
    void displayModes();
};

void WallpaperServiceTest::addAndUse()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QDir media(directory.path());
    const QString png = makeImage(media, QStringLiteral("海边日落.png"));
    const QString mp4 = makeVideo(media, QStringLiteral("雨夜霓虹.mp4"));
    QVERIFY2(!png.isEmpty() && !mp4.isEmpty(), "生成测试媒体失败");
    QFile document(media.filePath("说明.txt"));
    QVERIFY(document.open(QIODevice::WriteOnly));
    document.write("not media");
    document.close();
    WallpaperService service(directory.path(), false);
    const auto added = data(service.add({{"path", png}}));
    QVERIFY2(ok(service.snapshot()), qPrintable(service.snapshot().value("error").toString()));
    const auto first = items(service).first().toMap();
    QCOMPARE(first.value("id").toString(), added.value("id").toString());
    QCOMPARE(first.value("name").toString(), QStringLiteral("海边日落.png"));
    QCOMPARE(first.value("type").toString(), QStringLiteral("image"));
    QCOMPARE(first.value("animated").toBool(), false);
    QCOMPARE(first.value("exists").toBool(), true);
    QVERIFY(first.value("thumbnail").toString().startsWith("data:image/png;base64,"));
    QCOMPARE(data(service.snapshot()).value("activeId").toString(), added.value("id").toString());
    QCOMPARE(data(service.snapshot()).value("enabled").toBool(), false);
    // 同一路径重复添加时切换使用，不产生重复记录。
    QVERIFY(ok(service.add({{"path", png}})));
    QCOMPARE(items(service).size(), 1);
    // 视频按扩展名识别，不生成缩略图；再次添加切换当前标识。
    const auto video = data(service.add({{"path", mp4}}));
    QVERIFY(ok(service.snapshot()));
    QCOMPARE(items(service).size(), 2);
    QCOMPARE(video.value("type").toString(), QStringLiteral("video"));
    QCOMPARE(video.value("animated").toBool(), false);
    QCOMPARE(video.value("thumbnail").toString(), QString());
    QCOMPARE(data(service.snapshot()).value("activeId").toString(), video.value("id").toString());
    // 非媒体文件与不存在的路径都会被拒绝。
    QVERIFY(!ok(service.add({{"path", document.fileName()}})));
    QVERIFY(!ok(service.add({{"path", media.filePath("missing.png")}})));
    QVERIFY(!ok(service.add({})));
    QCOMPARE(items(service).size(), 2); // 失败的添加不产生半成品记录。
}

void WallpaperServiceTest::enableAndReload()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QDir media(directory.path());
    const QString png = makeImage(media, QStringLiteral("海边日落.png"));
    QVERIFY2(!png.isEmpty(), "生成测试图片失败");
    WallpaperService service(directory.path(), false);
    const auto added = data(service.add({{"path", png}}));
    QVERIFY(ok(service.setEnabled(true)));
    QCOMPARE(data(service.snapshot()).value("enabled").toBool(), true);
    // 重启后开关与记录完整恢复。
    WallpaperService reload(directory.path(), false);
    QCOMPARE(data(reload.snapshot()).value("enabled").toBool(), true);
    QCOMPARE(data(reload.snapshot()).value("activeId").toString(), added.value("id").toString());
    QCOMPARE(items(reload).size(), 1);
    // 再添加一条视频并切换使用，当前标识随之更新。
    const QString mp4 = makeVideo(media, QStringLiteral("雨夜霓虹.mp4"));
    QVERIFY2(!mp4.isEmpty(), "生成测试视频失败");
    const auto video = data(reload.add({{"path", mp4}}));
    QCOMPARE(data(reload.snapshot()).value("activeId").toString(), video.value("id").toString());
    QVERIFY(ok(reload.use(added.value("id").toString())));
    QCOMPARE(data(reload.snapshot()).value("activeId").toString(), added.value("id").toString());
    QVERIFY(!reload.use(QStringLiteral("missing")).value("ok").toBool());
    QVERIFY(ok(reload.setEnabled(false)));
    QCOMPARE(data(reload.snapshot()).value("enabled").toBool(), false);
}

void WallpaperServiceTest::missingResourceReports()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QDir media(directory.path());
    const QString png = makeImage(media, QStringLiteral("海边日落.png"));
    QVERIFY2(!png.isEmpty(), "生成测试图片失败");
    WallpaperService service(directory.path(), false);
    const auto added = data(service.add({{"path", png}}));
    const QString id = added.value("id").toString();
    QVERIFY(QFile::remove(png));
    // 快照标记资源丢失，原记录保留。
    QCOMPARE(items(service).first().toMap().value("exists").toBool(), false);
    QCOMPARE(items(service).size(), 1);
    // 使用丢失的资源返回中文错误，当前标识保持不变。
    const auto used = service.use(id);
    QVERIFY(!ok(used));
    QVERIFY(used.value("error").toString().contains(QStringLiteral("丢失")));
    QCOMPARE(data(service.snapshot()).value("activeId").toString(), id);
    // 资源丢失时不能开启动态壁纸。
    const auto enabled = service.setEnabled(true);
    QVERIFY(!ok(enabled));
    QVERIFY(enabled.value("error").toString().contains(QStringLiteral("丢失")));
    QCOMPARE(data(service.snapshot()).value("enabled").toBool(), false);
}

void WallpaperServiceTest::removeRecords()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QDir media(directory.path());
    const QString png = makeImage(media, QStringLiteral("海边日落.png"));
    const QString mp4 = makeVideo(media, QStringLiteral("雨夜霓虹.mp4"));
    QVERIFY2(!png.isEmpty() && !mp4.isEmpty(), "生成测试媒体失败");
    WallpaperService service(directory.path(), false);
    const auto image = data(service.add({{"path", png}}));
    const auto video = data(service.add({{"path", mp4}}));
    QVERIFY(ok(service.use(image.value("id").toString())));
    QVERIFY(!ok(service.remove(QStringList{QStringLiteral("missing")})));
    QCOMPARE(items(service).size(), 2); // 任一标识不存在时不删除任何条目。
    // 删除非当前记录不影响当前显示。
    QVERIFY(ok(service.remove({video.value("id").toString()})));
    QCOMPARE(items(service).size(), 1);
    QCOMPARE(data(service.snapshot()).value("activeId").toString(), image.value("id").toString());
    // 删除当前记录后回到未选择状态，开关保持原样。
    QVERIFY(ok(service.setEnabled(true)));
    QVERIFY(ok(service.remove({image.value("id").toString()})));
    QCOMPARE(items(service).size(), 0);
    QCOMPARE(data(service.snapshot()).value("activeId").toString(), QString());
    QCOMPARE(data(service.snapshot()).value("enabled").toBool(), true);
}

void WallpaperServiceTest::corruptionProtection()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QDir media(directory.path());
    const QString png = makeImage(media, QStringLiteral("海边日落.png"));
    QVERIFY2(!png.isEmpty(), "生成测试图片失败");
    WallpaperService service(directory.path(), false);
    QVERIFY(ok(service.add({{"path", png}})));
    const QString indexPath = directory.filePath("wallpaper/wallpaper-history.v1.json");
    QFile corrupt(indexPath);
    QVERIFY(corrupt.open(QIODevice::WriteOnly));
    corrupt.write("{broken");
    corrupt.close();
    WallpaperService broken(directory.path(), false);
    QVERIFY(!ok(broken.snapshot()));
    QVERIFY(!ok(broken.add({{"path", png}})));
    QVERIFY(!ok(broken.setEnabled(true)));
    QVERIFY(corrupt.open(QIODevice::ReadOnly));
    QCOMPARE(corrupt.readAll(), QByteArray("{broken"));
    QVERIFY(media.exists(QStringLiteral("海边日落.png")));
}

void WallpaperServiceTest::independentScreens()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    WallpaperService service(directory.path(), false, nullptr, twoScreens);
    const QString first = data(service.add({{"path", makeImage(QDir(directory.path()), "a.png")},
        {"screenId", "monitor-a"}})).value("id").toString();
    const QString second = data(service.add({{"path", makeImage(QDir(directory.path()), "b.png")},
        {"screenId", "monitor-b"}})).value("id").toString();
    QVERIFY(!first.isEmpty() && !second.isEmpty());
    QCOMPARE(assigned(service, "monitor-a"), first);
    QCOMPARE(assigned(service, "monitor-b"), second);
    QVERIFY(ok(service.setEnabled(true)));
    QVERIFY(ok(service.setEnabled(false)));
    WallpaperService reload(directory.path(), false, nullptr, twoScreens);
    QCOMPARE(assigned(reload, "monitor-a"), first);
    QCOMPARE(assigned(reload, "monitor-b"), second);
    QVERIFY(ok(reload.clearScreen("monitor-a")));
    QVERIFY(assigned(reload, "monitor-a").isEmpty());
    QCOMPARE(assigned(reload, "monitor-b"), second);
    QVERIFY(ok(reload.use(first, "monitor-a")));
    QVERIFY(ok(reload.remove({second})));
    QCOMPARE(assigned(reload, "monitor-a"), first);
    QVERIFY(assigned(reload, "monitor-b").isEmpty());
    QVERIFY(ok(reload.use(first, "monitor-b")));
    QVERIFY(ok(reload.remove({first})));
    QVERIFY(assigned(reload, "monitor-a").isEmpty());
    QVERIFY(assigned(reload, "monitor-b").isEmpty());
}

void WallpaperServiceTest::reconnectScreens()
{
    QTemporaryDir directory;
    QVariantList screens = twoScreens();
    WallpaperService service(directory.path(), false, nullptr, [&screens] { return screens; });
    const QString id = data(service.add({{"path", makeImage(QDir(directory.path()), "a.png")},
        {"screenId", "monitor-b"}})).value("id").toString();
    QVERIFY(!id.isEmpty());
    screens.removeLast();
    QVERIFY(!ok(service.use(id, "monitor-b")));
    QCOMPARE(data(service.snapshot()).value("screens").toList().size(), 1);
    QVERIFY(assigned(service, "monitor-a").isEmpty());
    QVERIFY(ok(service.setEnabled(true)));
    screens = twoScreens();
    std::reverse(screens.begin(), screens.end());
    QVERIFY(ok(service.setEnabled(false)));
    QCOMPARE(assigned(service, "monitor-b"), id);
    QVERIFY(assigned(service, "monitor-a").isEmpty());
}

void WallpaperServiceTest::migrationAndWriteFailure()
{
    QTemporaryDir directory;
    WallpaperService service(directory.path(), false, nullptr, twoScreens);
    const QString id = data(service.add({{"path", makeImage(QDir(directory.path()), "old.png")}})).value("id").toString();
    const QString indexPath = directory.filePath("wallpaper/wallpaper-history.v1.json");
    QFile file(indexPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto legacy = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    legacy.insert("version", 1);
    legacy.insert("activeId", id);
    legacy.remove("screens");
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(legacy).toJson());
    file.close();
    WallpaperService migrated(directory.path(), false, nullptr, twoScreens);
    QCOMPARE(assigned(migrated, "monitor-a"), id);
    QCOMPARE(assigned(migrated, "monitor-b"), id);
    QVERIFY(ok(migrated.clearScreen("monitor-b")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(file.readAll()).object().value("version").toInt(), 2);
    file.close();
    QVERIFY(file.rename(indexPath + ".saved"));
    QVERIFY(QDir().mkdir(indexPath));
    QVERIFY(!ok(migrated.clearScreen("monitor-a")));
    QVERIFY(!ok(migrated.setEnabled(true)));
    QCOMPARE(assigned(migrated, "monitor-a"), id);
    QVERIFY(!data(migrated.snapshot()).value("enabled").toBool());
}

void WallpaperServiceTest::displayModes()
{
    QTemporaryDir directory;
    WallpaperService service(directory.path(), false, nullptr, twoScreens);
    const auto added = data(service.add({{"path", makeImage(QDir(directory.path()), "image.png")}}));
    QCOMPARE(added.value("width").toInt(), 64);
    QCOMPARE(added.value("height").toInt(), 36);
    QCOMPARE(data(service.snapshot()).value("screens").toList().first().toMap().value("displayMode").toString(), QStringLiteral("fill"));
    QVERIFY(ok(service.setDisplayMode("monitor-a", "tile")));
    QVERIFY(ok(service.setDisplayMode("monitor-b", "center")));
    QVERIFY(!ok(service.setDisplayMode("missing", "fit")));
    QVERIFY(!ok(service.setDisplayMode("monitor-a", "bogus")));
    WallpaperService reload(directory.path(), false, nullptr, twoScreens);
    const auto screens = data(reload.snapshot()).value("screens").toList();
    QCOMPARE(screens.first().toMap().value("displayMode").toString(), QStringLiteral("tile"));
    QCOMPARE(screens.last().toMap().value("displayMode").toString(), QStringLiteral("center"));
    const QString indexPath = directory.filePath("wallpaper/wallpaper-history.v1.json");
    QVERIFY(QFile::rename(indexPath, indexPath + ".saved"));
    QVERIFY(QDir().mkdir(indexPath));
    QVERIFY(!ok(reload.setDisplayMode("monitor-a", "stretch")));
    QCOMPARE(data(reload.snapshot()).value("screens").toList(), screens);
}

QTEST_MAIN(WallpaperServiceTest)
#include "WallpaperServiceTest.moc"
