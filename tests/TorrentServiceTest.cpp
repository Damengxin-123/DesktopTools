#include "services/TorrentService.h"
#include "TorrentTestPeer.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

namespace {
/** 判断服务的统一结果是否成功。 */
bool ok(const QVariantMap& result) { return result.value(QStringLiteral("ok")).toBool(); }

/** 提取服务成功结果中的对象数据。 */
QVariantMap data(const QVariantMap& result) { return result.value(QStringLiteral("data")).toMap(); }

/** 从快照查找指定任务，任务已移除时返回空对象。 */
QVariantMap task(const TorrentService& service, const QString& id)
{
    for (const QVariant& value : data(service.snapshot()).value(QStringLiteral("items")).toList()) {
        const QVariantMap item = value.toMap();
        if (item.value(QStringLiteral("id")).toString() == id) return item;
    }
    return {};
}

/** 将完整任务状态写入断言消息，便于识别网络和元数据失败。 */
QString describeTask(const TorrentService& service, const QString& id)
{
    return QString::fromUtf8(QJsonDocument::fromVariant(task(service, id)).toJson(QJsonDocument::Compact));
}

/** 驱动 Qt 事件循环等待指定状态，遇到服务失败时立即返回。 */
bool waitForStatus(TorrentService& service, const QString& id, const QString& expected, int timeout = 15000)
{
    QElapsedTimer elapsed;
    elapsed.start();
    do {
        const QString status = task(service, id).value(QStringLiteral("status")).toString();
        if (status == expected) return true;
        if (status == QStringLiteral("failed") || status.isEmpty()) return false;
        QTest::qWait(20);
    } while (elapsed.elapsed() < timeout);
    return false;
}

/** 从权威元数据清单获取指定相对路径的原始文件索引。 */
int fileIndex(const TorrentService& service, const QString& id, const QString& path)
{
    for (const QVariant& value : data(service.files(id)).value(QStringLiteral("files")).toList()) {
        const QVariantMap file = value.toMap();
        if (file.value(QStringLiteral("path")).toString() == path)
            return file.value(QStringLiteral("index")).toInt();
    }
    return -1;
}

/** 读取测试拥有的文件内容，用于逐字节比较结果。 */
QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

/** 创建测试拥有的普通文件，模拟用户在任务目录中新增资料。 */
bool writeFile(const QString& path, const QByteArray& bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        && file.write(bytes) == bytes.size() && file.flush();
}

/** 等待取消后的异步文件句柄关闭，再移除终态记录。 */
bool removeWhenReady(TorrentService& service, const QString& id)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 10000) {
        if (task(service, id).isEmpty() || ok(service.removeTask(id))) return true;
        QTest::qWait(20);
    }
    return false;
}
}

/** 使用真实回环做种节点验证磁力任务的选择、恢复和清理边界。 */
class TorrentServiceTest final : public QObject
{
    Q_OBJECT
private slots:
    /** 元数据解析完成后，未确认前不得创建两份载荷文件。 */
    void metadataDoesNotCreatePayload();
    /** 空列表、越界、重复和非整数索引不得改变未确认状态。 */
    void invalidSelectionsAreRejected();
    /** 只选择第一份文件时，逐字节完成且未选文件始终不存在。 */
    void downloadsOnlySelectedFile();
    /** 等待选择的任务重启后保留元数据，继续操作也不自动下载。 */
    void awaitingSelectionSurvivesRestart();
    /** 已确认任务暂停重启后保持选择，只在用户继续后完成。 */
    void confirmedPauseSurvivesRestart();
    /** 取消清理只删除任务载荷，保留同目录内用户新增的文件。 */
    void cancellationPreservesUserFiles();
};

void TorrentServiceTest::metadataDoesNotCreatePayload()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString downloads = QDir(root.path()).filePath(QStringLiteral("downloads"));
    QVERIFY(QDir().mkpath(downloads));
    TorrentTestPeer peer(QDir(root.path()).filePath(QStringLiteral("seed")));
    QVERIFY2(peer.isValid(), qPrintable(peer.error()));
    TorrentService service(QDir(root.path()).filePath(QStringLiteral("data")));
    const QVariantMap created = service.createTask(peer.magnet(), downloads);
    QVERIFY2(ok(created), qPrintable(created.value(QStringLiteral("error")).toString()));
    const QString id = data(created).value(QStringLiteral("id")).toString();
    QVERIFY2(waitForStatus(service, id, QStringLiteral("awaiting_selection")), qPrintable(describeTask(service, id)));
    const QVariantMap current = task(service, id);
    const QString directory = current.value(QStringLiteral("directory")).toString();
    QCOMPARE(data(service.files(id)).value(QStringLiteral("files")).toList().size(), 2);
    QCOMPARE(current.value(QStringLiteral("bytesReceived")).toLongLong(), 0);
    QVERIFY(!current.value(QStringLiteral("selectionConfirmed")).toBool());
    QVERIFY(!service.hasActiveTasks());
    QTest::qWait(300);
    QVERIFY(!QFileInfo::exists(QDir(directory).filePath(QStringLiteral("bundle/first.bin"))));
    QVERIFY(!QFileInfo::exists(QDir(directory).filePath(QStringLiteral("bundle/second.bin"))));
    QCOMPARE(task(service, id).value(QStringLiteral("bytesReceived")).toLongLong(), 0);
}

void TorrentServiceTest::invalidSelectionsAreRejected()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    TorrentTestPeer peer(QDir(root.path()).filePath(QStringLiteral("seed")));
    QVERIFY2(peer.isValid(), qPrintable(peer.error()));
    TorrentService service(QDir(root.path()).filePath(QStringLiteral("data")));
    const auto created = service.createTask(peer.magnet(), root.path());
    QVERIFY(ok(created));
    const QString id = data(created).value(QStringLiteral("id")).toString();
    QVERIFY2(waitForStatus(service, id, QStringLiteral("awaiting_selection")), qPrintable(describeTask(service, id)));
    const int first = fileIndex(service, id, QStringLiteral("bundle/first.bin"));
    QVERIFY(first >= 0);
    const QList<QVariantList> invalid{
        QVariantList{}, QVariantList{-1}, QVariantList{999}, QVariantList{first, first},
        QVariantList{QString::number(first)}, QVariantList{true}, QVariantList{0.5}
    };
    for (const QVariantList& selection : invalid) {
        const QVariantMap result = service.confirmFiles(id, selection);
        QVERIFY(!ok(result));
        QVERIFY(!result.value(QStringLiteral("error")).toString().isEmpty());
        const QVariantMap current = task(service, id);
        QCOMPARE(current.value(QStringLiteral("status")).toString(), QStringLiteral("awaiting_selection"));
        QVERIFY(!current.value(QStringLiteral("selectionConfirmed")).toBool());
        QCOMPARE(current.value(QStringLiteral("selectedCount")).toInt(), 0);
        QCOMPARE(current.value(QStringLiteral("bytesReceived")).toLongLong(), 0);
    }
    const QString directory = task(service, id).value(QStringLiteral("directory")).toString();
    QVERIFY(!QFileInfo::exists(QDir(directory).filePath(QStringLiteral("bundle/first.bin"))));
}

void TorrentServiceTest::downloadsOnlySelectedFile()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString source = QDir(root.path()).filePath(QStringLiteral("seed"));
    TorrentTestPeer peer(source);
    QVERIFY2(peer.isValid(), qPrintable(peer.error()));
    TorrentService service(QDir(root.path()).filePath(QStringLiteral("data")));
    const auto created = service.createTask(peer.magnet(), root.path());
    QVERIFY(ok(created));
    const QString id = data(created).value(QStringLiteral("id")).toString();
    QVERIFY2(waitForStatus(service, id, QStringLiteral("awaiting_selection")), qPrintable(describeTask(service, id)));
    const int first = fileIndex(service, id, QStringLiteral("bundle/first.bin"));
    QVERIFY(first >= 0);
    const auto confirmed = service.confirmFiles(id, QVariantList{first});
    QVERIFY2(ok(confirmed), qPrintable(confirmed.value(QStringLiteral("error")).toString()));
    QVERIFY2(waitForStatus(service, id, QStringLiteral("completed"), 20000), qPrintable(describeTask(service, id)));
    const QVariantMap current = task(service, id);
    const QString directory = current.value(QStringLiteral("directory")).toString();
    QCOMPARE(current.value(QStringLiteral("selectedCount")).toInt(), 1);
    QCOMPARE(current.value(QStringLiteral("totalBytes")).toLongLong(), 256 * 1024);
    QCOMPARE(current.value(QStringLiteral("bytesReceived")).toLongLong(), 256 * 1024);
    const QByteArray expected = readFile(QDir(source).filePath(QStringLiteral("bundle/first.bin")));
    QCOMPARE(expected.size(), 256 * 1024);
    QCOMPARE(readFile(QDir(directory).filePath(QStringLiteral("bundle/first.bin"))), expected);
    QVERIFY(!QFileInfo::exists(QDir(directory).filePath(QStringLiteral("bundle/second.bin"))));
    QVERIFY(!service.hasActiveTasks());
}

void TorrentServiceTest::awaitingSelectionSurvivesRestart()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    TorrentTestPeer peer(QDir(root.path()).filePath(QStringLiteral("seed")));
    QVERIFY2(peer.isValid(), qPrintable(peer.error()));
    const QString dataRoot = QDir(root.path()).filePath(QStringLiteral("data"));
    auto service = std::make_unique<TorrentService>(dataRoot);
    const auto created = service->createTask(peer.magnet(), root.path());
    QVERIFY(ok(created));
    const QString id = data(created).value(QStringLiteral("id")).toString();
    QVERIFY2(waitForStatus(*service, id, QStringLiteral("awaiting_selection")), qPrintable(describeTask(*service, id)));
    const QString directory = task(*service, id).value(QStringLiteral("directory")).toString();
    const QVariantList before = data(service->files(id)).value(QStringLiteral("files")).toList();
    service.reset();
    service = std::make_unique<TorrentService>(dataRoot);
    QCOMPARE(task(*service, id).value(QStringLiteral("status")).toString(), QStringLiteral("awaiting_selection"));
    QCOMPARE(data(service->files(id)).value(QStringLiteral("files")).toList(), before);
    QVERIFY(!service->hasActiveTasks());
    QVERIFY(ok(service->resumeTask(id)));
    QTest::qWait(300);
    QCOMPARE(task(*service, id).value(QStringLiteral("status")).toString(), QStringLiteral("awaiting_selection"));
    QVERIFY(!task(*service, id).value(QStringLiteral("selectionConfirmed")).toBool());
    QVERIFY(!service->hasActiveTasks());
    QVERIFY(!QFileInfo::exists(QDir(directory).filePath(QStringLiteral("bundle/first.bin"))));
    QVERIFY(!QFileInfo::exists(QDir(directory).filePath(QStringLiteral("bundle/second.bin"))));
}

void TorrentServiceTest::confirmedPauseSurvivesRestart()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString source = QDir(root.path()).filePath(QStringLiteral("seed"));
    TorrentTestPeer peer(source, 17);
    QVERIFY2(peer.isValid(), qPrintable(peer.error()));
    const QString dataRoot = QDir(root.path()).filePath(QStringLiteral("data"));
    auto service = std::make_unique<TorrentService>(dataRoot);
    const auto created = service->createTask(peer.magnet(), root.path());
    QVERIFY(ok(created));
    const QString id = data(created).value(QStringLiteral("id")).toString();
    QVERIFY2(waitForStatus(*service, id, QStringLiteral("awaiting_selection")), qPrintable(describeTask(*service, id)));
    const int first = fileIndex(*service, id, QStringLiteral("bundle/first.bin"));
    QVERIFY(first >= 0);
    QVERIFY(ok(service->confirmFiles(id, QVariantList{first})));
    // 不进入事件循环就立即暂停，使断言不依赖本机传输速度。
    QVERIFY(ok(service->pauseTask(id)));
    QCOMPARE(task(*service, id).value(QStringLiteral("status")).toString(), QStringLiteral("paused"));
    const QString directory = task(*service, id).value(QStringLiteral("directory")).toString();
    service.reset();
    service = std::make_unique<TorrentService>(dataRoot);
    const QVariantMap restored = task(*service, id);
    QCOMPARE(restored.value(QStringLiteral("status")).toString(), QStringLiteral("paused"));
    QVERIFY(restored.value(QStringLiteral("selectionConfirmed")).toBool());
    QCOMPARE(restored.value(QStringLiteral("selectedCount")).toInt(), 1);
    QVERIFY(!service->hasActiveTasks());
    const QString selectedPath = QDir(directory).filePath(QStringLiteral("bundle/first.bin"));
    const QByteArray pausedBytes = readFile(selectedPath);
    QTest::qWait(150);
    QCOMPARE(readFile(selectedPath), pausedBytes);
    QVERIFY(ok(service->resumeTask(id)));
    QVERIFY2(waitForStatus(*service, id, QStringLiteral("completed"), 20000), qPrintable(describeTask(*service, id)));
    QCOMPARE(readFile(selectedPath), readFile(QDir(source).filePath(QStringLiteral("bundle/first.bin"))));
    QVERIFY(!QFileInfo::exists(QDir(directory).filePath(QStringLiteral("bundle/second.bin"))));
}

void TorrentServiceTest::cancellationPreservesUserFiles()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    TorrentTestPeer peer(QDir(root.path()).filePath(QStringLiteral("seed")));
    QVERIFY2(peer.isValid(), qPrintable(peer.error()));
    TorrentService service(QDir(root.path()).filePath(QStringLiteral("data")));
    const auto created = service.createTask(peer.magnet(), root.path());
    QVERIFY(ok(created));
    const QString id = data(created).value(QStringLiteral("id")).toString();
    QVERIFY2(waitForStatus(service, id, QStringLiteral("awaiting_selection")), qPrintable(describeTask(service, id)));
    const int first = fileIndex(service, id, QStringLiteral("bundle/first.bin"));
    QVERIFY(first >= 0);
    const QString directory = task(service, id).value(QStringLiteral("directory")).toString();
    const QString extra = QDir(directory).filePath(QStringLiteral("bundle/user-created.txt"));
    const QByteArray extraBytes("user-owned content must survive cancellation\n");
    QVERIFY(writeFile(extra, extraBytes));
    QVERIFY(ok(service.confirmFiles(id, QVariantList{first})));
    QVERIFY(ok(service.pauseTask(id)));
    const auto cancelled = service.cancelTask(id);
    QVERIFY2(ok(cancelled), qPrintable(cancelled.value(QStringLiteral("error")).toString()));
    QCOMPARE(task(service, id).value(QStringLiteral("status")).toString(), QStringLiteral("cancelled"));
    QVERIFY2(removeWhenReady(service, id), qPrintable(describeTask(service, id)));
    QCOMPARE(readFile(extra), extraBytes);
    QVERIFY(QFileInfo(directory).isDir());
    QVERIFY(!QFileInfo::exists(QDir(directory).filePath(QStringLiteral("bundle/first.bin"))));
    QVERIFY(!QFileInfo::exists(QDir(directory).filePath(QStringLiteral("bundle/second.bin"))));
    QVERIFY(!service.hasActiveTasks());
}

QTEST_GUILESS_MAIN(TorrentServiceTest)
#include "TorrentServiceTest.moc"
