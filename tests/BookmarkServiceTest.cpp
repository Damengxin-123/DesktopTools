#include "services/BookmarkService.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {
/** 读取统一服务结果中的成功数据。 */
QVariantMap data(const QVariantMap& result)
{
    return result.value(QStringLiteral("data")).toMap();
}

/** 判断统一服务结果是否成功。 */
bool ok(const QVariantMap& result)
{
    return result.value(QStringLiteral("ok")).toBool();
}

/** 在临时目录中写入任意文件，用于构造损坏索引。 */
bool writeFile(const QString& path, const QByteArray& bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

/** 读取统一服务结果里新建书签的标识。 */
QString firstId(const QVariantMap& result)
{
    return data(result).value(QStringLiteral("id")).toString();
}
}

/** 验证书签索引的新增、修改、删除、排序、网址校验与损坏保护。 */
class BookmarkServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    /** 新增、重命名、改址、删除与排序。 */
    void savesUpdatesRemovesAndReorders()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        BookmarkService service(root.path());
        QSignalSpy changed(&service, &BookmarkService::changed);
        QCOMPARE(data(service.snapshot()).value(QStringLiteral("items")).toList().size(), 0);

        QVariantMap created = service.save({{QStringLiteral("name"), QStringLiteral("示例网站")},
            {QStringLiteral("url"), QStringLiteral("example.com")}});
        QVERIFY(ok(created));
        const QString first = firstId(created);
        QCOMPARE(data(created).value(QStringLiteral("url")).toString(), QStringLiteral("https://example.com"));

        const QVariantMap second = service.save({{QStringLiteral("name"), QStringLiteral("第二个")},
            {QStringLiteral("url"), QStringLiteral("https://other.org/page")}});
        QVERIFY(ok(second));
        const QString secondId = firstId(second);

        auto items = data(service.snapshot()).value(QStringLiteral("items")).toList();
        QCOMPARE(items.size(), 2);
        QCOMPARE(changed.count(), 2);

        // 修改名称与地址，创建的记录不应重复出现。
        QVariantMap updated = service.save({{QStringLiteral("id"), first},
            {QStringLiteral("name"), QStringLiteral("重命名后的网站")},
            {QStringLiteral("url"), QStringLiteral("https://example.com/index.html")}});
        QVERIFY(ok(updated));
        items = data(service.snapshot()).value(QStringLiteral("items")).toList();
        QCOMPARE(items.size(), 2);
        QCOMPARE(items.first().toMap().value(QStringLiteral("name")).toString(), QStringLiteral("重命名后的网站"));

        // 把第一个移到末尾。
        QVERIFY(ok(service.move(first, QString())));
        items = data(service.snapshot()).value(QStringLiteral("items")).toList();
        QCOMPARE(items.last().toMap().value(QStringLiteral("id")).toString(), first);
        // 再移到指定书签之前。
        QVERIFY(ok(service.move(first, secondId)));
        items = data(service.snapshot()).value(QStringLiteral("items")).toList();
        QCOMPARE(items.first().toMap().value(QStringLiteral("id")).toString(), first);
        QCOMPARE(items.last().toMap().value(QStringLiteral("id")).toString(), secondId);

        QVERIFY(ok(service.remove({first, secondId})));
        QCOMPARE(data(service.snapshot()).value(QStringLiteral("items")).toList().size(), 0);
        QVERIFY(!ok(service.remove({QStringLiteral("missing")})));
        QVERIFY(!ok(service.move(first, secondId)));
    }

    /** 名称与地址的边界校验。 */
    void rejectsInvalidNamesAndSchemes()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        BookmarkService service(root.path());
        QVERIFY(!ok(service.save({{QStringLiteral("name"), QStringLiteral("   ")},
            {QStringLiteral("url"), QStringLiteral("example.com")}})));
        QVERIFY(!ok(service.save({{QStringLiteral("name"), QStringLiteral("缺少地址")},
            {QStringLiteral("url"), QString()}})));
        QVERIFY(!ok(service.save({{QStringLiteral("name"), QStringLiteral("本地文件")},
            {QStringLiteral("url"), QStringLiteral("file:///C:/data/report.pdf")}})));
        QVERIFY(!ok(service.save({{QStringLiteral("name"), QStringLiteral("脚本地址")},
            {QStringLiteral("url"), QStringLiteral("javascript:alert(1)")}})));
        QVERIFY(!ok(service.save({{QStringLiteral("name"), QStringLiteral("带空格")},
            {QStringLiteral("url"), QStringLiteral("https://example.com/a page")}})));
        QVERIFY(!ok(service.save({{QStringLiteral("id"), QStringLiteral("nope")},
            {QStringLiteral("name"), QStringLiteral("不存在的书签")},
            {QStringLiteral("url"), QStringLiteral("example.com")}})));
        QCOMPARE(data(service.snapshot()).value(QStringLiteral("items")).toList().size(), 0);
    }

    /** 索引损坏时进入只读保护，不覆盖已有文件。 */
    void protectsCorruptedIndex()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString indexPath = QDir(root.filePath(QStringLiteral("bookmark"))).filePath(QStringLiteral("bookmarks.v1.json"));
        QVERIFY(writeFile(indexPath, QByteArrayLiteral("{not-json")));
        BookmarkService service(root.path());
        QVERIFY(!ok(service.snapshot()));
        QVERIFY(!ok(service.save({{QStringLiteral("name"), QStringLiteral("不应写入")},
            {QStringLiteral("url"), QStringLiteral("example.com")}})));
        QFile original(indexPath);
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), QByteArrayLiteral("{not-json"));
    }

    /** 网址规范化：补全协议并拒绝不受支持的协议。 */
    void normalizesAddresses()
    {
        // 主机名由 QUrl 归一化为小写。
        QCOMPARE(data(BookmarkService::normalize(QStringLiteral("Example.com/path?q=1"))).value(QStringLiteral("url")).toString(),
            QStringLiteral("https://example.com/path?q=1"));
        QCOMPARE(data(BookmarkService::normalize(QStringLiteral("http://localhost:8080/index"))).value(QStringLiteral("url")).toString(),
            QStringLiteral("http://localhost:8080/index"));
        QCOMPARE(data(BookmarkService::normalize(QStringLiteral("  https://中国.example/ "))).value(QStringLiteral("url")).toString(),
            QUrl(QStringLiteral("https://xn--fiqs8s.example/")).toString());
        QVERIFY(!ok(BookmarkService::normalize(QString())));
        QVERIFY(!ok(BookmarkService::normalize(QStringLiteral("ftp://example.com"))));
        QVERIFY(!ok(BookmarkService::normalize(QStringLiteral("about:blank"))));
        QVERIFY(!ok(BookmarkService::normalize(QStringLiteral("https://"))));
    }

    /** 索引文件可再次读作相同内容，便于备份与人工查看。 */
    void persistsAcrossInstances()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        BookmarkService first(root.path());
        const QVariantMap created = first.save({{QStringLiteral("name"), QStringLiteral("持久化的书签")},
            {QStringLiteral("url"), QStringLiteral("example.org")}});
        QVERIFY(ok(created));
        BookmarkService second(root.path());
        const auto items = data(second.snapshot()).value(QStringLiteral("items")).toList();
        QCOMPARE(items.size(), 1);
        QCOMPARE(items.first().toMap().value(QStringLiteral("name")).toString(), QStringLiteral("持久化的书签"));
        QCOMPARE(items.first().toMap().value(QStringLiteral("id")).toString(), firstId(created));
    }
};

QTEST_GUILESS_MAIN(BookmarkServiceTest)
#include "BookmarkServiceTest.moc"
