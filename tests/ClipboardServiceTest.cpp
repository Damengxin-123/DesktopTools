#include "services/ClipboardService.h"
#include <QClipboard>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QUrl>
#include <QtTest>

namespace {
// 提取服务的成功状态。
bool ok(const QVariantMap& result) { return result.value("ok").toBool(); }
// 提取服务返回的数据对象。
QVariantMap data(const QVariantMap& result) { return result.value("data").toMap(); }
// 获取当前历史摘要。
QVariantList items(const ClipboardService& service) { return data(service.snapshot()).value("items").toList(); }
// 获取最近一次记录标识。
QString firstId(const ClipboardService& service) { return items(service).first().toMap().value("id").toString(); }
}

// 覆盖自动监听、内容分类、重启恢复、容量和磁盘故障的数据保护。
class ClipboardServiceTest final : public QObject
{
    Q_OBJECT
private slots:
    // 未勾选时不记录；完整文本搜索、重复复制、暂停和重启均符合设置。
    void filteringAndReload();
    // 同一正文的格式补充与重入只记一次，不同内容、间隔复制和管理操作仍正常记录。
    void duplicateNotifications();
    // 图片及混合文件按整批内容去重，专有格式不凭格式名误判相同内容。
    void duplicateMediaBatches();
    // 图片保留像素，多项普通文件引用不会因附带预览图而丢失。
    void imagesAndFileReferences();
    // 图像文件、聊天缓存和复合图像格式正确分类，混合复制分别遵循类型勾选。
    void imageFileClassification();
    // 验证系统事件监听和再次复制不生成循环记录。
    void liveClipboardAndRestore();
    // 原子写入失败与损坏历史不修改既有数据。
    void failureProtection();
    // 超限仅移除最早未置顶记录，删除和清空保持设置。
    void capacityAndManagement();
    // 剪贴板资源转换优先使用文件引用，图像内容按唯一名称保存。
    void pasteResourceFromFileAndImage();
    // 无内容、多文件与失效引用返回明确错误。
    void pasteResourceRejectsInvalidClipboard();
};

void ClipboardServiceTest::filteringAndReload()
{
    QTemporaryDir directory;
    ClipboardService service(directory.path(), false);
    QMimeData text;
    text.setText(QString(700, QLatin1Char('a')) + QStringLiteral("完整文本关键词"));
    QVERIFY(ok(service.capture(&text)));
    QVERIFY(items(service).isEmpty());
    QVERIFY(ok(service.setTypes({"text"})));
    QVERIFY(!ok(service.setTypes({"invalid"})));
    QVERIFY(ok(service.capture(&text)));
    QVERIFY(ok(service.capture(&text)));
    QCOMPARE(items(service).size(), 1);
    QCOMPARE(data(service.snapshot(QStringLiteral("完整文本关键词"))).value("items").toList().size(), 1);
    QCOMPARE(data(service.read(firstId(service))).value("text").toString(), text.text());
    QVERIFY(items(service).first().toMap().value("preview").toString().size() <= 600);
    ClipboardService reload(directory.path(), false);
    QCOMPARE(items(reload).size(), 1);
    QCOMPARE(data(reload.snapshot()).value("types").toStringList(), QStringList{"text"});
    QVERIFY(ok(reload.capture(&text))); // 重启后的主动复制不能被上一进程的记录压制。
    QCOMPARE(items(reload).size(), 2);
    QVERIFY(ok(service.setTypes({})));
    QVERIFY(ok(service.capture(&text)));
    QCOMPARE(items(service).size(), 1);
    QVERIFY(ok(service.setTypes({"text"})));
    text.setText(QString(2 * 1024 * 1024 + 1, QLatin1Char('x')));
    QVERIFY(!ok(service.capture(&text)));
    QCOMPARE(items(service).size(), 1);
    QVERIFY(!data(service.snapshot()).value("notice").toString().isEmpty());
}

void ClipboardServiceTest::duplicateNotifications()
{
    QTemporaryDir directory;
    ClipboardService service(directory.path(), false);
    QVERIFY(ok(service.setTypes({"text"})));
    QMimeData first;
    first.setText(QStringLiteral("甲：你好\r\n乙：收到"));
    bool reentered = false;
    // 模拟读取或保存过程中嵌套送达通知，避免递归保存同一条记录。
    connect(&service, &ClipboardService::changed, &service, [&]() {
        if (!reentered) {
            reentered = true;
            QVERIFY(ok(service.capture(&first)));
        }
    });
    QSignalSpy changes(&service, &ClipboardService::changed);
    QVERIFY(ok(service.capture(&first)));
    QVERIFY(reentered);
    QCOMPARE(items(service).size(), 1);
    const QString originalId = firstId(service);
    const auto original = data(service.read(originalId));
    QMimeData enriched;
    enriched.setHtml(QStringLiteral("<p>甲：你好</p><p>乙：收到</p>"));
    enriched.setText(QStringLiteral("甲：你好\n乙：收到"));
    enriched.setData("application/chat-format", "extra metadata");
    QVERIFY(ok(service.capture(&enriched)));
    QCOMPARE(items(service).size(), 1);
    QCOMPARE(changes.count(), 1);
    QCOMPARE(data(service.read(originalId)), original); // 重复通知不改原文、时间和标识。
    QMimeData htmlOnly;
    htmlOnly.setHtml(QStringLiteral("<p>甲：你好</p><p>乙：收到</p>"));
    QVERIFY(ok(service.capture(&htmlOnly)));
    QCOMPARE(items(service).size(), 1);

    QVERIFY(ok(service.pin(originalId, true)));
    QVERIFY(ok(service.capture(&enriched)));
    QCOMPARE(items(service).size(), 1);
    QTest::qWait(650);
    QVERIFY(ok(service.capture(&enriched)));
    QCOMPARE(items(service).size(), 1);
    QTest::qWait(450); // 中途重复通知不能把去重窗口继续向后延长。
    QVERIFY(ok(service.capture(&enriched)));
    QCOMPARE(items(service).size(), 2);
    QVERIFY(firstId(service) != originalId);

    // 快速连续复制不同内容，包含有意义的空白变化，都必须保存。
    QMimeData other;
    other.setText(QStringLiteral("不同内容"));
    QVERIFY(ok(service.capture(&other)));
    QVERIFY(ok(service.capture(&enriched)));
    QCOMPARE(items(service).size(), 4);
    enriched.setText(enriched.text() + QLatin1Char(' '));
    QVERIFY(ok(service.capture(&enriched)));
    QCOMPARE(items(service).size(), 5);
    QVERIFY(ok(service.remove({firstId(service)})));
    QVERIFY(ok(service.capture(&enriched)));
    QCOMPARE(items(service).size(), 5);
    QVERIFY(ok(service.clear()));
    QVERIFY(ok(service.capture(&enriched)));
    QCOMPARE(items(service).size(), 1);
    QVERIFY(ok(service.setTypes({})));
    QVERIFY(ok(service.setTypes({"text"})));
    QVERIFY(ok(service.capture(&enriched)));
    QCOMPARE(items(service).size(), 2);
}

void ClipboardServiceTest::duplicateMediaBatches()
{
    QTemporaryDir directory;
    ClipboardService service(directory.filePath("history"), false);
    QVERIFY(ok(service.setTypes({"image", "files", "other"})));
    QImage image(16, 12, QImage::Format_ARGB32);
    image.fill(Qt::green);
    QMimeData chat;
    chat.setImageData(image);
    QVERIFY(ok(service.capture(&chat)));
    chat.setUrls({QUrl::fromLocalFile(directory.filePath("chat-cache.tmp"))});
    QVERIFY(ok(service.capture(&chat)));
    QCOMPARE(items(service).size(), 1); // 补充缓存路径不应重复保存同一图片。
    image.fill(Qt::red);
    chat.setImageData(image);
    QVERIFY(ok(service.capture(&chat)));
    QCOMPARE(items(service).size(), 2);

    const QString path = directory.filePath("image.png");
    QVERIFY(image.save(path));
    QMimeData batch;
    batch.setUrls({QUrl::fromLocalFile(path), QUrl::fromLocalFile(directory.path())});
    QVERIFY(ok(service.capture(&batch)));
    QCOMPARE(items(service).size(), 4);
    QVERIFY(ok(service.capture(&batch)));
    QCOMPARE(items(service).size(), 4); // 整批图像和文件只合并重复事件，不丢失拆分条目。
    QMimeData unknown;
    unknown.setData("application/chat-private", "first");
    QVERIFY(ok(service.capture(&unknown)));
    unknown.setData("application/chat-private", "second");
    QVERIFY(ok(service.capture(&unknown)));
    QCOMPARE(items(service).size(), 6);
}

void ClipboardServiceTest::imagesAndFileReferences()
{
    QTemporaryDir directory;
    ClipboardService service(directory.filePath("history"), false);
    QVERIFY(ok(service.setTypes({"text", "image", "files", "links", "other"})));
    QImage original(32, 24, QImage::Format_ARGB32);
    original.fill(QColor(12, 34, 56, 120));
    QMimeData image;
    image.setImageData(original);
    image.setText("image fallback");
    QVERIFY(ok(service.capture(&image)));
    const auto saved = data(service.read(firstId(service)));
    QCOMPARE(saved.value("type").toString(), QString("image"));
    const auto decoded = QImage::fromData(QByteArray::fromBase64(saved.value("image").toString().section(',', 1).toLatin1()), "PNG");
    QCOMPARE(decoded, original);
    QVERIFY(!items(service).first().toMap().contains("image"));
    QFile source(directory.filePath(QStringLiteral("示例文件.txt")));
    QVERIFY(source.open(QIODevice::WriteOnly));
    source.write("original bytes");
    source.close();
    QMimeData files;
    files.setUrls({QUrl::fromLocalFile(source.fileName()), QUrl::fromLocalFile(directory.path())});
    files.setText(source.fileName());
    files.setImageData(original);
    QVERIFY(ok(service.capture(&files)));
    const QString fileId = firstId(service);
    const auto fileRecord = data(service.read(fileId));
    QCOMPARE(fileRecord.value("type").toString(), QString("files"));
    QCOMPARE(fileRecord.value("files").toList().size(), 2);
    QVERIFY(!fileRecord.contains("image"));
    QCOMPARE(QDir(directory.filePath("history")).entryList(QDir::Files).size(), 1);
    QVERIFY(ok(service.remove({fileId})));
    QVERIFY(source.open(QIODevice::ReadOnly));
    QCOMPARE(source.readAll(), QByteArray("original bytes"));
    source.close();
    QVERIFY(ok(service.setTypes({"text"})));
    QVERIFY(ok(service.capture(&files)));
    QCOMPARE(items(service).size(), 1); // 未勾选文件时，不把文件复制误存为文本。
    QVERIFY(ok(service.setTypes({"links", "other"})));
    QMimeData links;
    links.setUrls({QUrl("https://example.com/a")});
    QVERIFY(ok(service.capture(&links)));
    QCOMPARE(items(service).first().toMap().value("type").toString(), QString("links"));
    QMimeData unknown;
    unknown.setData("application/custom", "do not persist these bytes");
    QVERIFY(ok(service.capture(&unknown)));
    QVERIFY(!data(service.read(firstId(service))).value("text").toString().contains("do not persist"));
    QVERIFY(!ok(service.copy(firstId(service))));
    ClipboardService reload(directory.filePath("history"), false);
    QCOMPARE(items(reload).size(), 3);
}

void ClipboardServiceTest::imageFileClassification()
{
    QTemporaryDir directory;
    ClipboardService service(directory.filePath("history"), false);
    QImage original(32, 24, QImage::Format_ARGB32);
    original.fill(QColor(12, 34, 56, 120));
    const QString png = directory.filePath(QStringLiteral("复制图片.PNG"));
    const QString cache = directory.filePath("chat-cache.dat");
    QVERIFY(original.save(png, "PNG"));
    QVERIFY(original.save(cache, "PNG"));
    QFile document(directory.filePath("notes.txt"));
    QVERIFY(document.open(QIODevice::WriteOnly));
    document.write("keep this document");
    document.close();
    QMimeData paths;
    paths.setUrls({QUrl::fromLocalFile(png)});
    paths.setText(png);
    QVERIFY(ok(service.setTypes({"files", "text"})));
    QVERIFY(ok(service.capture(&paths)));
    QVERIFY(items(service).isEmpty()); // 未勾选图像时不能改归文件或文本。
    QVERIFY(ok(service.setTypes({"image"})));
    QVERIFY(ok(service.capture(&paths)));
    QCOMPARE(items(service).size(), 1);
    QCOMPARE(items(service).first().toMap().value("type").toString(), QString("image"));
    const QString imageId = firstId(service);
    QCOMPARE(data(service.snapshot(QStringLiteral("复制图片.PNG"))).value("items").toList().size(), 1);
    QVERIFY(QFile::remove(png));
    QVERIFY(ok(service.copy(imageId))); // 保存的是图片内容，原图删除后仍可再次复制。
    QCOMPARE(QGuiApplication::clipboard()->image(), original);
    paths.setUrls({QUrl::fromLocalFile(cache)});
    QVERIFY(ok(service.capture(&paths)));
    QCOMPARE(items(service).first().toMap().value("type").toString(), QString("image"));
    QVERIFY(ok(service.clear()));
    QMimeData chat;
    chat.setImageData(original);
    chat.setUrls({QUrl::fromLocalFile(directory.filePath("expired-chat-cache.tmp"))});
    chat.setText("chat image");
    QVERIFY(ok(service.capture(&chat)));
    QCOMPARE(items(service).size(), 1);
    QCOMPARE(items(service).first().toMap().value("type").toString(), QString("image"));
    QVERIFY(ok(service.setTypes({"files"})));
    QVERIFY(ok(service.capture(&chat)));
    QCOMPARE(items(service).size(), 1);
    QVERIFY(ok(service.clear()));
    paths.setUrls({QUrl::fromLocalFile(cache), QUrl::fromLocalFile(document.fileName())});
    QVERIFY(ok(service.capture(&paths)));
    QCOMPARE(items(service).size(), 1);
    auto fileRecord = data(service.read(firstId(service)));
    QCOMPARE(fileRecord.value("type").toString(), QString("files"));
    QCOMPARE(fileRecord.value("files").toList().size(), 1);
    QCOMPARE(fileRecord.value("files").toList().first().toMap().value("name").toString(), QString("notes.txt"));
    QVERIFY(ok(service.clear()));
    QVERIFY(ok(service.setTypes({"image"})));
    QVERIFY(ok(service.capture(&paths)));
    QCOMPARE(items(service).size(), 1);
    QCOMPARE(items(service).first().toMap().value("type").toString(), QString("image"));
    QVERIFY(ok(service.clear()));
    QVERIFY(ok(service.setTypes({"image", "files"})));
    QVERIFY(ok(service.capture(&paths)));
    QCOMPARE(items(service).size(), 2);
    QCOMPARE(items(service).at(0).toMap().value("type").toString(), QString("image"));
    QCOMPARE(items(service).at(1).toMap().value("type").toString(), QString("files"));
    QCOMPARE(items(service).at(0).toMap().value("copiedAt"), items(service).at(1).toMap().value("copiedAt"));
    QVERIFY(ok(service.clear()));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(original.save(&buffer, "PNG"));
    QMimeData raw;
    raw.setData("application/x-qt-windows-mime;value=\"PNG\"", bytes);
    raw.setUrls({QUrl::fromLocalFile(directory.filePath("missing.tmp"))});
    QVERIFY(ok(service.capture(&raw)));
    QCOMPARE(items(service).size(), 1);
    QCOMPARE(items(service).first().toMap().value("type").toString(), QString("image"));
    QVERIFY(ok(service.copy(firstId(service))));
    QCOMPARE(QGuiApplication::clipboard()->image(), original);
    QVERIFY(ok(service.clear()));
    QFile broken(directory.filePath("broken.png"));
    QVERIFY(broken.open(QIODevice::WriteOnly));
    broken.write("invalid image bytes");
    broken.close();
    paths.setUrls({QUrl::fromLocalFile(broken.fileName())});
    QVERIFY(!ok(service.capture(&paths)));
    QVERIFY(items(service).isEmpty());
    QVERIFY(!data(service.snapshot()).value("notice").toString().isEmpty());
    QVERIFY(ok(service.capture(&chat)));
    ClipboardService reload(directory.filePath("history"), false);
    QCOMPARE(items(reload).size(), 1);
    QVERIFY(ok(reload.copy(firstId(reload))));
    QCOMPARE(QGuiApplication::clipboard()->image(), original);
    QVERIFY(document.open(QIODevice::ReadOnly));
    QCOMPARE(document.readAll(), QByteArray("keep this document"));
}

void ClipboardServiceTest::liveClipboardAndRestore()
{
    QTemporaryDir directory;
    ClipboardService service(directory.path(), true);
    QVERIFY(ok(service.setTypes({"text", "image", "files"})));
    QGuiApplication::clipboard()->setText(QStringLiteral("系统复制一"));
    QTRY_COMPARE(items(service).size(), 1);
    const QString textId = firstId(service);
    auto* enriched = new QMimeData;
    enriched->setText(QStringLiteral("系统复制一"));
    enriched->setHtml(QStringLiteral("<b>系统复制一</b>"));
    QGuiApplication::clipboard()->setMimeData(enriched);
    QTest::qWait(80);
    QCOMPARE(items(service).size(), 1); // 真实剪贴板的格式补充通知也走去重逻辑。
    QGuiApplication::clipboard()->setText(QStringLiteral("系统复制二"));
    QTRY_COMPARE(items(service).size(), 2);
    QVERIFY(ok(service.copy(textId)));
    QTest::qWait(80);
    QCOMPARE(items(service).size(), 2);
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("系统复制一"));
    QImage image(15, 20, QImage::Format_RGB32);
    image.fill(Qt::blue);
    QGuiApplication::clipboard()->setImage(image);
    QTRY_COMPARE(items(service).size(), 3);
    const QString imageId = firstId(service);
    QVERIFY(ok(service.copy(textId)));
    QVERIFY(ok(service.copy(imageId)));
    QCOMPARE(QGuiApplication::clipboard()->image(), image);
    QCOMPARE(items(service).size(), 3);
    QFile original(directory.filePath("kept.txt"));
    QVERIFY(original.open(QIODevice::WriteOnly));
    original.close();
    auto* files = new QMimeData;
    files->setUrls({QUrl::fromLocalFile(original.fileName())});
    QGuiApplication::clipboard()->setMimeData(files);
    QTRY_COMPARE(items(service).size(), 4);
    const QString fileId = firstId(service);
    QVERIFY(ok(service.copy(textId)));
    QVERIFY(ok(service.copy(fileId)));
    QCOMPARE(QGuiApplication::clipboard()->mimeData()->urls().first(), QUrl::fromLocalFile(original.fileName()));
    QCOMPARE(items(service).size(), 4);
    QVERIFY(original.remove());
    QVERIFY(!ok(service.copy(fileId)));
}

void ClipboardServiceTest::failureProtection()
{
    QTemporaryDir directory;
    ClipboardService service(directory.path(), false);
    QVERIFY(ok(service.setTypes({"text"})));
    QMimeData text;
    text.setText("kept");
    QVERIFY(ok(service.capture(&text)));
    const QString path = directory.filePath("clipboard-history.v1.json");
    QVERIFY(QFile::rename(path, path + ".backup"));
    QVERIFY(QDir().mkpath(path));
    text.setText("cannot save");
    QVERIFY(!ok(service.capture(&text)));
    QCOMPARE(items(service).size(), 1);
    QVERIFY(!ok(service.setTypes({"image"})));
    QCOMPARE(data(service.snapshot()).value("types").toStringList(), QStringList{"text"});
    QVERIFY(QDir().rmdir(path));
    QVERIFY(ok(service.capture(&text))); // 前一次保存失败不能阻止相同内容重试。
    QCOMPARE(items(service).size(), 2);
    QFile corrupt(path);
    QVERIFY(corrupt.open(QIODevice::WriteOnly));
    corrupt.write("{broken");
    corrupt.close();
    ClipboardService broken(directory.path(), false);
    QVERIFY(!ok(broken.snapshot()));
    QVERIFY(!ok(broken.clear()));
    QVERIFY(!ok(broken.setTypes({"text"})));
    QVERIFY(corrupt.open(QIODevice::ReadOnly));
    QCOMPARE(corrupt.readAll(), QByteArray("{broken"));
}

void ClipboardServiceTest::capacityAndManagement()
{
    QTemporaryDir directory;
    ClipboardService service(directory.path(), false);
    QVERIFY(ok(service.setTypes({"text"})));
    QMimeData text;
    text.setText("pinned oldest");
    QVERIFY(ok(service.capture(&text)));
    const QString pinned = firstId(service);
    QVERIFY(ok(service.pin(pinned, true)));
    QString unpinned;
    for (int index = 0; index < 500; ++index) {
        text.setText(QString::number(index));
        QVERIFY(ok(service.capture(&text)));
        if (index == 0) unpinned = firstId(service);
    }
    QCOMPARE(items(service).size(), 500);
    QVERIFY(ok(service.read(pinned)));
    QVERIFY(!ok(service.read(unpinned)));
    QVERIFY(ok(service.remove({pinned, firstId(service)})));
    QCOMPARE(items(service).size(), 498);
    QVERIFY(ok(service.clear()));
    QVERIFY(items(service).isEmpty());
    QCOMPARE(data(service.snapshot()).value("types").toStringList(), QStringList{"text"});
    ClipboardService reload(directory.path(), false);
    QVERIFY(items(reload).isEmpty());
}

void ClipboardServiceTest::pasteResourceFromFileAndImage()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ClipboardService service(directory.path(), false);
    // 复制的单个文件直接返回原路径，即使同时附带图像内容也不另存副本。
    QImage original(48, 36, QImage::Format_RGB32);
    original.fill(QColor(200, 100, 50));
    const QString png = directory.filePath(QStringLiteral("复制图片.png"));
    QVERIFY(original.save(png, "PNG"));
    // setMimeData 会接管数据所有权，传给剪贴板的数据必须堆分配。
    auto* fileCopy = new QMimeData;
    fileCopy->setUrls({QUrl::fromLocalFile(png)});
    fileCopy->setImageData(original);
    QGuiApplication::clipboard()->setMimeData(fileCopy);
    const QString target = directory.filePath("emoji/clipboard");
    const auto fromFile = service.pasteResource(target);
    QVERIFY(ok(fromFile));
    QCOMPARE(data(fromFile).value("target").toString(), QDir::toNativeSeparators(png));
    QCOMPARE(data(fromFile).value("title").toString(), QStringLiteral("复制图片.png"));
    QCOMPARE(QDir(target).entryList(QDir::Files).size(), 0); // 文件引用不产生副本。
    // 截屏等纯图像内容保存到指定目录，可原样读回；再次粘贴生成不同文件。
    QGuiApplication::clipboard()->setImage(original);
    const auto fromImage = service.pasteResource(target);
    QVERIFY(ok(fromImage));
    const QString saved = data(fromImage).value("target").toString();
    QVERIFY(QFileInfo(saved).fileName().startsWith(QStringLiteral("剪贴板图片_")));
    QCOMPARE(QImage(saved), original);
    const auto again = service.pasteResource(target);
    QVERIFY(ok(again));
    QVERIFY(data(again).value("target").toString() != saved);
    QCOMPARE(QDir(target).entryList(QDir::Files).size(), 2);
}

void ClipboardServiceTest::pasteResourceRejectsInvalidClipboard()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ClipboardService service(directory.path(), false);
    // 没有可用内容时提示先复制。
    QGuiApplication::clipboard()->setMimeData(new QMimeData);
    const auto empty = service.pasteResource(directory.path());
    QVERIFY(!ok(empty));
    QVERIFY(empty.value("error").toString().contains(QStringLiteral("请先复制内容")));
    // 多个文件引用要求只复制一个。
    auto* many = new QMimeData;
    many->setUrls({QUrl::fromLocalFile(directory.filePath("a.png")),
        QUrl::fromLocalFile(directory.filePath("b.png"))});
    QGuiApplication::clipboard()->setMimeData(many);
    const auto multiple = service.pasteResource(directory.path());
    QVERIFY(!ok(multiple));
    QVERIFY(multiple.value("error").toString().contains(QStringLiteral("多个文件")));
    // 引用的文件已删除时返回明确错误。
    auto* gone = new QMimeData;
    gone->setUrls({QUrl::fromLocalFile(directory.filePath("missing.png"))});
    QGuiApplication::clipboard()->setMimeData(gone);
    QVERIFY(!ok(service.pasteResource(directory.path())));
    QVERIFY(!QDir(directory.path()).exists(QStringLiteral("missing.png")));
}

QTEST_MAIN(ClipboardServiceTest)
#include "ClipboardServiceTest.moc"
