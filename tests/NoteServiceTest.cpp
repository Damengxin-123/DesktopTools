#include "services/NoteService.h"

#include <QDir>
#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {
/** 在测试临时目录中写入文件，自动创建父目录。 */
bool writeFixture(const QString& path, const QByteArray& bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

/** 读取测试文件，核对迁移及失败后是否保留原内容。 */
QByteArray readFixture(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

/** 提取统一服务结果中的成功数据。 */
QVariantMap data(const QVariantMap& result)
{
    return result.value(QStringLiteral("data")).toMap();
}

/** 判断统一服务结果是否成功。 */
bool ok(const QVariantMap& result)
{
    return result.value(QStringLiteral("ok")).toBool();
}

/** 创建一份包含默认分类的新版索引测试数据。 */
QByteArray indexFixture(const QString& directory)
{
    const QJsonObject category{{QStringLiteral("id"), QStringLiteral("default")},
                              {QStringLiteral("name"), QStringLiteral("默认分类")}};
    const QJsonObject note{{QStringLiteral("id"), QString(32, QLatin1Char('a'))},
                          {QStringLiteral("categoryId"), QStringLiteral("default")},
                          {QStringLiteral("title"), QStringLiteral("测试")},
                          {QStringLiteral("directory"), directory}};
    return QJsonDocument(QJsonObject{{QStringLiteral("version"), 2},
        {QStringLiteral("categories"), QJsonArray{category}},
        {QStringLiteral("items"), QJsonArray{note}}}).toJson();
}
}

/** 验证便签迁移、文件隔离以及索引保存失败时的数据保护。 */
class NoteServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    /** 旧便签分类和图片成功导入，首次保存保留旧目录原件。 */
    void importsLegacyNotesAndImages();
    /** 重命名、分类删除、重新加载均维持同一个便签标识。 */
    void preservesIdsAndCategories();
    /** 批量删除仅影响索引，重启不会重新导入已删除旧便签。 */
    void deletesWithoutReimportingLegacyContent();
    /** 损坏的新索引不能被任何保存操作覆盖。 */
    void protectsCorruptIndex();
    /** 旧分类索引损坏时扫描便签，并向调用方报告警告。 */
    void recoversCorruptLegacyIndex();
    /** 索引及图片中的目录穿越不能读取根目录外内容。 */
    void rejectsPathTraversalAndExternalResources();
    /** 无法写入索引时不能返回成功或修改原便签。 */
    void reportsSaveFailureWithoutChangingContent();
    /** 移动便签时同时保持分类和相邻顺序。 */
    void movesAndOrdersNotes();
    /** 拒绝超过像素限制或引用外部路径的图片，不能悄悄替换已保存内容。 */
    void rejectsUnsavableImagesWithoutChangingContent();
};

void NoteServiceTest::importsLegacyNotesAndImages()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString oldPath = directory.filePath(QStringLiteral("note/旧便签/index.html"));
    const QByteArray original = QStringLiteral("<html><body><p>旧内容<b>加粗</b></p><img src=\"images/photo.png\"></body></html>").toUtf8();
    QVERIFY(writeFixture(oldPath, original));
    QVERIFY(QDir().mkpath(directory.filePath(QStringLiteral("note/旧便签/images"))));
    QImage picture(3, 2, QImage::Format_ARGB32);
    picture.fill(Qt::red);
    QVERIFY(picture.save(directory.filePath(QStringLiteral("note/旧便签/images/photo.png"))));
    QVERIFY(writeFixture(directory.filePath(QStringLiteral("note/tree_config.json")),
        QStringLiteral("{\"categories\":[{\"name\":\"工作\",\"items\":[\"旧便签\"]}]}").toUtf8()));

    NoteService service(directory.path());
    QVERIFY(ok(service.snapshot()));
    const QVariantList items = data(service.snapshot()).value(QStringLiteral("items")).toList();
    QCOMPARE(items.size(), 1);
    const QVariantMap item = items.first().toMap();
    const QString id = item.value(QStringLiteral("id")).toString();
    QVERIFY(item.value(QStringLiteral("categoryId")).toString() != QStringLiteral("default"));
    QVariantMap note = data(service.readNote(id));
    const QString html = note.value(QStringLiteral("html")).toString();
    QVERIFY(html.contains(QStringLiteral("旧内容")));
    QVERIFY(html.contains(QStringLiteral("data:image/png;base64,")));
    QVERIFY(!html.contains(QStringLiteral("images/photo.png")));
    note[QStringLiteral("title")] = QStringLiteral("重命名后的便签");
    QVERIFY(ok(service.saveNote(note)));
    QCOMPARE(readFixture(oldPath), original);
    QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("note/") + id + QStringLiteral("/index.html"))));
    QCOMPARE(data(service.readNote(id)).value(QStringLiteral("title")).toString(), QStringLiteral("重命名后的便签"));
}

void NoteServiceTest::preservesIdsAndCategories()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    NoteService service(directory.path());
    QSignalSpy changes(&service, &NoteService::changed);
    const QString categoryId = data(service.saveCategory({}, QStringLiteral("计划"))).value(QStringLiteral("id")).toString();
    QVERIFY(!categoryId.isEmpty());
    const QVariantMap saved = service.saveNote({{QStringLiteral("title"), QStringLiteral("第一版")},
        {QStringLiteral("categoryId"), categoryId}, {QStringLiteral("html"), QStringLiteral("<p>正文</p>")}});
    QVERIFY(ok(saved));
    const QString id = data(saved).value(QStringLiteral("id")).toString();
    QVariantMap note = data(saved);
    note[QStringLiteral("title")] = QStringLiteral("第二版");
    QVERIFY(ok(service.saveNote(note)));
    QVERIFY(ok(service.saveCategory(categoryId, QStringLiteral("新的分类名"))));
    QVERIFY(ok(service.removeCategory(categoryId)));
    QVERIFY(!ok(service.removeCategory(QStringLiteral("default"))));
    QCOMPARE(changes.count(), 5);

    NoteService reopened(directory.path());
    const QVariantMap reloaded = data(reopened.readNote(id));
    QCOMPARE(reloaded.value(QStringLiteral("id")).toString(), id);
    QCOMPARE(reloaded.value(QStringLiteral("title")).toString(), QStringLiteral("第二版"));
    QCOMPARE(reloaded.value(QStringLiteral("categoryId")).toString(), QStringLiteral("default"));
    QCOMPARE(data(reopened.snapshot()).value(QStringLiteral("items")).toList().size(), 1);
}

void NoteServiceTest::deletesWithoutReimportingLegacyContent()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(writeFixture(directory.filePath(QStringLiteral("note/甲/index.html")), "<p>A</p>"));
    QVERIFY(writeFixture(directory.filePath(QStringLiteral("note/乙/index.html")), "<p>B</p>"));
    NoteService service(directory.path());
    QStringList ids;
    for (const QVariant& item : data(service.snapshot()).value(QStringLiteral("items")).toList())
        ids.append(item.toMap().value(QStringLiteral("id")).toString());
    QCOMPARE(ids.size(), 2);
    QVERIFY(!ok(service.removeNotes({ids.first(), QStringLiteral("unknown")})));
    QCOMPARE(data(service.snapshot()).value(QStringLiteral("items")).toList().size(), 2);
    QVERIFY(ok(service.removeNotes(ids)));
    QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("note/甲/index.html"))));
    QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("note/乙/index.html"))));
    NoteService reopened(directory.path());
    QVERIFY(data(reopened.snapshot()).value(QStringLiteral("items")).toList().isEmpty());
}

void NoteServiceTest::protectsCorruptIndex()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("note/notes.v2.json"));
    const QByteArray original("{damaged index");
    QVERIFY(writeFixture(path, original));
    NoteService service(directory.path());
    QVERIFY(!ok(service.snapshot()));
    QVERIFY(!ok(service.saveCategory({}, QStringLiteral("新分类"))));
    QVERIFY(!ok(service.saveNote({{QStringLiteral("title"), QStringLiteral("新便签")},
        {QStringLiteral("html"), QStringLiteral("<p>不能保存</p>")}})));
    QVERIFY(!ok(service.removeNotes({})));
    QCOMPARE(readFixture(path), original);
}

void NoteServiceTest::recoversCorruptLegacyIndex()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString legacy = directory.filePath(QStringLiteral("note/tree_config.json"));
    QVERIFY(writeFixture(legacy, "broken legacy index"));
    QVERIFY(writeFixture(directory.filePath(QStringLiteral("note/可恢复/index.html")), "<p>preserved</p>"));
    NoteService service(directory.path());
    const QVariantMap state = data(service.snapshot());
    QVERIFY(ok(service.snapshot()));
    QCOMPARE(state.value(QStringLiteral("items")).toList().size(), 1);
    QVERIFY(!state.value(QStringLiteral("warnings")).toStringList().isEmpty());
    QCOMPARE(readFixture(legacy), QByteArray("broken legacy index"));
}

void NoteServiceTest::rejectsPathTraversalAndExternalResources()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString index = directory.filePath(QStringLiteral("note/notes.v2.json"));
    const QByteArray original = indexFixture(QStringLiteral("../outside"));
    QVERIFY(writeFixture(index, original));
    NoteService invalid(directory.path());
    QVERIFY(!ok(invalid.snapshot()));
    QVERIFY(!ok(invalid.saveCategory({}, QStringLiteral("禁止写入"))));
    QCOMPARE(readFixture(index), original);
    QVERIFY(QFile::remove(index));

    const QByteArray html("<html><body background='file:///secret.png'><p style=\"background-image:url(file:///secret.png)\">safe</p>"
        "<img src='../../outside.png'><img src='https://example.invalid/pixel.png'>"
        "<iframe src='file:///secret.txt'></iframe><script>alert('bad')</script>"
        "<a href='javascript:alert(1)'>link</a></body></html>");
    QVERIFY(writeFixture(directory.filePath(QStringLiteral("note/安全便签/index.html")), html));
    QImage image(1, 1, QImage::Format_ARGB32);
    image.fill(Qt::blue);
    QVERIFY(image.save(directory.filePath(QStringLiteral("outside.png"))));
    NoteService service(directory.path());
    const QString id = data(service.snapshot()).value(QStringLiteral("items")).toList().first().toMap().value(QStringLiteral("id")).toString();
    const QString clean = data(service.readNote(id)).value(QStringLiteral("html")).toString();
    QVERIFY(!clean.contains(QStringLiteral("file:")));
    QVERIFY(!clean.contains(QStringLiteral("javascript:")));
    QVERIFY(!clean.contains(QStringLiteral("example.invalid")));
    QVERIFY(!clean.contains(QStringLiteral("<script")));
    QVERIFY(!clean.contains(QStringLiteral("<iframe")));
    QVERIFY(!clean.contains(QStringLiteral("data:image")));
    QVERIFY(clean.contains(QStringLiteral("safe")));
}

void NoteServiceTest::reportsSaveFailureWithoutChangingContent()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    NoteService service(directory.path());
    const QVariantMap saved = service.saveNote({{QStringLiteral("title"), QStringLiteral("原便签")},
        {QStringLiteral("html"), QStringLiteral("<p>原内容</p>")}});
    QVERIFY(ok(saved));
    QVariantMap input = data(saved);
    const QString content = directory.filePath(QStringLiteral("note/") + input.value(QStringLiteral("id")).toString() + QStringLiteral("/index.html"));
    const QByteArray original = readFixture(content);
    const QString index = directory.filePath(QStringLiteral("note/notes.v2.json"));
    QVERIFY(QFile::remove(index));
    QVERIFY(QDir().mkdir(index));
    QSignalSpy changes(&service, &NoteService::changed);
    input[QStringLiteral("html")] = QStringLiteral("<p>新内容</p>");
    QVERIFY(!ok(service.saveNote(input)));
    QVERIFY(!ok(service.saveCategory({}, QStringLiteral("无法写入"))));
    QCOMPARE(changes.count(), 0);
    QCOMPARE(readFixture(content), original);
    QCOMPARE(data(service.snapshot()).value(QStringLiteral("items")).toList().size(), 1);
}

void NoteServiceTest::movesAndOrdersNotes()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    NoteService service(directory.path());
    const QString category = data(service.saveCategory({}, QStringLiteral("目标"))).value(QStringLiteral("id")).toString();
    const QString first = data(service.saveNote({{QStringLiteral("title"), QStringLiteral("甲")},
        {QStringLiteral("html"), QStringLiteral("<p>A</p>")}, {QStringLiteral("categoryId"), category}})).value(QStringLiteral("id")).toString();
    const QString second = data(service.saveNote({{QStringLiteral("title"), QStringLiteral("乙")},
        {QStringLiteral("html"), QStringLiteral("<p>B</p>")}})).value(QStringLiteral("id")).toString();
    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());
    QVERIFY(ok(service.moveNote(second, category, first)));
    const QVariantList items = data(service.snapshot()).value(QStringLiteral("items")).toList();
    QCOMPARE(items.first().toMap().value(QStringLiteral("id")).toString(), second);
    QCOMPARE(items.first().toMap().value(QStringLiteral("categoryId")).toString(), category);
    QVERIFY(!ok(service.moveNote(first, QStringLiteral("default"), second)));
}

void NoteServiceTest::rejectsUnsavableImagesWithoutChangingContent()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    NoteService service(directory.path());
    const QVariantMap saved = service.saveNote({{QStringLiteral("title"), QStringLiteral("原便签")},
        {QStringLiteral("html"), QStringLiteral("<p>需要保留的正文</p>")}});
    QVERIFY(ok(saved));
    QVariantMap input = data(saved);
    const QString contentPath = directory.filePath(QStringLiteral("note/")
        + input.value(QStringLiteral("id")).toString() + QStringLiteral("/index.html"));
    const QByteArray originalContent = readFixture(contentPath);
    const QString indexPath = directory.filePath(QStringLiteral("note/notes.v2.json"));
    const QByteArray originalIndex = readFixture(indexPath);
    QSignalSpy changes(&service, &NoteService::changed);

    // 单色高像素图片压缩后很小，可通过网页的文件大小校验但超过服务像素限制。
    QImage oversized(5000, 5000, QImage::Format_Mono);
    oversized.fill(0);
    QByteArray png;
    QBuffer buffer(&png);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(oversized.save(&buffer, "PNG"));
    QVERIFY(png.size() < 10 * 1024 * 1024);
    const QStringList sources{
        QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64()),
        QStringLiteral("file:///C:/Windows/secret.png"),
        QStringLiteral("https://example.invalid/image.png"),
        QStringLiteral("data:image/png;base64,bm90IGEgcG5n")};
    for (const QString& source : sources) {
        input[QStringLiteral("html")] = QStringLiteral("<p>不能替换旧内容</p><img src=\"") + source + QStringLiteral("\">");
        const QVariantMap result = service.saveNote(input);
        QVERIFY(!ok(result));
        QVERIFY(!result.value(QStringLiteral("error")).toString().isEmpty());
        QCOMPARE(readFixture(contentPath), originalContent);
        QCOMPARE(readFixture(indexPath), originalIndex);
    }
    QCOMPARE(changes.count(), 0);
    const QString id = input.value(QStringLiteral("id")).toString();
    QVERIFY(data(service.readNote(id)).value(QStringLiteral("html")).toString().contains(QStringLiteral("需要保留的正文")));
}

QTEST_MAIN(NoteServiceTest)
#include "NoteServiceTest.moc"
