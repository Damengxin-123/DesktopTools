#include "services/QrDecoder.h"
#include "services/QrService.h"

#include <QFile>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QTransform>
#include <QtTest>

namespace {
// 提取服务的成功状态。
bool ok(const QVariantMap& result) { return result.value("ok").toBool(); }
// 提取服务返回的数据对象。
QVariantMap data(const QVariantMap& result) { return result.value("data").toMap(); }
// 获取当前历史摘要列表。
QVariantList items(const QrService& service) { return data(service.snapshot()).value("items").toList(); }
// 加载 tests/media 下的二维码测试图。
QImage fixture(const QString& name)
{
    return QImage(QStringLiteral(TEST_QR_DIR) + QLatin1Char('/') + name);
}
// 在临时目录写入文件，用于构造损坏的历史索引。
bool writeFile(const QString& path, const QByteArray& content)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}
// 读取持久化文件，以检查失败操作是否更改原始内容。
QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
}

// 覆盖解码器（版本、纠错级别、数据段模式、旋转缩放反色）与历史服务的增删持久化。
class QrServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    // 全部测试二维码按生成时的预期内容解码。
    void decodesFixtures();
    // 在线旋转、缩放与扩大白边后的图片仍可识别。
    void decodesTransformedImages();
    // 非二维码与空白图片返回明确失败。
    void rejectsNonQrImages();
    // 识别成功写入历史；快照不含原图，读取返回完整记录。
    void recordsHistory();
    // 相同内容的重复识别只保留最新一条。
    void deduplicatesByText();
    // 删除指定记录并支持清空。
    void removesAndClears();
    // 同一数据目录重新加载后记录保持一致。
    void reloadsPersistedData();
    // 历史文件损坏时进入只读保护，拒绝覆盖原文件。
    void protectsCorruptedIndex();
};

void QrServiceTest::decodesFixtures()
{
    const struct Case {
        const char* file;
        const char* expected;
    } cases[] = {
        {"qr-v1-l-url.png", "https://qt.io"},
        {"qr-v1-h-numeric.png", "8613800138000"},
        {"qr-v2-m-url.png", "https://example.com/hello"},
        {"qr-v3-q-alnum.png", "HTTPS://EXAMPLE.COM/QT"},
        {"qr-v4-m-eciu8.png", "二维码识别测试✓"},
        {"qr-v5-l-segments.png", "2026年桌面工具"},
        {"qr-v7-l-version.png", "version-info-check-7"},
        {"qr-rot90.png", "https://example.com/hello"},
        {"qr-rot180.png", "https://qt.io"},
        {"qr-rot30.png", "https://example.com/hello"},
        {"qr-scaled50.png", "https://example.com/hello"},
        {"qr-inverted.png", "https://qt.io"},
    };
    for (const Case& entry : cases) {
        const QImage image = fixture(QLatin1String(entry.file));
        QVERIFY2(!image.isNull(), qPrintable(QStringLiteral("缺少测试图：") + entry.file));
        const auto result = QrDecoder::decode(image);
        QVERIFY2(!result.text.isEmpty(), qPrintable(QStringLiteral("解码失败：") + entry.file
            + QStringLiteral("：") + result.error));
        QVERIFY2(result.text == QString::fromUtf8(entry.expected),
            qPrintable(QStringLiteral("解码内容不符：") + entry.file + QStringLiteral("：") + result.text));
        QVERIFY(result.version >= 1 && result.version <= 40);
    }
    // 长链接单独构造，避免源文件里出现 84 个重复字符。
    const QString longUrl = QStringLiteral("https://example.com/path?query=") + QString(84, QLatin1Char('a'));
    const auto longResult = QrDecoder::decode(fixture(QStringLiteral("qr-v6-l-long.png")));
    QVERIFY(!longResult.text.isEmpty());
    QCOMPARE(longResult.text, longUrl);
    QCOMPARE(longResult.version, 6);
}

void QrServiceTest::decodesTransformedImages()
{
    const QImage source = fixture(QStringLiteral("qr-v2-m-url.png"));
    QVERIFY(!source.isNull());
    const QString expected = QStringLiteral("https://example.com/hello");

    const QImage rotated = source.transformed(QTransform().rotate(90));
    QVERIFY(!rotated.isNull());
    QCOMPARE(QrDecoder::decode(rotated).text, expected);

    // 小角度旋转产生抗锯齿边缘，验证几何重建的鲁棒性。
    const QImage skewed = source.transformed(QTransform().rotate(15), Qt::SmoothTransformation);
    QVERIFY(!skewed.isNull());
    QCOMPARE(QrDecoder::decode(skewed).text, expected);

    // 放大一倍后模块尺寸变化，仍需正确重建网格。
    const QImage enlarged = source.scaled(source.width() * 2, source.height() * 2,
        Qt::KeepAspectRatio, Qt::FastTransformation);
    QCOMPARE(QrDecoder::decode(enlarged).text, expected);

    // 四周扩大白边，验证缺少边界接触时的采样。
    QImage padded(source.width() + 120, source.height() + 120, QImage::Format_Grayscale8);
    padded.fill(255);
    QPainter painter(&padded);
    painter.drawImage(60, 60, source);
    painter.end();
    QCOMPARE(QrDecoder::decode(padded).text, expected);
}

void QrServiceTest::rejectsNonQrImages()
{
    const auto negative = QrDecoder::decode(fixture(QStringLiteral("not-qr.png")));
    QVERIFY(negative.text.isEmpty());
    QVERIFY(!negative.error.isEmpty());
    QCOMPARE(negative.version, 0);

    QImage blank(120, 120, QImage::Format_Grayscale8);
    blank.fill(255);
    const auto empty = QrDecoder::decode(blank);
    QVERIFY(empty.text.isEmpty());
    QVERIFY(!empty.error.isEmpty());

    QImage noise(120, 120, QImage::Format_Grayscale8);
    noise.fill(0);
    QVERIFY(QrDecoder::decode(noise).text.isEmpty());
}

void QrServiceTest::recordsHistory()
{
    QTemporaryDir root;
    QrService service(root.path());
    int changes = 0;
    connect(&service, &QrService::changed, [&changes]() { ++changes; });

    const auto decoded = service.decode(fixture(QStringLiteral("qr-v1-l-url.png")), QStringLiteral("本地文件"));
    QVERIFY(ok(decoded));
    const QVariantMap record = data(decoded);
    const QString id = record.value("id").toString();
    QVERIFY(!id.isEmpty());
    QCOMPARE(record.value("text").toString(), QStringLiteral("https://qt.io"));
    QVERIFY(record.value("image").toString().startsWith(QStringLiteral("data:image/png;base64,")));
    QVERIFY(record.value("thumbnail").toString().startsWith(QStringLiteral("data:image/png;base64,")));
    QCOMPARE(record.value("source").toString(), QStringLiteral("本地文件"));
    QCOMPARE(record.value("version").toInt(), 1);
    QCOMPARE(changes, 1);

    // 快照只提供预览和缩略图，不携带原图数据。
    const QVariantList summaries = items(service);
    QCOMPARE(summaries.size(), 1);
    const QVariantMap summary = summaries.first().toMap();
    QVERIFY(!summary.contains(QStringLiteral("image")));
    QVERIFY(!summary.contains(QStringLiteral("text")));
    QCOMPARE(summary.value("preview").toString(), QStringLiteral("https://qt.io"));
    QVERIFY(summary.value("thumbnail").toString().startsWith(QStringLiteral("data:image/png;base64,")));
    QVERIFY(QDateTime::fromString(summary.value("decodedAt").toString(), Qt::ISODateWithMs).isValid());

    // 完整读取包含原图；不存在的标识返回失败。
    const QVariantMap full = data(service.read(id));
    QCOMPARE(full.value("text").toString(), QStringLiteral("https://qt.io"));
    QVERIFY(!full.value("image").toString().isEmpty());
    QVERIFY(!ok(service.read(QStringLiteral("missing"))));

    // 识别失败不产生历史记录，也不发出变更信号。
    QImage blank(120, 120, QImage::Format_Grayscale8);
    blank.fill(255);
    const QVariantMap failed = service.decode(blank, QStringLiteral("剪贴板"));
    QVERIFY(!ok(failed));
    QVERIFY(!failed.value("error").toString().isEmpty());
    QCOMPARE(changes, 1);
    QCOMPARE(items(service).size(), 1);
}

void QrServiceTest::deduplicatesByText()
{
    QTemporaryDir root;
    QrService service(root.path());
    QVERIFY(ok(service.decode(fixture(QStringLiteral("qr-v1-l-url.png")), QStringLiteral("本地文件"))));
    // 相同内容的识别上移为最新记录，不新增条目。
    QVERIFY(ok(service.decode(fixture(QStringLiteral("qr-v1-l-url.png")), QStringLiteral("剪贴板"))));
    QCOMPARE(items(service).size(), 1);
    QCOMPARE(items(service).first().toMap().value("source").toString(), QStringLiteral("剪贴板"));

    QVERIFY(ok(service.decode(fixture(QStringLiteral("qr-v1-h-numeric.png")), QStringLiteral("本地文件"))));
    QCOMPARE(items(service).size(), 2);
    QCOMPARE(items(service).first().toMap().value("preview").toString(), QStringLiteral("8613800138000"));
    QCOMPARE(items(service).at(1).toMap().value("preview").toString(), QStringLiteral("https://qt.io"));
}

void QrServiceTest::removesAndClears()
{
    QTemporaryDir root;
    QrService service(root.path());
    QVERIFY(ok(service.decode(fixture(QStringLiteral("qr-v1-l-url.png")), QStringLiteral("本地文件"))));
    QVERIFY(ok(service.decode(fixture(QStringLiteral("qr-v1-h-numeric.png")), QStringLiteral("本地文件"))));
    const QString firstId = items(service).first().toMap().value("id").toString();

    // 批量删除时忽略不存在的标识。
    QVERIFY(ok(service.remove({firstId, QStringLiteral("missing")})));
    QCOMPARE(items(service).size(), 1);
    QVERIFY(ok(service.clear()));
    QCOMPARE(items(service).size(), 0);
}

void QrServiceTest::reloadsPersistedData()
{
    QTemporaryDir root;
    QVariantMap before;
    {
        QrService service(root.path());
        QVERIFY(ok(service.decode(fixture(QStringLiteral("qr-v1-l-url.png")), QStringLiteral("本地文件"))));
        QVERIFY(ok(service.decode(fixture(QStringLiteral("qr-v1-h-numeric.png")), QStringLiteral("剪贴板"))));
        before = service.snapshot();
        QVERIFY(ok(before));
    }
    QrService reopened(root.path());
    QCOMPARE(reopened.snapshot(), before);
}

void QrServiceTest::protectsCorruptedIndex()
{
    QTemporaryDir root;
    const QString path = root.filePath(QStringLiteral("qr-history.v1.json"));
    QVERIFY(writeFile(path, QByteArray("{broken")));
    QrService service(root.path());
    QVERIFY(!ok(service.snapshot()));
    QVERIFY(!ok(service.decode(fixture(QStringLiteral("qr-v1-l-url.png")), QStringLiteral("本地文件"))));
    QVERIFY(!ok(service.clear()));
    QCOMPARE(readFile(path), QByteArray("{broken"));

    // 有效 JSON 但记录字段非法时同样保护，不覆盖原文件。
    QVERIFY(writeFile(path, QByteArray("{\"version\":2,\"items\":[]}")));
    QrService wrongVersion(root.path());
    QVERIFY(!ok(wrongVersion.snapshot()));
    QVERIFY(!ok(wrongVersion.clear()));
    QCOMPARE(readFile(path), QByteArray("{\"version\":2,\"items\":[]}"));
}

QTEST_MAIN(QrServiceTest)
#include "QrServiceTest.moc"
