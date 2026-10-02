#include "services/GridMapService.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

namespace {
// 提取服务的成功状态。
bool ok(const QVariantMap& result) { return result.value("ok").toBool(); }
// 提取服务返回的数据对象。
QVariantMap data(const QVariantMap& result) { return result.value("data").toMap(); }
// 获取当前项目列表。
QVariantList items(const GridMapService& service) { return data(service.snapshot()).value("items").toList(); }
// 构造格子三元组。
QVariantList cell(qint64 x, qint64 y, const QString& color)
{
    return {static_cast<double>(x), static_cast<double>(y), color};
}
}

// 覆盖项目创建、栅格保存读取、越界保护、删除和损坏索引保护。
class GridMapServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    // 新建项目写入初始文件，重命名和列表快照保持稳定标识。
    void createRenameAndList();
    // 栅格坐标与颜色保存后可完整读回，更新时间刷新。
    void saveAndReadCells();
    // 无效名称、越界坐标、非法颜色和粗细都会被拒绝。
    void rejectsInvalidInput();
    // 重复坐标去重、#RGB 展开为 #RRGGBB。
    void normalizesCells();
    // 删除项目同时移除数据文件。
    void removesProject();
    // 同一数据目录重新加载后数据保持一致。
    void reloadsPersistedData();
    // 索引损坏时服务进入只读保护，拒绝覆盖原文件。
    void protectsCorruptedIndex();

private:
    // 在临时目录创建一个网格图并保存两个格子，返回项目 ID。
    QString seedProject(const QString& dataRoot);
};

void GridMapServiceTest::createRenameAndList()
{
    QTemporaryDir root;
    GridMapService service(root.path());
    const auto created = service.create(QStringLiteral("机房布置"));
    QVERIFY(ok(created));
    QCOMPARE(data(created).value("title").toString(), QStringLiteral("机房布置"));
    QCOMPARE(items(service).size(), 1);

    // 空名称与超长名称都被拒绝。
    QVERIFY(!ok(service.create(QStringLiteral("  "))));
    QVERIFY(!ok(service.create(QString(201, QLatin1Char('x')))));

    const QString id = data(created).value("id").toString();
    const auto renamed = service.rename(id, QStringLiteral("楼层示意"));
    QVERIFY(ok(renamed));
    QCOMPARE(data(renamed).value("title").toString(), QStringLiteral("楼层示意"));
    QCOMPARE(items(service).first().toMap().value("title").toString(), QStringLiteral("楼层示意"));
    QVERIFY(!ok(service.rename(QStringLiteral("missing"), QStringLiteral("其他"))));
}

QString GridMapServiceTest::seedProject(const QString& dataRoot)
{
    GridMapService service(dataRoot);
    const auto created = service.create(QStringLiteral("演示"));
    if (!ok(created))
        return QString();
    const QString id = data(created).value("id").toString();
    const auto saved = service.save(id, QVariantMap{
        {"lineColor", QStringLiteral("#94a3b8")}, {"lineWidth", 2},
        {"cells", QVariantList{cell(0, 0, QStringLiteral("#ff0000")),
            cell(-3, 2, QStringLiteral("#00aa55")), cell(1, -1, QStringLiteral("#30f"))}}});
    return ok(saved) ? id : QString();
}

void GridMapServiceTest::saveAndReadCells()
{
    QTemporaryDir root;
    const QString id = seedProject(root.path());
    QVERIFY(!id.isEmpty());

    GridMapService service(root.path());
    const auto loaded = service.read(id);
    QVERIFY(ok(loaded));
    QCOMPARE(data(loaded).value("lineColor").toString(), QStringLiteral("#94a3b8"));
    QCOMPARE(data(loaded).value("lineWidth").toInt(), 2);
    const QVariantList cells = data(loaded).value("cells").toList();
    QCOMPARE(cells.size(), 3);
    // 读取顺序保持保存顺序。
    QCOMPARE(cells.at(0).toList().at(0).toDouble(), 0.0);
    QCOMPARE(cells.at(0).toList().at(2).toString(), QStringLiteral("#ff0000"));
    QCOMPARE(cells.at(1).toList().at(0).toDouble(), -3.0);
    QCOMPARE(cells.at(2).toList().at(2).toString(), QStringLiteral("#3300ff"));
    // 读取不存在的项目返回明确错误。
    QVERIFY(!ok(service.read(QStringLiteral("ffffffffffffffffffffffffffffffff"))));

    // 保存后更新时间变化，快照反映新时间。
    const QString before = items(service).first().toMap().value("updatedAt").toString();
    const auto saved = service.save(id, QVariantMap{{"lineColor", QStringLiteral("#123456")},
        {"lineWidth", 1}, {"cells", QVariantList{QVariant(cell(5, 5, QStringLiteral("#123456")))}}});
    QVERIFY(ok(saved));
    QVERIFY(data(saved).value("cellCount").toInt() == 1);
    QVERIFY(items(service).first().toMap().value("updatedAt").toString() >= before);
}

void GridMapServiceTest::rejectsInvalidInput()
{
    QTemporaryDir root;
    GridMapService service(root.path());
    const QString id = data(service.create(QStringLiteral("校验"))).value("id").toString();

    QVERIFY(!ok(service.save(id, QVariantMap{{"lineColor", QStringLiteral("red")},
        {"lineWidth", 1}, {"cells", QVariantList{}}})));
    QVERIFY(!ok(service.save(id, QVariantMap{{"lineColor", QStringLiteral("#112233")},
        {"lineWidth", 0}, {"cells", QVariantList{}}})));
    QVERIFY(!ok(service.save(id, QVariantMap{{"lineColor", QStringLiteral("#112233")},
        {"lineWidth", 7}, {"cells", QVariantList{}}})));
    // 坐标超界、非整数与小数都被拒绝。
    QVERIFY(!ok(service.save(id, QVariantMap{{"lineColor", QStringLiteral("#112233")},
        {"lineWidth", 1}, {"cells", QVariantList{QVariant(cell(1000001, 0, QStringLiteral("#112233")))}}})));
    QVERIFY(!ok(service.save(id, QVariantMap{{"lineColor", QStringLiteral("#112233")},
        {"lineWidth", 1}, {"cells", QVariantList{QVariant(cell(1, 2, QStringLiteral("blue")))}}})));
    QVERIFY(!ok(service.save(id, QVariantMap{{"lineColor", QStringLiteral("#112233")},
        {"lineWidth", 1}, {"cells", QVariantList{QVariant(QVariantList{0.5, 0, QStringLiteral("#112233")})}}})));
    // 只有坐标没有颜色的格子被拒绝。
    const QVariantList colorless = QVariantList{0, 0};
    QVERIFY(!ok(service.save(id, QVariantMap{{"lineColor", QStringLiteral("#112233")},
        {"lineWidth", 1}, {"cells", QVariantList{colorless}}})));
    // 项目不存在时保存失败。
    QVERIFY(!ok(service.save(QStringLiteral("ffffffffffffffffffffffffffffffff"),
        QVariantMap{{"lineColor", QStringLiteral("#112233")}, {"lineWidth", 1}, {"cells", QVariantList{}}})));
    // 重新加载确认校验失败没有写入任何内容。
    GridMapService reload(root.path());
    const auto loaded = reload.read(id);
    QCOMPARE(data(loaded).value("cells").toList().size(), 0);
}

void GridMapServiceTest::normalizesCells()
{
    QTemporaryDir root;
    GridMapService service(root.path());
    const QString id = data(service.create(QStringLiteral("规范化"))).value("id").toString();
    const auto saved = service.save(id, QVariantMap{{"lineColor", QStringLiteral("#ABC")},
        {"lineWidth", 3}, {"cells", QVariantList{cell(2, 2, QStringLiteral("#ABC")),
            cell(2, 2, QStringLiteral("#000000")), cell(-1, 0, QStringLiteral("#FFFFFF"))}}});
    QVERIFY(ok(saved));
    const auto loaded = service.read(id);
    QVERIFY(ok(loaded));
    const QVariantList cells = data(loaded).value("cells").toList();
    QCOMPARE(cells.size(), 2); // 重复坐标只保留首次出现的颜色。
    QCOMPARE(data(loaded).value("lineColor").toString(), QStringLiteral("#aabbcc"));
    QCOMPARE(cells.at(0).toList().at(2).toString(), QStringLiteral("#aabbcc"));
    QCOMPARE(cells.at(1).toList().at(2).toString(), QStringLiteral("#ffffff"));
}

void GridMapServiceTest::removesProject()
{
    QTemporaryDir root;
    GridMapService service(root.path());
    const QString first = data(service.create(QStringLiteral("第一个"))).value("id").toString();
    const QString second = data(service.create(QStringLiteral("第二个"))).value("id").toString();
    QVERIFY(ok(service.save(first, QVariantMap{{"lineColor", QStringLiteral("#112233")},
        {"lineWidth", 1}, {"cells", QVariantList{QVariant(cell(0, 0, QStringLiteral("#112233")))}}})));

    const auto removed = service.remove(QStringList{first, QStringLiteral("ffffffffffffffffffffffffffffffff")});
    QVERIFY(!ok(removed)); // 不存在的项目阻止整批删除。
    QCOMPARE(items(service).size(), 2);

    QVERIFY(ok(service.remove(QStringList{first})));
    QCOMPARE(items(service).size(), 1);
    QVERIFY(!ok(service.read(first)));
    QVERIFY(!QFile::exists(QDir(QDir(root.path()).filePath(QStringLiteral("gridmap/maps")))
        .filePath(first + QStringLiteral(".json"))));
    QVERIFY(QFile::exists(QDir(QDir(root.path()).filePath(QStringLiteral("gridmap/maps")))
        .filePath(second + QStringLiteral(".json"))));
}

void GridMapServiceTest::reloadsPersistedData()
{
    QTemporaryDir root;
    const QString id = seedProject(root.path());
    QVERIFY(!id.isEmpty());

    GridMapService service(root.path());
    QCOMPARE(items(service).size(), 1);
    QCOMPARE(items(service).first().toMap().value("title").toString(), QStringLiteral("演示"));
    const auto loaded = service.read(id);
    QVERIFY(ok(loaded));
    QCOMPARE(data(loaded).value("cells").toList().size(), 3);
}

void GridMapServiceTest::protectsCorruptedIndex()
{
    QTemporaryDir root;
    const QString id = seedProject(root.path());
    QVERIFY(!id.isEmpty());
    const QString indexPath = QDir(QDir(root.path()).filePath(QStringLiteral("gridmap")))
        .filePath(QStringLiteral("index.v1.json"));
    QFile(indexPath).resize(0); // 截断为空文件，模拟损坏。

    GridMapService service(root.path());
    QVERIFY(!ok(service.snapshot()));
    QVERIFY(!ok(service.create(QStringLiteral("不应创建"))));
    QVERIFY(!ok(service.save(id, QVariantMap{{"lineColor", QStringLiteral("#112233")},
        {"lineWidth", 1}, {"cells", QVariantList{}}})));
    QVERIFY(!ok(service.remove(QStringList{id})));
    // 损坏的索引保持原样，用户可以手工恢复。
    QCOMPARE(QFileInfo(indexPath).size(), 0);
    // 修复索引后服务恢复正常。
    QJsonObject rootObject{{"version", 1}, {"items", QJsonArray{QJsonObject{
        {"id", id}, {"title", QStringLiteral("演示")}, {"updatedAt", QString()}}}}};
    QFile repaired(indexPath);
    QVERIFY(repaired.open(QIODevice::WriteOnly));
    repaired.write(QJsonDocument(rootObject).toJson());
    repaired.close();
    GridMapService recovered(root.path());
    QVERIFY(ok(recovered.snapshot()));
}

QTEST_MAIN(GridMapServiceTest)
#include "GridMapServiceTest.moc"
