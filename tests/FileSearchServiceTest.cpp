#include "services/FileSearchIndex.h"
#include "services/FileSearchService.h"
#include "services/ServiceResult.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QDateTime>
#include <QElapsedTimer>
#include <memory>
#include <QtEndian>
#include "services/FileJournalProtocol.h"

namespace
{
// 写入一个小文本文件。
void writeFile(const QString& path, const char* content)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write(content) != -1);
}

// 建立带中文目录的测试文件树；成功时输出根路径并返回 true。
bool makeTree(QTemporaryDir& directory, QString* root)
{
    QDir base(directory.path());
    if (!base.mkpath(QStringLiteral("报表/归档")) || !base.mkpath(QStringLiteral("相册")))
        return false;
    writeFile(base.filePath(QStringLiteral("报表/年度总结.docx")), "a");
    writeFile(base.filePath(QStringLiteral("报表/月度报表.docx")), "b");
    writeFile(base.filePath(QStringLiteral("报表/归档/2019方案.txt")), "c");
    writeFile(base.filePath(QStringLiteral("相册/假期照片.jpg")), "d");
    writeFile(base.filePath(QStringLiteral("说明.txt")), "e");
    *root = QDir::toNativeSeparators(base.absolutePath());
    return true;
}

// 提交检索并等待结果信号；成功时输出最近一次结果并返回 true。
bool searchAndWait(FileSearchService& service, const QString& query, QVariantMap* result,
                   bool searchPath = false)
{
    QSignalSpy spy(&service, &FileSearchService::searchFinished);
    if (!service.search(query, 0, 200, searchPath).value("ok").toBool())
        return false;
    if (!spy.wait(10000))
        return false;
    *result = spy.last().first().toMap();
    return true;
}

// 结果行路径列表。
QStringList rowPaths(const QVariantMap& result)
{
    QStringList paths;
    for (const auto& value : result.value(QStringLiteral("rows")).toList())
        paths.append(value.toMap().value(QStringLiteral("path")).toString());
    return paths;
}
}

// 验证文件名索引核心的键布局、路径拼接、查询语法与持久化，以及服务的扫描与实时更新。
class FileSearchServiceTest final : public QObject
{
    Q_OBJECT
private slots:
    // 键包含卷序号；跨盘同名引用号互不冲突。
    void keyLayout();
    // 日志跨批次改名、组合删除原因、MFT 槽位复用和损坏批次保护。
    void journalReplay();
    // 查询预筛结果与朴素 Unicode 匹配一致，分页缓存随更新失效。
    void fastSearchConsistency();
    // 只读服务协议拒绝写盘指令和无效输入尺寸。
    void journalProtocolBounds();
    // 连续输入只交付最新结果，不继续计算过期查询。
    void latestQueryWins();
    // 批量文件变化后仍可检索，反复关闭监视线程不得挂起。
    void changeBurstAndShutdown();
    // 深层目录与缺失父节点的清理保持完整，并覆盖大批量删除。
    void largeSubtreeRemoval();
    // 沿父链拼接完整路径；根记录名称即卷前缀。
    void upsertAndPathOf();
    // 删除目录时连带删除全部子孙并维护计数。
    void removeRemovesSubtree();
    // 查询解析：词元、通配、ext:/folder:/file:/path:。
    void queryParse();
    // 检索过滤、路径模式与分页排序。
    void searchFilterAndPagination();
    // 命中超过收集上限时仍给出准确总数。
    void searchCollectionCap();
    // 持久化往返后索引与卷信息保持一致。
    void storeRoundTrip();
    // 未启用时状态明确且检索失败。
    void disabledService();
    // 服务扫描注入目录、检索结果正确，且文件增删能实时反映。
    void serviceScansRootsAndTracksChanges();
};

void FileSearchServiceTest::keyLayout()
{
    const quint64 first = FileSearchIndex::key(3, 0x1234);
    const quint64 second = FileSearchIndex::key(4, 0x1234);
    QVERIFY(first != second);
    QCOMPARE(FileSearchIndex::key(3, 0x1234), first);
    // 扫描键只占低 56 位且带特征位，与引用号 0-15 的保留区间不冲突。
    const quint64 scan = FileSearchIndex::scanKey(1, QStringLiteral("C:\\Windows"));
    QVERIFY(scan != 0);
    QCOMPARE(scan >> 56, quint64(1));
    QVERIFY(scan & 0x0080000000000000ull);
}

void FileSearchServiceTest::upsertAndPathOf()
{
    FileSearchIndex index;
    const quint64 root = FileSearchIndex::key(3, 0);
    index.upsert(root, 0, QStringLiteral("C:"), true);
    const quint64 docs = FileSearchIndex::key(3, 100);
    const quint64 file = FileSearchIndex::key(3, 101);
    index.upsert(docs, root, QStringLiteral("文档"), true);
    index.upsert(file, docs, QStringLiteral("计划.TXT"), false);
    QCOMPARE(index.size(), 3);
    QCOMPARE(index.fileCount(), 1);
    QCOMPARE(index.directoryCount(), 2);
    QCOMPARE(index.pathOf(file), QStringLiteral("C:\\文档\\计划.TXT"));
    QCOMPARE(index.pathOf(docs), QStringLiteral("C:\\文档"));
    QCOMPARE(index.pathOf(root), QStringLiteral("C:"));
    // 父记录缺失时无法拼出路径，返回空串。
    QVERIFY(index.pathOf(FileSearchIndex::key(3, 999)).isEmpty());
    // 覆盖更新不改变计数。
    index.upsert(file, docs, QStringLiteral("计划.TXT"), false);
    QCOMPARE(index.size(), 3);
}

void FileSearchServiceTest::removeRemovesSubtree()
{
    FileSearchIndex index;
    const quint64 root = FileSearchIndex::key(2, 0);
    const quint64 dir = FileSearchIndex::key(2, 10);
    const quint64 child = FileSearchIndex::key(2, 11);
    const quint64 grand = FileSearchIndex::key(2, 12);
    const quint64 sibling = FileSearchIndex::key(2, 13);
    index.upsert(root, 0, QStringLiteral("D:"), true);
    index.upsert(dir, root, QStringLiteral("a"), true);
    index.upsert(child, dir, QStringLiteral("b"), true);
    index.upsert(grand, child, QStringLiteral("c.txt"), false);
    index.upsert(sibling, root, QStringLiteral("d.txt"), false);
    const QVector<quint64> removed = index.remove({dir});
    QCOMPARE(removed.size(), 3);
    QVERIFY(!index.contains(dir) && !index.contains(child) && !index.contains(grand));
    QVERIFY(index.contains(sibling));
    QCOMPARE(index.fileCount(), 1);
    QCOMPARE(index.directoryCount(), 1);
}

void FileSearchServiceTest::queryParse()
{
    const auto query = FileSearchIndex::Query::parse(
        QStringLiteral("年度 报告*.docx ext:PDF,jpg folder: path:"));
    QVERIFY(query.searchPath);
    QVERIFY(query.foldersOnly);
    QCOMPARE(query.substrings.size(), 1);
    QCOMPARE(query.substrings.first(), QStringLiteral("年度"));
    QCOMPARE(query.wildcards.size(), 1);
    QCOMPARE(query.wildcards.first(), QStringLiteral("报告*.docx"));
    QCOMPARE(query.extensions, QVector<QString>({QStringLiteral("pdf"), QStringLiteral("jpg")}));
    QVERIFY(!query.isEmpty());
    // 纯过滤词也有意义（folder: 单独使用列出全部文件夹）。
    QVERIFY(!FileSearchIndex::Query::parse(QStringLiteral("file:"), false).isEmpty());
    QVERIFY(FileSearchIndex::Query::parse(QStringLiteral("   "), false).isEmpty());
}

void FileSearchServiceTest::searchFilterAndPagination()
{
    FileSearchIndex index;
    const quint64 root = FileSearchIndex::key(1, 0);
    index.upsert(root, 0, QStringLiteral("C:"), true);
    const quint64 docs = FileSearchIndex::key(1, 10);
    index.upsert(docs, root, QStringLiteral("docs"), true);
    quint64 frn = 100;
    for (const QString& name : {QStringLiteral("年度总结.docx"), QStringLiteral("月度总结.docx"),
             QStringLiteral("总结.jpg"), QStringLiteral("其他.txt")}) {
        index.upsert(FileSearchIndex::key(1, frn++), docs, name, false);
    }
    index.upsert(FileSearchIndex::key(1, frn++), docs, QStringLiteral("总结备份"), true);

    // 单词元子串匹配，按路径排序（中文按 UTF-16 码点：年 < 总 < 月）。
    auto result = index.search(FileSearchIndex::Query::parse(QStringLiteral("总结"), false),
        0, 10, nullptr, nullptr);
    QStringList paths;
    for (const quint64 key : result)
        paths.append(index.pathOf(key));
    QCOMPARE(paths, QStringList({QStringLiteral("C:\\docs\\年度总结.docx"), QStringLiteral("C:\\docs\\总结.jpg"),
        QStringLiteral("C:\\docs\\总结备份"), QStringLiteral("C:\\docs\\月度总结.docx")}));

    // AND 词元 + 扩展名过滤。
    QCOMPARE(index.search(FileSearchIndex::Query::parse(QStringLiteral("总结 ext:docx"), false),
        0, 10).size(), 2);
    QCOMPARE(index.search(FileSearchIndex::Query::parse(QStringLiteral("总结 ext:pdf"), false), 0, 10).size(), 0);

    // folder:/file: 过滤。
    QCOMPARE(index.search(FileSearchIndex::Query::parse(QStringLiteral("总结 folder:"), false), 0, 10).size(), 1);
    QCOMPARE(index.search(FileSearchIndex::Query::parse(QStringLiteral("总结 file:"), false), 0, 10).size(), 3);

    // 通配整名匹配且大小写不敏感。
    QCOMPARE(index.search(FileSearchIndex::Query::parse(QStringLiteral("*.DOCX"), false), 0, 10).size(), 2);
    QCOMPARE(index.search(FileSearchIndex::Query::parse(QStringLiteral("年度*"), false), 0, 10).size(), 1);

    // 路径模式：目录名出现在路径中即可命中（含 docs 目录自身与其中 5 条记录）。
    QCOMPARE(index.search(FileSearchIndex::Query::parse(QStringLiteral("docs"), true), 0, 10).size(), 6);
    // 名称模式只匹配名称本身：仅 docs 目录命中。
    QCOMPARE(index.search(FileSearchIndex::Query::parse(QStringLiteral("docs"), false), 0, 10).size(), 1);

    // 分页：offset 跳过已展示条目。
    int total = 0;
    const auto page = index.search(FileSearchIndex::Query::parse(QStringLiteral("总结"), false),
        2, 2, &total, nullptr);
    QCOMPARE(page.size(), 2);
    QCOMPARE(total, 4);
    QCOMPARE(index.pathOf(page.first()), QStringLiteral("C:\\docs\\总结备份"));
}

void FileSearchServiceTest::searchCollectionCap()
{
    FileSearchIndex index;
    const quint64 root = FileSearchIndex::key(1, 0);
    index.upsert(root, 0, QStringLiteral("C:"), true);
    for (quint64 i = 0; i < FileSearchIndex::CollectionCap + 10; ++i)
        index.upsert(FileSearchIndex::key(1, 100 + i), root, QStringLiteral("文件") + QString::number(i), false);
    int total = 0;
    int collected = 0;
    const auto page = index.search(FileSearchIndex::Query::parse(QStringLiteral("文件"), false),
        0, 200, &total, &collected);
    QCOMPARE(total, FileSearchIndex::CollectionCap + 10);
    QCOMPARE(collected, FileSearchIndex::CollectionCap);
    QCOMPARE(page.size(), 200);
}

void FileSearchServiceTest::storeRoundTrip()
{
    FileSearchIndex index;
    QVector<FileSearchVolume> volumes;
    FileSearchVolume ntfs;
    ntfs.driveIndex = 3;
    ntfs.displayRoot = QStringLiteral("C:");
    ntfs.fileSystem = QStringLiteral("NTFS");
    ntfs.mftMode = true;
    ntfs.journalId = 0x123456789;
    ntfs.nextUsn = 42;
    volumes.append(ntfs);
    FileSearchVolume scan;
    scan.driveIndex = 4;
    scan.displayRoot = QStringLiteral("D:\\资料");
    scan.fileSystem = QStringLiteral("DIR");
    volumes.append(scan);
    index.upsert(FileSearchIndex::key(3, 0), 0, QStringLiteral("C:"), true);
    index.upsert(FileSearchIndex::key(3, 7), FileSearchIndex::key(3, 0), QStringLiteral("计划.xlsx"), false);
    index.upsert(FileSearchIndex::scanKey(4, scan.displayRoot), 0, scan.displayRoot, true);
    index.upsert(FileSearchIndex::scanKey(4, scan.displayRoot + QStringLiteral("\\报表")),
        FileSearchIndex::scanKey(4, scan.displayRoot), QStringLiteral("报表"), true);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = QDir(directory.path()).absoluteFilePath(QStringLiteral("index.bin"));
    QString error;
    QVERIFY2(FileSearchIndex::save(path, volumes, index, &error), qPrintable(error));
    QVector<FileSearchVolume> loadedVolumes;
    FileSearchIndex loaded;
    QVERIFY2(FileSearchIndex::load(path, &loadedVolumes, &loaded, &error), qPrintable(error));
    QCOMPARE(loadedVolumes.size(), volumes.size());
    QCOMPARE(loadedVolumes.first().displayRoot, QStringLiteral("C:"));
    QCOMPARE(loadedVolumes.first().journalId, quint64(0x123456789));
    QCOMPARE(loadedVolumes.last().mftMode, false);
    QCOMPARE(loaded.size(), index.size());
    QCOMPARE(loaded.fileCount(), index.fileCount());
    QCOMPARE(loaded.directoryCount(), index.directoryCount());
    QCOMPARE(loaded.pathOf(FileSearchIndex::key(3, 7)), QStringLiteral("C:\\计划.xlsx"));
    // 损坏文件必须报错而不是返回半份数据。
    QVERIFY(QFile::resize(path, 10));
    QVERIFY(!FileSearchIndex::load(path, &loadedVolumes, &loaded, &error));
    QVERIFY(!error.isEmpty());
}

void FileSearchServiceTest::disabledService()
{
    QVERIFY(qEnvironmentVariableIsEmpty("DESKTOPTOOL_FILESEARCH_ROOTS"));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FileSearchService service(directory.path(), false);
    QCOMPARE(service.statusSnapshot().value("phase").toString(), QStringLiteral("disabled"));
    const auto result = service.search(QStringLiteral("任何"), 0, 10, false);
    QVERIFY(!result.value("ok").toBool());
    QVERIFY(!result.value("error").toString().isEmpty());
}

void FileSearchServiceTest::serviceScansRootsAndTracksChanges()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QTemporaryDir data;
    QVERIFY(data.isValid());
    QString root;
    QVERIFY(makeTree(directory, &root));
    // 通过环境变量注入扫描根目录（服务构造时读取）；
    // qputenv 按系统代码页写字节，含中文路径必须用 toLocal8Bit。
    qputenv("DESKTOPTOOL_FILESEARCH_ROOTS", root.toLocal8Bit());
    FileSearchService service(data.path(), false);
    QTRY_VERIFY_WITH_TIMEOUT(service.statusSnapshot().value("phase").toString()
        == QStringLiteral("ready"), 30000);
    const auto status = service.statusSnapshot();
    // 失败时输出完整状态，便于区分目录注入失效与扫描逻辑缺陷。
    if (status.value("fileCount").toInt() != 5)
        qWarning().noquote() << "文件检索服务状态：" << status;
    QCOMPARE(status.value("mode").toString(), QStringLiteral("scan"));
    QCOMPARE(status.value("fileCount").toInt(), 5);
    QCOMPARE(status.value("directoryCount").toInt(), 4);

    // 中文关键词与扩展名过滤（树中仅“年度总结.docx”含“总结”）。
    QVariantMap result;
    QVERIFY(searchAndWait(service, QStringLiteral("总结"), &result));
    QCOMPARE(rowPaths(result).size(), 1);
    QVERIFY(rowPaths(result).first().endsWith(QStringLiteral("年度总结.docx")));
    QVERIFY(searchAndWait(service, QStringLiteral("报表"), &result));
    QCOMPARE(rowPaths(result).size(), 2);
    QVERIFY(searchAndWait(service, QStringLiteral("ext:jpg"), &result));
    QCOMPARE(rowPaths(result).size(), 1);
    QVERIFY(rowPaths(result).first().endsWith(QStringLiteral("假期照片.jpg")));

    // 新增文件在变更监视回收后可被检索到。
    writeFile(QDir(root).filePath(QStringLiteral("新增说明.txt")), "x");
    const QDateTime deadline = QDateTime::currentDateTimeUtc().addSecs(25);
    forever {
        QVariantMap fresh;
        if (searchAndWait(service, QStringLiteral("新增说明"), &fresh))
            result = fresh;
        if (rowPaths(result).size() == 1 || QDateTime::currentDateTimeUtc() > deadline)
            break;
    }
    QCOMPARE(rowPaths(result).size(), 1);
    QVERIFY(rowPaths(result).first().endsWith(QStringLiteral("新增说明.txt")));

    // 删除文件后从索引消失。
    QFile::remove(QDir(root).filePath(QStringLiteral("新增说明.txt")));
    const QDateTime removalDeadline = QDateTime::currentDateTimeUtc().addSecs(25);
    forever {
        QVariantMap fresh;
        if (searchAndWait(service, QStringLiteral("新增说明"), &fresh))
            result = fresh;
        if (rowPaths(result).isEmpty() || QDateTime::currentDateTimeUtc() > removalDeadline)
            break;
    }
    QCOMPARE(rowPaths(result).size(), 0);
    qunsetenv("DESKTOPTOOL_FILESEARCH_ROOTS");
}

void FileSearchServiceTest::largeSubtreeRemoval()
{
    FileSearchIndex index;
    index.upsert(1, 0, QStringLiteral("根"), true);
    for (quint64 i = 2; i <= 2000; ++i)
        index.upsert(i, i - 1, QStringLiteral("目录"), true);
    for (quint64 i = 3000; i < 53000; ++i)
        index.upsert(i, 2000, QStringLiteral("文件"), false);
    QCOMPARE(index.remove({2}).size(), 51999);
    QCOMPARE(index.size(), 1);
    index.upsert(60000, 59999, QStringLiteral("孤儿"), false);
    QCOMPARE(index.remove({59999}).size(), 1);
    QCOMPARE(index.size(), 1);
}

void FileSearchServiceTest::changeBurstAndShutdown()
{
    QTemporaryDir directory;
    QTemporaryDir data;
    QVERIFY(directory.isValid() && data.isValid());
    qputenv("DESKTOPTOOL_FILESEARCH_ROOTS", directory.path().toLocal8Bit());
    auto service = std::make_unique<FileSearchService>(data.path(), false);
    qunsetenv("DESKTOPTOOL_FILESEARCH_ROOTS");
    QTRY_COMPARE_WITH_TIMEOUT(service->statusSnapshot().value("phase").toString(),
        QStringLiteral("ready"), 10000);
    // 超过合并上限的独立目录变更也不能丢失最终新增文件。
    for (int i = 0; i < 300; ++i) {
        const QString child = QStringLiteral("目录%1").arg(i);
        QVERIFY(QDir(directory.path()).mkpath(child));
        writeFile(QDir(directory.path()).filePath(child + QStringLiteral("/测试.txt")), "x");
    }
    QTRY_COMPARE_WITH_TIMEOUT(service->statusSnapshot().value("fileCount").toInt(), 300, 20000);
    QVariantMap result;
    QVERIFY(searchAndWait(*service, QStringLiteral("测试"), &result));
    QCOMPARE(result.value("total").toInt(), 300);
    QElapsedTimer timer;
    timer.start();
    service.reset();
    QVERIFY2(timer.elapsed() < 3000, "关闭目录监视耗时过长");
    for (int i = 0; i < 10; ++i) {
        qputenv("DESKTOPTOOL_FILESEARCH_ROOTS", directory.path().toLocal8Bit());
        service = std::make_unique<FileSearchService>(data.path(), false);
        qunsetenv("DESKTOPTOOL_FILESEARCH_ROOTS");
        QTRY_COMPARE_WITH_TIMEOUT(service->statusSnapshot().value("phase").toString(),
            QStringLiteral("ready"), 10000);
        timer.restart();
        service.reset();
        QVERIFY(timer.elapsed() < 3000);
    }
}

namespace {
// 构造完整 USN V2 批次，使用公开的记录布局模拟内核返回值。
QByteArray journalBatch(quint64 reference, quint64 parent, quint32 reason, const QString& name, bool directory)
{
    const int length = (60 + name.size() * 2 + 7) & ~7;
    QByteArray result(8 + length, char(0));
    auto* data = reinterpret_cast<uchar*>(result.data());
    qToLittleEndian<quint64>(100, data);
    data += 8;
    qToLittleEndian<quint32>(length, data);
    qToLittleEndian<quint16>(2, data + 4);
    qToLittleEndian<quint64>(reference, data + 8);
    qToLittleEndian<quint64>(parent, data + 16);
    qToLittleEndian<quint32>(reason, data + 40);
    qToLittleEndian<quint32>(directory ? 0x10 : 0, data + 52);
    qToLittleEndian<quint16>(name.size() * 2, data + 56);
    qToLittleEndian<quint16>(60, data + 58);
    memcpy(data + 60, name.utf16(), name.size() * 2);
    return result;
}
}

void FileSearchServiceTest::journalReplay()
{
    FileSearchIndex index;
    QString error;
    index.upsert(FileSearchIndex::key(3, 0), 0, QStringLiteral("C:"), true);
    const quint64 directory = (quint64(2) << 48) | 100;
    const quint64 file = (quint64(3) << 48) | 101;
    QVERIFY(index.applyJournalRecords(3, journalBatch(directory, 5, 0x100, QStringLiteral("旧目录"), true), false, &error));
    QVERIFY(index.applyJournalRecords(3, journalBatch(file, directory, 0x100, QStringLiteral("文件.txt"), false), false, &error));
    QVERIFY(index.applyJournalRecords(3, journalBatch(directory, 5, 0x1000, QStringLiteral("旧目录"), true), false, &error));
    QCOMPARE(index.size(), 3);
    QVERIFY(index.applyJournalRecords(3, journalBatch(directory, 5, 0x2000, QStringLiteral("新目录"), true), false, &error));
    QCOMPARE(index.pathOf(FileSearchIndex::key(3, 101)), QStringLiteral("C:\\新目录\\文件.txt"));
    // 无效后缀使整个批次失败，前面的有效记录也不能被提交。
    const QByteArray damaged = journalBatch(file, directory, 0x200, QStringLiteral("文件.txt"), false) + QByteArray(4, 'x');
    QVERIFY(!index.applyJournalRecords(3, damaged, false, &error));
    QCOMPARE(index.size(), 3);
    QVERIFY(index.applyJournalRecords(3, journalBatch(file, directory, 0x300, QStringLiteral("文件.txt"), false), false, &error));
    QCOMPARE(index.size(), 2);
    const quint64 replacement = (quint64(4) << 48) | 101;
    QVERIFY(index.applyJournalRecords(3, journalBatch(replacement, directory, 0x100, QStringLiteral("新文件.txt"), false), false, &error));
    QVERIFY(index.applyJournalRecords(3, journalBatch(file, directory, 0x200, QStringLiteral("文件.txt"), false), false, &error));
    QCOMPARE(index.size(), 3);
    index.dropOrphans();
    QCOMPARE(index.size(), 3);
    QTemporaryDir temporary;
    FileSearchVolume volume;
    volume.driveIndex = 3; volume.mftMode = true; volume.serialNumber = 42; volume.nextUsn = 100;
    QVector<FileSearchVolume> volumes{volume}, loadedVolumes;
    QVERIFY(FileSearchIndex::save(temporary.filePath("index.bin"), volumes, index, &error));
    FileSearchIndex loaded;
    QVERIFY(FileSearchIndex::load(temporary.filePath("index.bin"), &loadedVolumes, &loaded, &error));
    QCOMPARE(loadedVolumes.first().serialNumber, quint64(42));
    QCOMPARE(loaded.record(FileSearchIndex::key(3, 101))->sequence, quint16(4));
}

void FileSearchServiceTest::fastSearchConsistency()
{
    FileSearchIndex index;
    index.upsert(1, 0, QStringLiteral("C:"), true);
    QStringList names;
    for (int i = 0; i < 4000; ++i) {
        const QString name = QStringLiteral("项目_%1_AbC_Σςσ_Ää_文件.%2").arg(i).arg(i % 2 ? "png" : "txt");
        names.append(name);
        index.upsert(i + 2, 1, name, false);
    }
    for (const QString& text : {QStringLiteral("项目_19"), QStringLiteral("abc"), QStringLiteral("σ_ä"), QStringLiteral("不存在"), QStringLiteral("文件")}) {
        int expected = 0, total = 0;
        for (const auto& name : names) if (name.contains(text, Qt::CaseInsensitive)) ++expected;
        const auto query = FileSearchIndex::Query::parse(text);
        index.search(query, 0, 20, &total);
        QCOMPARE(total, expected);
        index.search(query, 20, 20, &total);
        QCOMPARE(total, expected);
    }
    int total = 0;
    index.search(FileSearchIndex::Query::parse(QStringLiteral("ext:png,txt")), 0, 20, &total);
    QCOMPARE(total, 4000);
    index.upsert(9000, 1, QStringLiteral("项目_新文件.txt"), false);
    index.search(FileSearchIndex::Query::parse(QStringLiteral("ext:png,txt")), 0, 20, &total);
    QCOMPARE(total, 4001);
    index.remove({9000});
    index.search(FileSearchIndex::Query::parse(QStringLiteral("ext:png,txt")), 0, 20, &total);
    QCOMPARE(total, 4000);
    int calls = 0;
    const auto cancelled = index.search(FileSearchIndex::Query::parse(QStringLiteral("AbC")), 0, 20, nullptr, nullptr,
        [&calls]() { return ++calls > 2; });
    QVERIFY(cancelled.isEmpty());
    QVERIFY(calls > 2);
}

void FileSearchServiceTest::journalProtocolBounds()
{
    using namespace FileJournalProtocol;
    QVERIFY(valid(Request{Magic, FSCTL_QUERY_USN_JOURNAL, 3, 0}));
    QVERIFY(!valid(Request{Magic, FSCTL_DELETE_USN_JOURNAL, 3, 0}));
    QVERIFY(!valid(Request{Magic, FSCTL_READ_USN_JOURNAL, 3, 4096}));
    QVERIFY(!valid(Request{Magic, FSCTL_QUERY_USN_JOURNAL, 27, 0}));
    QVERIFY(!valid(Request{0, FSCTL_QUERY_USN_JOURNAL, 3, 0}));
}

void FileSearchServiceTest::latestQueryWins()
{
    QTemporaryDir directory, data;
    writeFile(directory.filePath(QStringLiteral("最终查询.txt")), "x");
    qputenv("DESKTOPTOOL_FILESEARCH_ROOTS", directory.path().toLocal8Bit());
    FileSearchService service(data.path(), false);
    qunsetenv("DESKTOPTOOL_FILESEARCH_ROOTS");
    QTRY_COMPARE_WITH_TIMEOUT(service.statusSnapshot().value("phase").toString(), QStringLiteral("ready"), 10000);
    QSignalSpy spy(&service, &FileSearchService::searchFinished);
    for (int i = 0; i < 100; ++i) service.search(QString::number(i), 0, 20, false);
    const int id = service.search(QStringLiteral("最终查询"), 0, 20, false).value("data").toMap().value("requestId").toInt();
    QTRY_VERIFY_WITH_TIMEOUT(!spy.isEmpty() && spy.last().first().toMap().value("requestId").toInt() == id, 10000);
    QCOMPARE(spy.last().first().toMap().value("total").toInt(), 1);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("DesktopToolFileSearchTest"));
    FileSearchServiceTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "FileSearchServiceTest.moc"
