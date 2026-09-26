#include "services/DownloadService.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

namespace {
/** 统一判断服务是否返回成功。 */
bool ok(const QVariantMap& result) { return result.value(QStringLiteral("ok")).toBool(); }

/** 读取服务成功结果里的数据对象。 */
QVariantMap data(const QVariantMap& result) { return result.value(QStringLiteral("data")).toMap(); }

/** 在任务快照中定位指定任务。 */
QVariantMap task(const DownloadService& service, const QString& id)
{
    for (const auto& value : data(service.snapshot()).value(QStringLiteral("items")).toList()) {
        const auto item = value.toMap();
        if (item.value(QStringLiteral("id")).toString() == id)
            return item;
    }
    return {};
}

/** 读取测试目录中的文件内容。 */
QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

/** 向测试目录写入精确字节内容。 */
bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

/** 本机 HTTP 测试服务器，提供慢流、断点和明确的异常响应。 */
class DownloadHttpFixture final : public QObject
{
public:
    /** 保存请求中的断点字段，验证客户端确实发送了续传请求。 */
    struct Request {
        QByteArray path;    ///< 请求的接口路径。
        QByteArray range;   ///< 客户端发送的 Range。
        QByteArray ifRange; ///< 客户端发送的 If-Range。
    };

    /** 启动仅监听本机回环地址的模拟服务器。 */
    DownloadHttpFixture()
        : m_server(this), m_payload(256 * 1024, '\0')
    {
        for (qsizetype index = 0; index < m_payload.size(); ++index)
            m_payload[index] = static_cast<char>(index % 251);
        m_listening = m_server.listen(QHostAddress::LocalHost, 0);
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    m_buffers[socket].append(socket->readAll());
                    if (!m_buffers.value(socket).contains("\r\n\r\n"))
                        return;
                    const QByteArray header = m_buffers.take(socket);
                    socket->disconnect(this);
                    connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                    respond(socket, header);
                });
                connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
                    m_buffers.remove(socket);
                    socket->deleteLater();
                });
            }
        });
    }
    /** 服务器是否成功绑定随机本机端口。 */
    bool listening() const { return m_listening; }
    /** 返回指定模拟接口的绝对下载地址。 */
    QString url(const QString& path) const
    {
        return QStringLiteral("http://127.0.0.1:%1/%2").arg(m_server.serverPort()).arg(path);
    }
    /** 返回测试文件的确定性内容。 */
    QByteArray payload() const { return m_payload; }
    /** 返回已经收到的请求列表。 */
    QList<Request> requests() const { return m_requests; }

private:
    /** 根据请求路径返回成功、续传、重定向或异常响应。 */
    void respond(QTcpSocket* socket, const QByteArray& header)
    {
        Request request;
        request.path = header.split('\n').first().split(' ').value(1);
        for (const auto& raw : header.split('\n')) {
            const QByteArray line = raw.trimmed();
            if (line.toLower().startsWith("range:"))
                request.range = line.mid(6).trimmed();
            if (line.toLower().startsWith("if-range:"))
                request.ifRange = line.mid(9).trimmed();
        }
        m_requests.append(request);
        if (request.path == "/redirect.bin" || request.path == "/unsafe-redirect.bin") {
            const QByteArray location = request.path == "/redirect.bin" ? "/range.bin" : "file:///outside.bin";
            socket->write("HTTP/1.1 302 Found\r\nLocation: " + location + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            socket->disconnectFromHost();
            return;
        }
        if (request.path == "/error.bin") {
            socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 3\r\nConnection: close\r\n\r\nbad");
            socket->disconnectFromHost();
            return;
        }
        const bool honorRange = !request.range.isEmpty() && request.path != "/no-range.bin";
        if (!request.range.isEmpty() && request.path == "/range-416.bin") {
            socket->write("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" + QByteArray::number(m_payload.size())
                + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            socket->disconnectFromHost();
            return;
        }
        const qint64 offset = honorRange ? request.range.mid(6).split('-').first().toLongLong() : 0;
        const QByteArray body = request.path == "/zero.bin" ? QByteArray() : m_payload.mid(offset);
        QByteArray response = honorRange ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
        if (request.path != "/unknown.bin")
            response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        if (request.path != "/no-validator.bin")
            response += "ETag: " + QByteArray(request.path == "/changed-validator.bin" && honorRange ? "\"changed\"" : "\"test-v1\"") + "\r\n";
        if (honorRange) {
            const qint64 first = offset + (request.path == "/bad-range.bin" ? 1 : 0);
            response += "Content-Range: bytes " + QByteArray::number(first) + '-'
                + QByteArray::number(m_payload.size() - 1) + '/'
                + (request.path == "/unknown-range.bin" ? QByteArray("*") : QByteArray::number(m_payload.size())) + "\r\n";
        }
        if (request.path == "/reserved.bin")
            response += "Content-Disposition: attachment; filename=\"../../CON.txt\"\r\n";
        if (request.path == "/encoded.bin")
            response += "Content-Encoding: gzip\r\n";
        response += "Content-Type: application/octet-stream\r\nConnection: close\r\n\r\n";
        socket->write(response);
        if (request.path == "/cut.bin") {
            socket->write(body.left(1024));
            socket->disconnectFromHost();
            return;
        }
        if (body.isEmpty()) {
            socket->disconnectFromHost();
            return;
        }
        auto sent = std::make_shared<qint64>(0);
        auto* timer = new QTimer(socket);
        timer->setInterval(10);
        connect(timer, &QTimer::timeout, socket, [socket, timer, sent, body] {
            if (socket->state() != QAbstractSocket::ConnectedState) {
                timer->stop();
                return;
            }
            const QByteArray chunk = body.mid(*sent, 4096);
            socket->write(chunk);
            *sent += chunk.size();
            if (*sent >= body.size()) {
                timer->stop();
                socket->disconnectFromHost();
            }
        });
        timer->start();
    }

    QTcpServer m_server;                  ///< 仅供本测试使用的本机服务器。
    QByteArray m_payload;                 ///< 各正常接口共同返回的确定性文件。
    bool m_listening = false;             ///< 本机监听是否成功。
    QHash<QTcpSocket*, QByteArray> m_buffers; ///< 尚未收齐请求头的连接缓存。
    QList<Request> m_requests;            ///< 用于断言 Range 行为的请求历史。
};
}

/** 验证真实异步 Qt 网络下载及持久化恢复，不访问外部网络或真实用户数据。 */
class DownloadServiceTest final : public QObject
{
    Q_OBJECT
private slots:
    /** 多个任务可以同时运行，完成文件不会覆盖已存在的同名文件。 */
    void downloadsConcurrentlyWithoutOverwriting();
    /** 暂停后保留内容，继续时发送 Range 与 If-Range 并正确拼接。 */
    void pausesAndResumesWithValidatedRange();
    /** 不支持断点或缺少校验标识时安全从头下载。 */
    void restartsSafelyWhenRangeIsUnavailable();
    /** 取消清理专属部分文件，移除记录不会影响其他任务。 */
    void cancelsAndRemovesOnlyOwnedPartialFiles();
    /** 目录失效时回退默认目录，并清理响应中的危险文件名。 */
    void fallsBackDirectoryAndSanitizesFileName();
    /** 网络错误、异常范围和危险重定向均不能生成完成文件。 */
    void rejectsInvalidResponsesAndInterruptedTransfers();
    /** 未知长度、零字节文件和安全重定向均能正常结束。 */
    void handlesUnknownLengthEmptyAndRedirectedFiles();
    /** 重建服务后把活动任务恢复为暂停，并使用真实部分文件大小继续。 */
    void restoresInterruptedTaskFromIndex();
    /** 损坏索引及恶意路径不能触发删除或覆盖。 */
    void protectsDamagedHistoryAndUnsafePaths();
    /** 取消无法清理部分文件时必须明确失败。 */
    void reportsPartialCleanupFailure();
    /** 416 断点失败最多重试一次完整请求，不能永远卡在无效断点。 */
    void restartsOnceAfterUnsatisfiableRange();
    /** 索引保存失败时取消和移除都保留原部分文件。 */
    void preservesPartialWhenIndexCannotBeSaved();
};

void DownloadServiceTest::downloadsConcurrentlyWithoutOverwriting()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    DownloadHttpFixture server;
    QVERIFY(server.listening());
    QVERIFY(writeFile(root.filePath(QStringLiteral("range.bin")), "original"));
    DownloadService service(root.filePath(QStringLiteral("data")));
    QSignalSpy changes(&service, &DownloadService::changed);
    const auto first = service.createTask(server.url(QStringLiteral("range.bin")), root.path(), root.path());
    const auto second = service.createTask(server.url(QStringLiteral("range.bin")), root.path(), root.path());
    QVERIFY(ok(first));
    QVERIFY(ok(second));
    const QString firstId = data(first).value(QStringLiteral("id")).toString();
    const QString secondId = data(second).value(QStringLiteral("id")).toString();
    QCOMPARE(data(service.snapshot()).value(QStringLiteral("activeCount")).toInt(), 2);
    QVERIFY(service.hasActiveTasks());
    QTRY_COMPARE_WITH_TIMEOUT(task(service, firstId).value(QStringLiteral("status")).toString(), QStringLiteral("completed"), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(task(service, secondId).value(QStringLiteral("status")).toString(), QStringLiteral("completed"), 10000);
    const auto firstTask = task(service, firstId);
    const auto secondTask = task(service, secondId);
    QVERIFY(firstTask.value(QStringLiteral("filePath")) != secondTask.value(QStringLiteral("filePath")));
    QCOMPARE(readFile(firstTask.value(QStringLiteral("filePath")).toString()), server.payload());
    QCOMPARE(readFile(secondTask.value(QStringLiteral("filePath")).toString()), server.payload());
    QCOMPARE(readFile(root.filePath(QStringLiteral("range.bin"))), QByteArray("original"));
    QCOMPARE(firstTask.value(QStringLiteral("bytesReceived")).toLongLong(), qint64(server.payload().size()));
    QCOMPARE(firstTask.value(QStringLiteral("totalBytes")).toLongLong(), qint64(server.payload().size()));
    QVERIFY(!service.hasActiveTasks());
    QVERIFY(changes.count() > 2);
    QVERIFY(QDir(root.path()).entryList({QStringLiteral("*.part")}, QDir::Files | QDir::Hidden).isEmpty());
}

void DownloadServiceTest::pausesAndResumesWithValidatedRange()
{
    QTemporaryDir root;
    DownloadHttpFixture server;
    QVERIFY(root.isValid() && server.listening());
    DownloadService service(root.filePath(QStringLiteral("data")));
    const QString id = data(service.createTask(server.url(QStringLiteral("range.bin")), root.path(), root.path())).value(QStringLiteral("id")).toString();
    QVERIFY(!id.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(task(service, id).value(QStringLiteral("bytesReceived")).toLongLong() >= 8192, 5000);
    const auto paused = service.pauseTask(id);
    QVERIFY(ok(paused));
    const qint64 offset = data(paused).value(QStringLiteral("bytesReceived")).toLongLong();
    QCOMPARE(data(paused).value(QStringLiteral("status")).toString(), QStringLiteral("paused"));
    QVERIFY(offset < server.payload().size());
    const QString part = root.filePath(QStringLiteral(".desktoptool-") + id + QStringLiteral(".part"));
    QCOMPARE(QFileInfo(part).size(), offset);
    QTest::qWait(50);
    QCOMPARE(task(service, id).value(QStringLiteral("bytesReceived")).toLongLong(), offset);
    QVERIFY(ok(service.resumeTask(id)));
    QTRY_COMPARE_WITH_TIMEOUT(task(service, id).value(QStringLiteral("status")).toString(), QStringLiteral("completed"), 10000);
    QCOMPARE(server.requests().last().range, QByteArray("bytes=") + QByteArray::number(offset) + '-');
    QCOMPARE(server.requests().last().ifRange, QByteArray("\"test-v1\""));
    QCOMPARE(readFile(task(service, id).value(QStringLiteral("filePath")).toString()), server.payload());
    QVERIFY(!QFileInfo::exists(part));
}

void DownloadServiceTest::restartsSafelyWhenRangeIsUnavailable()
{
    QTemporaryDir root;
    DownloadHttpFixture server;
    QVERIFY(root.isValid() && server.listening());
    DownloadService service(root.filePath(QStringLiteral("data")));
    for (const QString& name : {QStringLiteral("no-range.bin"), QStringLiteral("no-validator.bin")}) {
        const QString id = data(service.createTask(server.url(name), root.path(), root.path())).value(QStringLiteral("id")).toString();
        QTRY_VERIFY_WITH_TIMEOUT(task(service, id).value(QStringLiteral("bytesReceived")).toLongLong() >= 8192, 5000);
        QVERIFY(ok(service.pauseTask(id)));
        QVERIFY(ok(service.resumeTask(id)));
        QTRY_COMPARE_WITH_TIMEOUT(task(service, id).value(QStringLiteral("status")).toString(), QStringLiteral("completed"), 10000);
        QCOMPARE(readFile(task(service, id).value(QStringLiteral("filePath")).toString()), server.payload());
        QVERIFY(!task(service, id).value(QStringLiteral("warning")).toString().isEmpty());
        if (name == QStringLiteral("no-validator.bin"))
            QVERIFY(server.requests().last().range.isEmpty());
    }
}

void DownloadServiceTest::cancelsAndRemovesOnlyOwnedPartialFiles()
{
    QTemporaryDir root;
    DownloadHttpFixture server;
    QVERIFY(root.isValid() && server.listening());
    DownloadService service(root.filePath(QStringLiteral("data")));
    const QString id = data(service.createTask(server.url(QStringLiteral("range.bin")), root.path(), root.path())).value(QStringLiteral("id")).toString();
    QTRY_VERIFY_WITH_TIMEOUT(task(service, id).value(QStringLiteral("bytesReceived")).toLongLong() >= 4096, 5000);
    QVERIFY(!ok(service.removeTask(id)));
    QVERIFY(writeFile(root.filePath(QStringLiteral("unrelated.part")), "preserve"));
    QVERIFY(ok(service.cancelTask(id)));
    QCOMPARE(task(service, id).value(QStringLiteral("status")).toString(), QStringLiteral("cancelled"));
    QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral(".desktoptool-") + id + QStringLiteral(".part"))));
    QCOMPARE(readFile(root.filePath(QStringLiteral("unrelated.part"))), QByteArray("preserve"));
    QVERIFY(!ok(service.resumeTask(id)));
    QVERIFY(ok(service.removeTask(id)));
    QVERIFY(data(service.snapshot()).value(QStringLiteral("items")).toList().isEmpty());
}

void DownloadServiceTest::fallsBackDirectoryAndSanitizesFileName()
{
    QTemporaryDir root;
    DownloadHttpFixture server;
    QVERIFY(root.isValid() && server.listening());
    DownloadService service(root.filePath(QStringLiteral("data")));
    QVERIFY(!ok(service.createTask(QStringLiteral("file:///secret.txt"), root.path(), root.path())));
    QVERIFY(!ok(service.createTask(QStringLiteral("http://user:password@localhost/file"), root.path(), root.path())));
    const auto created = service.createTask(server.url(QStringLiteral("reserved.bin")), root.filePath(QStringLiteral("missing/subfolder")), root.path());
    QVERIFY(ok(created));
    const QString id = data(created).value(QStringLiteral("id")).toString();
    QCOMPARE(data(created).value(QStringLiteral("directory")).toString(), QFileInfo(root.path()).canonicalFilePath());
    QVERIFY(!data(created).value(QStringLiteral("warning")).toString().isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(task(service, id).value(QStringLiteral("status")).toString(), QStringLiteral("completed"), 10000);
    QCOMPARE(task(service, id).value(QStringLiteral("fileName")).toString(), QStringLiteral("_CON.txt"));
    QCOMPARE(readFile(task(service, id).value(QStringLiteral("filePath")).toString()), server.payload());
}

void DownloadServiceTest::rejectsInvalidResponsesAndInterruptedTransfers()
{
    QTemporaryDir root;
    DownloadHttpFixture server;
    QVERIFY(root.isValid() && server.listening());
    DownloadService service(root.filePath(QStringLiteral("data")));
    for (const QString& path : {QStringLiteral("error.bin"), QStringLiteral("cut.bin"), QStringLiteral("unsafe-redirect.bin"), QStringLiteral("encoded.bin")}) {
        const QString id = data(service.createTask(server.url(path), root.path(), root.path())).value(QStringLiteral("id")).toString();
        QTRY_COMPARE_WITH_TIMEOUT(task(service, id).value(QStringLiteral("status")).toString(), QStringLiteral("failed"), 10000);
        QVERIFY(!task(service, id).value(QStringLiteral("error")).toString().isEmpty());
        QVERIFY(!QFileInfo::exists(task(service, id).value(QStringLiteral("filePath")).toString()));
    }
    for (const QString& path : {QStringLiteral("bad-range.bin"), QStringLiteral("changed-validator.bin"), QStringLiteral("unknown-range.bin")}) {
        const QString id = data(service.createTask(server.url(path), root.path(), root.path())).value(QStringLiteral("id")).toString();
        QTRY_VERIFY_WITH_TIMEOUT(task(service, id).value(QStringLiteral("bytesReceived")).toLongLong() >= 4096, 5000);
        QVERIFY(ok(service.pauseTask(id)));
        const QString part = root.filePath(QStringLiteral(".desktoptool-") + id + QStringLiteral(".part"));
        const QByteArray before = readFile(part);
        QVERIFY(ok(service.resumeTask(id)));
        QTRY_COMPARE_WITH_TIMEOUT(task(service, id).value(QStringLiteral("status")).toString(), QStringLiteral("failed"), 10000);
        QCOMPARE(readFile(part), before);
    }
}

void DownloadServiceTest::handlesUnknownLengthEmptyAndRedirectedFiles()
{
    QTemporaryDir root;
    DownloadHttpFixture server;
    QVERIFY(root.isValid() && server.listening());
    DownloadService service(root.filePath(QStringLiteral("data")));
    for (const QString& path : {QStringLiteral("unknown.bin"), QStringLiteral("zero.bin"), QStringLiteral("redirect.bin")}) {
        const QString id = data(service.createTask(server.url(path), root.path(), root.path())).value(QStringLiteral("id")).toString();
        QTRY_COMPARE_WITH_TIMEOUT(task(service, id).value(QStringLiteral("status")).toString(), QStringLiteral("completed"), 10000);
        const QByteArray expected = path == QStringLiteral("zero.bin") ? QByteArray() : server.payload();
        QCOMPARE(readFile(task(service, id).value(QStringLiteral("filePath")).toString()), expected);
    }
}

void DownloadServiceTest::restoresInterruptedTaskFromIndex()
{
    QTemporaryDir root;
    DownloadHttpFixture server;
    QVERIFY(root.isValid() && server.listening());
    const QString dataRoot = root.filePath(QStringLiteral("data"));
    QString id;
    qint64 offset = 0;
    {
        DownloadService service(dataRoot);
        id = data(service.createTask(server.url(QStringLiteral("range.bin")), root.path(), root.path())).value(QStringLiteral("id")).toString();
        QTRY_VERIFY_WITH_TIMEOUT(task(service, id).value(QStringLiteral("bytesReceived")).toLongLong() >= 8192, 5000);
        offset = task(service, id).value(QStringLiteral("bytesReceived")).toLongLong();
    }
    DownloadService restored(dataRoot);
    QCOMPARE(task(restored, id).value(QStringLiteral("status")).toString(), QStringLiteral("paused"));
    QCOMPARE(task(restored, id).value(QStringLiteral("bytesReceived")).toLongLong(), offset);
    QVERIFY(!restored.hasActiveTasks());
    QVERIFY(ok(restored.resumeTask(id)));
    QTRY_COMPARE_WITH_TIMEOUT(task(restored, id).value(QStringLiteral("status")).toString(), QStringLiteral("completed"), 10000);
    QCOMPARE(server.requests().last().range, QByteArray("bytes=") + QByteArray::number(offset) + '-');
    QCOMPARE(readFile(task(restored, id).value(QStringLiteral("filePath")).toString()), server.payload());
}

void DownloadServiceTest::protectsDamagedHistoryAndUnsafePaths()
{
    QTemporaryDir root;
    DownloadHttpFixture server;
    QVERIFY(root.isValid() && server.listening());
    const QString index = root.filePath(QStringLiteral("download-tasks.v1.json"));
    const QByteArray broken("{broken download index");
    QVERIFY(writeFile(index, broken));
    {
        DownloadService service(root.path());
        QVERIFY(!ok(service.snapshot()));
        QVERIFY(!ok(service.createTask(server.url(QStringLiteral("range.bin")), root.path(), root.path())));
    }
    QCOMPARE(readFile(index), broken);
    const QByteArray malicious = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
        {QStringLiteral("items"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("../unrelated")},
            {QStringLiteral("url"), server.url(QStringLiteral("range.bin"))}, {QStringLiteral("directory"), root.path()},
            {QStringLiteral("fileName"), QStringLiteral("../outside.bin")}, {QStringLiteral("status"), QStringLiteral("paused")},
            {QStringLiteral("bytesReceived"), 0}, {QStringLiteral("totalBytes"), -1}}}}}).toJson();
    QVERIFY(writeFile(index, malicious));
    QVERIFY(writeFile(root.filePath(QStringLiteral("unrelated.part")), "protected"));
    {
        DownloadService service(root.path());
        QVERIFY(!ok(service.snapshot()));
        QVERIFY(!ok(service.cancelTask(QStringLiteral("../unrelated"))));
    }
    QCOMPARE(readFile(index), malicious);
    QCOMPARE(readFile(root.filePath(QStringLiteral("unrelated.part"))), QByteArray("protected"));
}

void DownloadServiceTest::reportsPartialCleanupFailure()
{
    QTemporaryDir root;
    DownloadHttpFixture server;
    QVERIFY(root.isValid() && server.listening());
    DownloadService service(root.filePath(QStringLiteral("data")));
    const QString id = data(service.createTask(server.url(QStringLiteral("range.bin")), root.path(), root.path())).value(QStringLiteral("id")).toString();
    QVERIFY(ok(service.pauseTask(id)));
    const QString partial = root.filePath(QStringLiteral(".desktoptool-") + id + QStringLiteral(".part"));
    QVERIFY(QFile::remove(partial));
    QVERIFY(QDir().mkdir(partial));
    QVERIFY(!ok(service.cancelTask(id)));
    QCOMPARE(task(service, id).value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
    QVERIFY(QFileInfo(partial).isDir());
    QVERIFY(!ok(service.removeTask(id)));
}

void DownloadServiceTest::restartsOnceAfterUnsatisfiableRange()
{
    QTemporaryDir root;
    DownloadHttpFixture server;
    QVERIFY(root.isValid() && server.listening());
    DownloadService service(root.filePath(QStringLiteral("data")));
    const QString id = data(service.createTask(server.url(QStringLiteral("range-416.bin")), root.path(), root.path())).value(QStringLiteral("id")).toString();
    QTRY_VERIFY_WITH_TIMEOUT(task(service, id).value(QStringLiteral("bytesReceived")).toLongLong() >= 8192, 5000);
    QVERIFY(ok(service.pauseTask(id)));
    QVERIFY(ok(service.resumeTask(id)));
    QTRY_COMPARE_WITH_TIMEOUT(task(service, id).value(QStringLiteral("status")).toString(), QStringLiteral("completed"), 10000);
    QCOMPARE(server.requests().size(), 3);
    QVERIFY(!server.requests().at(1).range.isEmpty());
    QVERIFY(server.requests().last().range.isEmpty());
    QCOMPARE(readFile(task(service, id).value(QStringLiteral("filePath")).toString()), server.payload());
}

void DownloadServiceTest::preservesPartialWhenIndexCannotBeSaved()
{
    QTemporaryDir root;
    DownloadHttpFixture server;
    QVERIFY(root.isValid() && server.listening());
    const QString dataRoot = root.filePath(QStringLiteral("data"));
    DownloadService service(dataRoot);
    const QString pausedId = data(service.createTask(server.url(QStringLiteral("range.bin")), root.path(), root.path())).value(QStringLiteral("id")).toString();
    const QString failedId = data(service.createTask(server.url(QStringLiteral("cut.bin")), root.path(), root.path())).value(QStringLiteral("id")).toString();
    QTRY_VERIFY_WITH_TIMEOUT(task(service, pausedId).value(QStringLiteral("bytesReceived")).toLongLong() >= 4096, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(task(service, failedId).value(QStringLiteral("status")).toString(), QStringLiteral("failed"), 5000);
    QVERIFY(ok(service.pauseTask(pausedId)));
    const QString pausedPart = root.filePath(QStringLiteral(".desktoptool-") + pausedId + QStringLiteral(".part"));
    const QString failedPart = root.filePath(QStringLiteral(".desktoptool-") + failedId + QStringLiteral(".part"));
    const QByteArray pausedBefore = readFile(pausedPart);
    const QByteArray failedBefore = readFile(failedPart);
    QVERIFY(!pausedBefore.isEmpty());
    QVERIFY(!failedBefore.isEmpty());
    const QString index = QDir(dataRoot).filePath(QStringLiteral("download-tasks.v1.json"));
    QVERIFY(QFile::remove(index));
    QVERIFY(QDir().mkdir(index));
    QVERIFY(!ok(service.cancelTask(pausedId)));
    QVERIFY(!ok(service.removeTask(failedId)));
    QCOMPARE(readFile(pausedPart), pausedBefore);
    QCOMPARE(readFile(failedPart), failedBefore);
    QCOMPARE(task(service, pausedId).value(QStringLiteral("status")).toString(), QStringLiteral("paused"));
    QCOMPARE(task(service, failedId).value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
}

QTEST_GUILESS_MAIN(DownloadServiceTest)
#include "DownloadServiceTest.moc"
