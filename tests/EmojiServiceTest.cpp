#include "services/ClipboardService.h"
#include "services/EmojiService.h"

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

namespace {
// 提取服务的成功状态。
bool ok(const QVariantMap& result) { return result.value("ok").toBool(); }
// 提取服务返回的数据对象。
QVariantMap data(const QVariantMap& result) { return result.value("data").toMap(); }
// 获取当前表情摘要列表。
QVariantList items(const EmojiService& service) { return data(service.snapshot()).value("items").toList(); }
// 获取剪贴板历史摘要列表。
QVariantList clipboardItems(const ClipboardService& service) { return data(service.snapshot()).value("items").toList(); }
// 获取分类数量。
int categoryCount(const EmojiService& service) { return data(service.snapshot()).value("categories").toList().size(); }
// 在临时目录生成一张 PNG 图片，失败时返回空路径。
QString makeImage(const QDir& directory, const QString& name, const QColor& color)
{
    QImage image(48, 36, QImage::Format_RGB32);
    image.fill(color);
    const QString path = directory.filePath(name);
    if (!image.save(path, "PNG"))
        return QString();
    return path;
}
}

// 覆盖添加校验、关键字检索、分类迁移、复制屏蔽和损坏索引保护。
class EmojiServiceTest final : public QObject
{
    Q_OBJECT
private slots:
    // 添加图片生成缩略图，关键字和文件名都可以检索，重复添加被拒绝。
    void addAndSearch();
    // 关键字与分类可以修改，分类新增、重命名和删除迁移符合约定。
    void editAndCategories();
    // 删除仅移除记录，重启恢复，索引损坏时拒绝写入。
    void removeReloadAndProtection();
    // 复制文件进入剪贴板但不记录历史，之后的外部复制仍然正常记录。
    void copyFileSkipsClipboardHistory();
    // 原文件丢失时打开目录与复制都返回明确错误。
    void missingFileReports();
};

void EmojiServiceTest::addAndSearch()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QDir images(directory.path());
    QVERIFY(images.mkpath("stickers"));
    images.cd("stickers");
    const QString png = makeImage(images, QStringLiteral("笑脸.png"), QColor("#f2b04e"));
    const QString jpg = makeImage(images, QStringLiteral("蓝星.jpg"), QColor("#6385ee"));
    QVERIFY2(!png.isEmpty() && !jpg.isEmpty(), "生成测试图片失败");
    QFile document(images.filePath("说明.txt"));
    QVERIFY(document.open(QIODevice::WriteOnly));
    document.write("not an image");
    document.close();
    EmojiService service(directory.path());
    QCOMPARE(categoryCount(service), 1);
    const auto addedFirst = service.add({{"path", png}, {"categoryId", "default"}, {"keywords", QVariantList{QStringLiteral("开心"), QStringLiteral("表情")}}});
    QVERIFY2(ok(addedFirst), qPrintable(addedFirst.value("error").toString()));
    QVERIFY(ok(service.add({{"path", jpg}, {"categoryId", "default"}, {"keywords", QVariantList{QStringLiteral("星星")}}})));
    QCOMPARE(items(service).size(), 2);
    const auto first = items(service).first().toMap();
    QCOMPARE(first.value("name").toString(), QStringLiteral("蓝星.jpg")); // 最近添加的排在前面。
    QVERIFY(first.value("thumbnail").toString().startsWith("data:image/png;base64,"));
    QCOMPARE(first.value("width").toInt(), 48);
    QCOMPARE(first.value("height").toInt(), 36);
    QCOMPARE(first.value("keywords").toStringList(), QStringList{QStringLiteral("星星")});
    QVERIFY(!first.value("path").toString().isEmpty());
    // 关键字、文件名检索都不区分大小写与全半角顺序。
    QCOMPARE(data(service.snapshot(QStringLiteral("开心"))).value("items").toList().size(), 1);
    QCOMPARE(data(service.snapshot(QStringLiteral("星"))).value("items").toList().size(), 1);
    QCOMPARE(data(service.snapshot(QStringLiteral("笑脸"))).value("items").toList().size(), 1);
    QCOMPARE(data(service.snapshot(QStringLiteral("没有"))).value("items").toList().size(), 0);
    // 非图片文件、不存在的路径和未知分类都会被拒绝。
    QVERIFY(!ok(service.add({{"path", document.fileName()}, {"categoryId", "default"}, {"keywords", QVariantList{"文本"}}})));
    QVERIFY(!ok(service.add({{"path", images.filePath("missing.png")}, {"categoryId", "default"}, {"keywords", QVariantList{"缺失"}}})));
    QVERIFY(!ok(service.add({{"path", png}, {"categoryId", "unknown"}, {"keywords", QVariantList{"未知"}}})));
    QVERIFY(!ok(service.add({{"path", png}, {"categoryId", "default"}, {"keywords", QVariantList{QStringLiteral("开心")}}})));
    QCOMPARE(items(service).size(), 2); // 失败的添加不产生半成品记录。
    // 关键字格式校验：超过 20 个或单个超长都会拒绝。
    QVariantList tooMany;
    for (int index = 0; index < 21; ++index)
        tooMany.append(QString::number(index));
    QVERIFY(!ok(service.add({{"path", png}, {"categoryId", "default"}, {"keywords", tooMany}})));
    QVERIFY(!ok(service.add({{"path", png}, {"categoryId", "default"},
        {"keywords", QVariantList{QString(51, QLatin1Char('a'))}}})));
}

void EmojiServiceTest::editAndCategories()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QDir images(directory.path());
    const QString png = makeImage(images, QStringLiteral("笑脸.png"), QColor("#f2b04e"));
    QVERIFY2(!png.isEmpty(), "生成测试图片失败");
    EmojiService service(directory.path());
    const auto added = data(service.add({{"path", png}, {"categoryId", "default"}, {"keywords", QVariantList{QStringLiteral("旧关键字")}}}));
    QVERIFY(!added.value("id").toString().isEmpty());
    // 新建分类并移动表情，关键字同步更新。
    const auto category = data(service.saveCategory(QString(), QStringLiteral("常用")));
    QVERIFY(!category.value("id").toString().isEmpty());
    QVERIFY(ok(service.save({{"id", added.value("id")}, {"categoryId", category.value("id")},
        {"keywords", QVariantList{QStringLiteral("新关键字"), QStringLiteral("开心")}}})));
    QCOMPARE(data(service.snapshot(QStringLiteral("新关键字"))).value("items").toList().size(), 1);
    QCOMPARE(data(service.snapshot(QStringLiteral("旧关键字"))).value("items").toList().size(), 0);
    QVERIFY(!ok(service.save({{"id", added.value("id")}, {"categoryId", "missing"}, {"keywords", QVariantList{"关键字"}}})));
    QVERIFY(!ok(service.save({{"id", "missing"}, {"categoryId", "default"}, {"keywords", QVariantList{"关键字"}}})));
    // 分类重命名与同名冲突。
    QVERIFY(ok(service.saveCategory(category.value("id").toString(), QStringLiteral("收藏"))));
    QCOMPARE(data(service.snapshot()).value("categories").toList().at(1).toMap().value("name").toString(), QStringLiteral("收藏"));
    QVERIFY(!ok(service.saveCategory(QString(), QStringLiteral("默认分类"))));
    // 删除分类把表情移回默认分类。
    QVERIFY(ok(service.removeCategory(category.value("id").toString())));
    QCOMPARE(items(service).first().toMap().value("categoryId").toString(), QString("default"));
    QVERIFY(!ok(service.removeCategory(QStringLiteral("default"))));
    QVERIFY(!ok(service.removeCategory(QStringLiteral("missing"))));
    QCOMPARE(categoryCount(service), 1);
}

void EmojiServiceTest::removeReloadAndProtection()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QDir images(directory.path());
    const QString png = makeImage(images, QStringLiteral("笑脸.png"), QColor("#f2b04e"));
    const QString jpg = makeImage(images, QStringLiteral("蓝星.png"), QColor("#6385ee"));
    QVERIFY2(!png.isEmpty() && !jpg.isEmpty(), "生成测试图片失败");
    EmojiService service(directory.path());
    QVERIFY(ok(service.add({{"path", png}, {"categoryId", "default"}, {"keywords", QVariantList{"笑脸"}}})));
    QVERIFY(ok(service.add({{"path", jpg}, {"categoryId", "default"}, {"keywords", QVariantList{"蓝星"}}})));
    QVERIFY(!ok(service.remove(QStringList{QStringLiteral("missing")})));
    QCOMPARE(items(service).size(), 2); // 任一标识不存在时不删除任何条目。
    QVERIFY(ok(service.remove({items(service).first().toMap().value("id").toString()})));
    QCOMPARE(items(service).size(), 1);
    // 重启后数据完整恢复。
    EmojiService reload(directory.path());
    QCOMPARE(items(reload).size(), 1);
    QCOMPARE(items(reload).first().toMap().value("keywords").toStringList(), QStringList{QStringLiteral("笑脸")});
    QVERIFY(reload.snapshot().value("ok").toBool());
    // 损坏索引拒绝读取与写入，原文件保持不变。
    const QString indexPath = directory.filePath("emoji/emoji-library.v1.json");
    QFile corrupt(indexPath);
    QVERIFY(corrupt.open(QIODevice::WriteOnly));
    corrupt.write("{broken");
    corrupt.close();
    EmojiService broken(directory.path());
    QVERIFY(!ok(broken.snapshot()));
    QVERIFY(!ok(broken.add({{"path", png}, {"categoryId", "default"}, {"keywords", QVariantList{"笑脸"}}})));
    QVERIFY(!ok(broken.remove(QStringList{items(reload).first().toMap().value("id").toString()})));
    QVERIFY(corrupt.open(QIODevice::ReadOnly));
    QCOMPARE(corrupt.readAll(), QByteArray("{broken"));
    QVERIFY(images.exists(QStringLiteral("笑脸.png")));
}

void EmojiServiceTest::copyFileSkipsClipboardHistory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QDir images(directory.path());
    const QString png = makeImage(images, QStringLiteral("笑脸.png"), QColor("#f2b04e"));
    QVERIFY2(!png.isEmpty(), "生成测试图片失败");
    ClipboardService clipboard(directory.path(), true);
    QVERIFY(ok(clipboard.setTypes({"files", "image", "text"})));
    EmojiService service(directory.path(), &clipboard);
    const auto added = data(service.add({{"path", png}, {"categoryId", "default"}, {"keywords", QVariantList{"笑脸"}}}));
    // 复制文件同时提供文件引用与图像内容，且不产生历史记录。
    QVERIFY(ok(service.copyFile(added.value("id").toString())));
    auto* mime = QGuiApplication::clipboard()->mimeData();
    QVERIFY(mime);
    QVERIFY(mime->hasUrls());
    QCOMPARE(mime->urls().first(), QUrl::fromLocalFile(QDir::toNativeSeparators(png)));
    QVERIFY(mime->hasImage());
    QCOMPARE(mime->imageData().value<QImage>().size(), QSize(48, 36));
    QTest::qWait(120);
    QCOMPARE(clipboardItems(clipboard).size(), 0);
    // 同一服务的文本复制依旧会记录，屏蔽只作用于表情复制本身。
    QMimeData text;
    text.setText(QStringLiteral("正常记录"));
    QVERIFY(ok(clipboard.capture(&text)));
    QCOMPARE(clipboardItems(clipboard).size(), 1);
    // 没有剪贴板服务时返回中文错误，不影响其他操作。
    EmojiService standalone(directory.path());
    QVERIFY(!ok(standalone.copyFile(added.value("id").toString())));
}

void EmojiServiceTest::missingFileReports()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QDir images(directory.path());
    const QString png = makeImage(images, QStringLiteral("笑脸.png"), QColor("#f2b04e"));
    QVERIFY2(!png.isEmpty(), "生成测试图片失败");
    ClipboardService clipboard(directory.path(), false);
    EmojiService service(directory.path(), &clipboard);
    const auto added = data(service.add({{"path", png}, {"categoryId", "default"}, {"keywords", QVariantList{"笑脸"}}}));
    const QString id = added.value("id").toString();
    QVERIFY(QFile::remove(png));
    QVERIFY(!ok(service.openDirectory(id)));
    QVERIFY(!ok(service.copyFile(id)));
    QCOMPARE(items(service).size(), 1); // 原文件丢失不自动清理记录。
    // 缩略图仍随索引显示，原路径可以查看。
    QCOMPARE(items(service).first().toMap().value("path").toString(),
        QDir::toNativeSeparators(QFileInfo(png).absoluteFilePath()));
}

QTEST_MAIN(EmojiServiceTest)
#include "EmojiServiceTest.moc"
