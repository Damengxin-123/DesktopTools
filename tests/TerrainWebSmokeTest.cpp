#include "app/TerrainEditWindow.h"
#include "app/InterfaceLanguage.h"
#include "services/TerrainService.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <cstring>

// 在真实 WebEngine + WebChannel 环境中驱动地形编辑页面：
// 数据加载、程序化笔划、撤销/重做、保存往返与 16 位 PNG 导出全链路验证。
class TerrainWebSmokeTest final : public QObject
{
    Q_OBJECT
private slots:
    void init();  // 每个用例前复位窗口指针。
    void editorPipeline();
    void cleanup();  // 用例结束后关闭窗口，避免 WebEngine 在退出时拆卸崩溃。

private:
    // 同步等待一次短 JavaScript 求值，带超时防止测试挂起。
    QVariant evaluate(QWebEnginePage* page, const QString& source, int timeoutMs = 8000);
    // 轮询等待条件成立。
    bool waitFor(QWebEnginePage* page, const QString& condition, int timeoutMs);
    static quint32 fnv1a(const QByteArray& bytes);
    static bool parseIhdr(const QByteArray& bytes, int* width, int* height,
                          int* bitDepth, int* colorType);
    QPointer<TerrainEditWindow> m_window;
};

void TerrainWebSmokeTest::init()
{
    m_window.clear();
}

void TerrainWebSmokeTest::cleanup()
{
    // 无论断言是否通过都关闭编辑窗口，让 WebEngine 在测试内完成拆卸。
    if (m_window) {
        m_window->close();
        QTest::qWait(300);
    }
}

QVariant TerrainWebSmokeTest::evaluate(QWebEnginePage* page, const QString& source, int timeoutMs)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QVariant result;
    auto state = std::make_shared<QVariant>();
    auto done = std::make_shared<bool>(false);
    QPointer<QEventLoop> guardedLoop(&loop);
    page->runJavaScript(source, [state, done, guardedLoop](const QVariant& value) {
        *state = value;
        *done = true;
        if (guardedLoop)
            guardedLoop->quit();
    });
    timeout.start(timeoutMs);
    if (!*done)
        loop.exec();
    return *done ? *state : QVariant();
}

bool TerrainWebSmokeTest::waitFor(QWebEnginePage* page, const QString& condition, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        const QVariant value = evaluate(page, condition, 3000);
        if (value.isValid() && value.toBool())
            return true;
        QTest::qWait(120);
    }
    return false;
}

quint32 TerrainWebSmokeTest::fnv1a(const QByteArray& bytes)
{
    quint32 hash = 2166136261u;
    for (char byte : bytes) {
        hash ^= static_cast<uchar>(byte);
        hash *= 16777619u;
    }
    return hash;
}

bool TerrainWebSmokeTest::parseIhdr(const QByteArray& bytes, int* width, int* height,
                                    int* bitDepth, int* colorType)
{
    static const uchar signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (bytes.size() < 33 || std::memcmp(bytes.constData(), signature, 8) != 0)
        return false;
    const uchar* ihdr = reinterpret_cast<const uchar*>(bytes.constData()) + 16;
    *width = (ihdr[0] << 24) | (ihdr[1] << 16) | (ihdr[2] << 8) | ihdr[3];
    *height = (ihdr[4] << 24) | (ihdr[5] << 16) | (ihdr[6] << 8) | ihdr[7];
    *bitDepth = ihdr[8];
    *colorType = ihdr[9];
    return true;
}

void TerrainWebSmokeTest::editorPipeline()
{
    QTemporaryDir data;
    QVERIFY(data.isValid());
    TerrainService service(data.path());
    // 129×129 网格（256 米、间距 2 米）平坦项目。
    const auto created = service.create(QStringLiteral("集成测试地形"), 256, 256, 2, 0);
    QVERIFY2(created.value("ok").toBool(), qPrintable(created.value("error").toString()));
    const QString projectId = created.value("data").toMap().value("id").toString();

    // 通过编辑窗口注册表打开，验证重复打开激活同一窗口。
    auto* window = TerrainEditWindow::open(&service, projectId, nullptr);
    QVERIFY(window);
    m_window = window;
    window->show();
    QVERIFY(TerrainEditWindow::isProjectOpen(projectId));
    QCOMPARE(TerrainEditWindow::open(&service, projectId, nullptr), window);
    auto* page = window->findChild<QWebEnginePage*>();
    QVERIFY(page);

    // 等待页面完成高度数据加载与场景初始化。
    QVERIFY2(waitFor(page, QStringLiteral("window.terrainReady === true"), 60000),
        qPrintable(evaluate(page, QStringLiteral("window.terrainBootError || 'timeout'")).toString()));

    // 网格与项目元数据一致。
    const QVariant stats = evaluate(page, QStringLiteral(
        "JSON.stringify(window.terrainEditor.getStats())"));
    const QJsonObject statsObject = QJsonDocument::fromJson(stats.toString().toUtf8()).object();
    QCOMPARE(statsObject.value("gridWidth").toInt(), 129);
    QCOMPARE(statsObject.value("gridDepth").toInt(), 129);
    QCOMPARE(statsObject.value("spacing").toDouble(), 2.0);

    const qint64 before = evaluate(page, QStringLiteral("window.terrainEditor.checksum()")).toLongLong();
    InterfaceLanguage::setLanguage(QStringLiteral("en"));
    QVERIFY(waitFor(page, QStringLiteral("document.getElementById('action-save').textContent === 'Save'"), 10000));
    QCOMPARE(evaluate(page, QStringLiteral("window.terrainEditor.checksum()")).toLongLong(), before);
    InterfaceLanguage::setLanguage(QStringLiteral("zh-CN"));
    QVERIFY(waitFor(page, QStringLiteral("document.getElementById('action-save').textContent === '保存'"), 10000));
    // 程序化细节笔划：从中心向外拖动 60 米。
    const QVariant stroke = evaluate(page, QStringLiteral(R"js(
        (function () {
            const points = [];
            for (let index = 0; index <= 15; ++index)
                points.push({ x: 128 + index * 4, z: 128 });
            return JSON.stringify(window.terrainEditor.applyStroke("detail", points, 33));
        })()
    )js"));
    Q_UNUSED(stroke)
    const qint64 after = evaluate(page, QStringLiteral("window.terrainEditor.checksum()")).toLongLong();
    QVERIFY(after != before);
    // 笔划终点附近的地形被抬高（世界坐标 180,128 位于路径旁）。
    QVERIFY(evaluate(page, QStringLiteral("window.terrainEditor.sampleHeight(180, 128) > 0")).toBool());
    QCOMPARE(evaluate(page, QStringLiteral("window.terrainEditor.getStats().dirty")).toBool(), true);
    QVERIFY(evaluate(page, QStringLiteral("window.terrainEditor.canUndo()")).toBool());

    // 撤销精确恢复，重做精确恢复。
    QVERIFY(evaluate(page, QStringLiteral("window.terrainEditor.undo(), true")).toBool());
    QCOMPARE(evaluate(page, QStringLiteral("window.terrainEditor.checksum()")).toLongLong(), before);
    QVERIFY(evaluate(page, QStringLiteral("window.terrainEditor.redo(), true")).toBool());
    QCOMPARE(evaluate(page, QStringLiteral("window.terrainEditor.checksum()")).toLongLong(), after);

    // 保存：服务端文件字节校验和与网页端一致（网页是权威数据源）。
    evaluate(page, QStringLiteral(
        "window.terrainEditor.save().then(() => { window._saveOk = true; })"
        ".catch((error) => { window._saveErr = String(error && error.message || error); }), true"),
        30000);
    QVERIFY2(waitFor(page, QStringLiteral(
        "window._saveOk === true || typeof window._saveErr === 'string'"), 30000), "首次保存未完成");
    QVERIFY2(evaluate(page, QStringLiteral("window._saveErr === undefined")).toBool(),
        qPrintable(evaluate(page, QStringLiteral("window._saveErr || 'unknown'")).toString()));
    QVERIFY(waitFor(page, QStringLiteral("!window.terrainEditor.getStats().dirty"), 30000));
    const auto open = service.openHeightStream(projectId);
    QVERIFY2(open.value("ok").toBool(), qPrintable(open.value("error").toString()));
    const auto stream = open.value("data").toMap();
    QByteArray saved;
    int index = 0;
    while (saved.size() < stream.value("bytes").toLongLong()) {
        const auto chunk = service.readHeightChunk(stream.value("token").toString(), index++);
        QVERIFY(chunk.value("ok").toBool());
        saved.append(QByteArray::fromBase64(
            chunk.value("data").toMap().value("data").toString().toLatin1()));
    }
    service.cancelStream(stream.value("token").toString());
    QCOMPARE(static_cast<quint32>(fnv1a(saved.mid(16))),
        static_cast<quint32>(after));

    // 导出：4 米间距 → 65×65 的 16 位灰度 PNG + JSON 说明。
    // 路径统一用正斜杠，避免 JavaScript 字符串里的反斜杠转义问题。
    const QString pngPath = QString(data.path() + QStringLiteral("/导出高度图.png"))
        .replace(QChar(92), QChar(47)); // 92=反斜杠 → 47=正斜杠
    evaluate(page, QString("window.terrainEditor.exportHeightmap('%1', 4), true").arg(pngPath), 60000);
    QVERIFY(waitFor(page, QStringLiteral(
        "window.document.getElementById('status-message').textContent.indexOf('已导出') >= 0"), 60000));
    QFile png(pngPath);
    QVERIFY(png.open(QIODevice::ReadOnly));
    const QByteArray pngBytes = png.readAll();
    int width = 0;
    int height = 0;
    int bitDepth = 0;
    int colorType = 0;
    QVERIFY(parseIhdr(pngBytes, &width, &height, &bitDepth, &colorType));
    QCOMPARE(width, 65);
    QCOMPARE(height, 65);
    QCOMPARE(bitDepth, 16);
    QCOMPARE(colorType, 0);
    QImage decoded;
    QVERIFY(decoded.loadFromData(pngBytes, "PNG"));
    QCOMPARE(decoded.format(), QImage::Format_Grayscale16);
    QVERIFY(QFileInfo::exists(data.path() + QStringLiteral("/导出高度图.json")));
    QFile json(data.path() + QStringLiteral("/导出高度图.json"));
    QVERIFY(json.open(QIODevice::ReadOnly));
    const QJsonObject meta = QJsonDocument::fromJson(json.readAll()).object();
    QCOMPARE(meta.value("pngWidth").toInt(), 65);
    QVERIFY(meta.value("exportSpacingMeters").toDouble() >= 4.0 - 1e-6);

    // 重采样到 4 米：网格变 65×65，撤销历史清空；保存后元数据同步。
    evaluate(page, QStringLiteral("window.terrainEditor.resampleTo(4), true"));
    QVERIFY(waitFor(page, QStringLiteral(
        "window.terrainEditor.getStats().gridWidth === 65"), 30000));
    QCOMPARE(evaluate(page, QStringLiteral("window.terrainEditor.getStats().canUndo")).toBool(), false);
    evaluate(page, QStringLiteral(
        "window._saveOk = undefined; window._saveErr = undefined;"
        "window.terrainEditor.save().then(() => { window._saveOk = true; })"
        ".catch((error) => { window._saveErr = String(error && error.message || error); }), true"),
        30000);
    QVERIFY2(waitFor(page, QStringLiteral(
        "window._saveOk === true || typeof window._saveErr === 'string'"), 30000), "重采样保存未完成");
    QVERIFY2(evaluate(page, QStringLiteral("window._saveErr === undefined")).toBool(),
        qPrintable(evaluate(page, QStringLiteral("window._saveErr || 'unknown'")).toString()));
    QVERIFY(waitFor(page, QStringLiteral("!window.terrainEditor.getStats().dirty"), 30000));
    const auto meta2 = service.readMeta(projectId).value("data").toMap();
    QCOMPARE(meta2.value("gridWidth").toInt(), 65);
    QCOMPARE(meta2.value("spacing").toDouble(), 4.0);

    // 未保存状态已解除，网页请求关闭后窗口自毁并从注册表移除。
    evaluate(page, QStringLiteral("window.terrainEditor.requestSaveAndClose(), true"));
    QTRY_VERIFY_WITH_TIMEOUT(!TerrainEditWindow::isProjectOpen(projectId), 15000);
}

QTEST_MAIN(TerrainWebSmokeTest)
#include "TerrainWebSmokeTest.moc"
