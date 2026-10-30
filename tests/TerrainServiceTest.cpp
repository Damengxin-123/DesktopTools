#include "services/TerrainService.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <cstring>
#include <limits>

// 地形服务测试：项目 CRUD、heightmap.bin 往返、分块流、损坏保护与 16 位 PNG 导出。
class TerrainServiceTest final : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    // 新建项目：尺寸整除与不整除的调整规则、参数校验。
    void createAndAdjustSize();
    // 高度数据分块读取往返：文件头、网格与浮点值一致。
    void heightStreamRoundtrip();
    // 保存流：顺序写入、错误顺序拒绝、提交后数据一致、索引同步。
    void saveRoundtrip();
    // 重采样后的网格参数可以保存，尺寸偏差受一个采样间距限制。
    void resampledGridSave();
    // 数据不完整、网格不符或包含 NaN 时提交被拒绝且原数据不变。
    void rejectedWrites();
    // 重命名与删除；索引损坏时读取与写入都被保护。
    void renameRemoveAndCorruptIndex();
    // 预览降采样与晕染缩略图。
    void previewAndThumbnail();
    // 16 位灰度 PNG 导出：IHDR 独立校验、QImage 回读比对、中文路径、JSON 说明。
    void exportHeightmap();
private:
    // 按序读取完整高度字节。
    QByteArray readAll(TerrainService& service, const QString& id);
    // 独立解析 PNG 头（不依赖 QImage）。
    static bool parseIhdr(const QByteArray& bytes, int* width, int* height, int* bitDepth, int* colorType);
    // FNV-1a 校验（与网页端一致）。
    static quint32 fnv1a(const QByteArray& bytes);

    QTemporaryDir m_dir;
    QString m_flatId;   // 100×60 米、间距 2、初始高度 5。
    QString m_oddId;    // 101×50 米、间距 2（尺寸被调整）。
};

void TerrainServiceTest::initTestCase()
{
    QVERIFY(m_dir.isValid());
    TerrainService service(m_dir.path());
    const auto flat = service.create(QStringLiteral("测试地形"), 100, 60, 2, 5);
    QVERIFY2(flat.value("ok").toBool(), qPrintable(flat.value("error").toString()));
    m_flatId = flat.value("data").toMap().value("id").toString();
    const auto odd = service.create(QStringLiteral("斜坡地图"), 101, 50, 2, 0);
    QVERIFY2(odd.value("ok").toBool(), qPrintable(odd.value("error").toString()));
    m_oddId = odd.value("data").toMap().value("id").toString();
    QVERIFY(!m_flatId.isEmpty() && !m_oddId.isEmpty());
}

void TerrainServiceTest::createAndAdjustSize()
{
    TerrainService service(m_dir.path());
    const auto snapshot = service.snapshot();
    QVERIFY(snapshot.value("ok").toBool());
    QCOMPARE(snapshot.value("data").toMap().value("items").toList().size(), 2);

    const auto meta = service.readMeta(m_flatId).value("data").toMap();
    QCOMPARE(meta.value("gridWidth").toInt(), 51);
    QCOMPARE(meta.value("gridDepth").toInt(), 31);
    QCOMPARE(meta.value("sizeWidth").toDouble(), 100.0);
    QCOMPARE(meta.value("sizeDepth").toDouble(), 60.0);

    const auto odd = service.readMeta(m_oddId).value("data").toMap();
    QCOMPARE(odd.value("gridWidth").toInt(), 51);
    QCOMPARE(odd.value("sizeWidth").toDouble(), 100.0); // 101 米向下调整为 100 米。

    // 非法参数被拒绝。
    QVERIFY(!service.create(QStringLiteral("坏"), 100, 60, 0.05, 0).value("ok").toBool());
    QVERIFY(!service.create(QStringLiteral("坏"), 100, 60, 2, 5000).value("ok").toBool());
    QVERIFY(!service.create(QStringLiteral("坏"), 100000, 60, 2, 0).value("ok").toBool());
    QVERIFY(!service.create(QStringLiteral(" "), 100, 60, 2, 0).value("ok").toBool());
    // 超大网格给出明确错误（尺寸合法但网格点数超限）。
    const auto huge = service.create(QStringLiteral("太大"), 16000, 16000, 1, 0);
    QVERIFY(!huge.value("ok").toBool());
    QVERIFY(huge.value("error").toString().contains(QStringLiteral("采样")));
}

QByteArray TerrainServiceTest::readAll(TerrainService& service, const QString& id)
{
    // QVERIFY 在非 void 函数中不能提前 return，这里用 qFail 显式报告后返回空数据。
    const auto open = service.openHeightStream(id);
    if (!open.value("ok").toBool()) {
        QTest::qFail(qPrintable(open.value("error").toString()), __FILE__, __LINE__);
        return {};
    }
    const auto info = open.value("data").toMap();
    QByteArray bytes;
    const qint64 total = info.value("bytes").toLongLong();
    const int chunkSize = info.value("chunkSize").toInt();
    int index = 0;
    for (qint64 offset = 0; offset < total; offset += chunkSize) {
        const auto chunk = service.readHeightChunk(info.value("token").toString(), index++);
        if (!chunk.value("ok").toBool()) {
            QTest::qFail("读取高度分块失败", __FILE__, __LINE__);
            return {};
        }
        bytes.append(QByteArray::fromBase64(
            chunk.value("data").toMap().value("data").toString().toLatin1()));
    }
    if (!service.cancelStream(info.value("token").toString()).value("ok").toBool()) {
        QTest::qFail("取消传输流失败", __FILE__, __LINE__);
        return {};
    }
    return bytes;
}

bool TerrainServiceTest::parseIhdr(const QByteArray& bytes, int* width, int* height,
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

quint32 TerrainServiceTest::fnv1a(const QByteArray& bytes)
{
    quint32 hash = 2166136261u;
    for (char byte : bytes) {
        hash ^= static_cast<uchar>(byte);
        hash *= 16777619u;
    }
    return hash;
}

void TerrainServiceTest::heightStreamRoundtrip()
{
    TerrainService service(m_dir.path());
    const QByteArray bytes = readAll(service, m_flatId);
    QCOMPARE(bytes.size(), 16 + 4 * 51 * 31);
    QCOMPARE(bytes.left(4), QByteArray("DHM1"));
    const auto* header = reinterpret_cast<const uchar*>(bytes.constData());
    QCOMPARE(qFromLittleEndian<qint32>(header + 4), 1);
    QCOMPARE(qFromLittleEndian<qint32>(header + 8), 51);
    QCOMPARE(qFromLittleEndian<qint32>(header + 12), 31);
    // 全部为初始高度 5。
    for (int index = 0; index < 51 * 31; ++index) {
        float value;
        std::memcpy(&value, bytes.constData() + 16 + index * 4, 4);
        QCOMPARE(value, 5.0f);
    }
    // 分块顺序错误被拒绝。
    const auto open = service.openHeightStream(m_flatId);
    const auto token = open.value("data").toMap().value("token").toString();
    QVERIFY(!service.readHeightChunk(token, 99999).value("ok").toBool());
    service.cancelStream(token);
}

void TerrainServiceTest::saveRoundtrip()
{
    TerrainService service(m_dir.path());
    // 构造新的高度数据：正弦坡。
    const int width = 51;
    const int depth = 31;
    QVector<float> heights(width * depth);
    for (int z = 0; z < depth; ++z) {
        for (int x = 0; x < width; ++x) {
            heights[z * width + x] = static_cast<float>(x * 0.5 + z * 0.25);
        }
    }
    auto toRaw = [](float value) {
        quint32 raw;
        std::memcpy(&raw, &value, 4);
        return raw;
    };
    // 网页侧协议只传输高度数据本身（不含文件头）。
    QByteArray payload(heights.size() * 4, Qt::Uninitialized);
    for (int index = 0; index < heights.size(); ++index) {
        qToLittleEndian<quint32>(toRaw(heights.at(index)),
            reinterpret_cast<uchar*>(payload.data()) + index * 4);
    }
    const auto begin = service.beginHeightWrite(m_flatId, width, depth, 2.0, 30);
    QVERIFY2(begin.value("ok").toBool(), qPrintable(begin.value("error").toString()));
    const auto info = begin.value("data").toMap();
    QCOMPARE(info.value("bytes").toLongLong(), qint64(payload.size()));
    const QString token = info.value("token").toString();
    const int chunkSize = info.value("chunkSize").toInt();
    // 顺序错误的写入被拒绝。
    QVERIFY(!service.writeHeightChunk(token, 1,
        QString::fromLatin1(payload.mid(0, 100).toBase64())).value("ok").toBool());
    int index = 0;
    for (int offset = 0; offset < payload.size(); offset += chunkSize) {
        const auto result = service.writeHeightChunk(token, index++,
            QString::fromLatin1(payload.mid(offset, chunkSize).toBase64()));
        QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString()));
    }
    const auto commit = service.commitHeightWrite(token);
    QVERIFY2(commit.value("ok").toBool(), qPrintable(commit.value("error").toString()));

    // 重新读取：服务端生成的文件头与数据字节级一致。
    QByteArray full(16 + payload.size(), Qt::Uninitialized);
    std::memcpy(full.data(), "DHM1", 4);
    auto putInt32 = [&full](int offset, qint32 value) {
        qToLittleEndian<qint32>(value, reinterpret_cast<uchar*>(full.data()) + offset);
    };
    putInt32(4, 1);
    putInt32(8, width);
    putInt32(12, depth);
    std::memcpy(full.data() + 16, payload.constData(), payload.size());
    const QByteArray reloaded = readAll(service, m_flatId);
    QCOMPARE(reloaded, full);
    QCOMPARE(fnv1a(reloaded.mid(16)), fnv1a(payload));
}

void TerrainServiceTest::resampledGridSave()
{
    TerrainService service(m_dir.path());
    // 重采样为 4 米间距：网格 26×16，实际尺寸 100×60 米不变。
    const int width = 26;
    const int depth = 16;
    auto toRaw2 = [](float value) {
        quint32 raw;
        std::memcpy(&raw, &value, 4);
        return raw;
    };
    QByteArray payload(width * depth * 4, Qt::Uninitialized);
    for (int index = 0; index < width * depth; ++index) {
        qToLittleEndian<quint32>(toRaw2(static_cast<float>(index % 7)),
            reinterpret_cast<uchar*>(payload.data()) + index * 4);
    }
    const auto begin = service.beginHeightWrite(m_flatId, width, depth, 4.0, 12);
    QVERIFY2(begin.value("ok").toBool(), qPrintable(begin.value("error").toString()));
    const auto info = begin.value("data").toMap();
    QVERIFY(service.writeHeightChunk(info.value("token").toString(), 0,
        QString::fromLatin1(payload.toBase64())).value("ok").toBool());
    const auto commit = service.commitHeightWrite(info.value("token").toString());
    QVERIFY2(commit.value("ok").toBool(), qPrintable(commit.value("error").toString()));
    const auto meta = service.readMeta(m_flatId).value("data").toMap();
    QCOMPARE(meta.value("gridWidth").toInt(), 26);
    QCOMPARE(meta.value("spacing").toDouble(), 4.0);
    QCOMPARE(meta.value("undoSteps").toDouble(), 12.0);
}

void TerrainServiceTest::rejectedWrites()
{
    TerrainService service(m_dir.path());
    const QString id = service.snapshot().value("data").toMap()
        .value("items").toList().first().toMap().value("id").toString();
    const QByteArray original = readAll(service, id);

    // 数据不完整：未写入任何分块直接提交。
    const auto begin1 = service.beginHeightWrite(id, 26, 16, 4.0, 30);
    const auto info1 = begin1.value("data").toMap();
    QVERIFY(!service.commitHeightWrite(info1.value("token").toString()).value("ok").toBool());
    // 分块数据超过声明大小被拒绝。
    const auto begin2 = service.beginHeightWrite(id, 26, 16, 4.0, 30);
    const auto info2 = begin2.value("data").toMap();
    const QByteArray oversize = original.mid(16) + QByteArray(4, 0); // 超过声明大小。
    QVERIFY(!service.writeHeightChunk(info2.value("token").toString(), 0,
        QString::fromLatin1(oversize.toBase64())).value("ok").toBool());
    // NaN 数据被拒绝。
    QByteArray nanPayload(26 * 16 * 4, Qt::Uninitialized);
    for (int index = 0; index < 26 * 16; ++index) {
        const float value = index == 5 ? std::numeric_limits<float>::quiet_NaN() : 1.0f;
        quint32 raw;
        std::memcpy(&raw, &value, 4);
        qToLittleEndian<quint32>(raw,
            reinterpret_cast<uchar*>(nanPayload.data()) + index * 4);
    }
    const auto begin3 = service.beginHeightWrite(id, 26, 16, 4.0, 30);
    const auto info3 = begin3.value("data").toMap();
    QVERIFY(service.writeHeightChunk(info3.value("token").toString(), 0,
        QString::fromLatin1(nanPayload.toBase64())).value("ok").toBool());
    QVERIFY(!service.commitHeightWrite(info3.value("token").toString()).value("ok").toBool());

    // 原数据保持不变。
    QCOMPARE(readAll(service, id), original);
}

void TerrainServiceTest::renameRemoveAndCorruptIndex()
{
    TerrainService service(m_dir.path());
    const auto created = service.create(QStringLiteral("待删除"), 20, 20, 2, 0);
    const QString id = created.value("data").toMap().value("id").toString();
    QVERIFY(service.rename(id, QStringLiteral("改名后的地形")).value("ok").toBool());
    QCOMPARE(service.readMeta(id).value("data").toMap().value("title").toString(),
        QStringLiteral("改名后的地形"));
    QVERIFY(service.remove({id}).value("ok").toBool());
    QVERIFY(!service.readMeta(id).value("ok").toBool());
    // 数据目录一并删除。
    QVERIFY(!QDir(m_dir.path() + QStringLiteral("/terrain/projects/") + id).exists());

    // 索引损坏：在独立数据目录中验证服务拒绝读取与写入，保护原文件。
    QTemporaryDir corruptDir;
    QVERIFY(corruptDir.isValid());
    TerrainService victim(corruptDir.path());
    const auto victimProject = victim.create(QStringLiteral("损坏测试"), 20, 20, 2, 0);
    QVERIFY(victimProject.value("ok").toBool());
    const QString indexPath = corruptDir.path() + QStringLiteral("/terrain/index.v1.json");
    QSaveFile broken(indexPath);
    QVERIFY(broken.open(QIODevice::WriteOnly));
    broken.write("{not json");
    QVERIFY(broken.commit());
    TerrainService damaged(corruptDir.path());
    QVERIFY(!damaged.snapshot().value("ok").toBool());
    QVERIFY(!damaged.rename(
        victimProject.value("data").toMap().value("id").toString(),
        QStringLiteral("非法改名")).value("ok").toBool());
    QCOMPARE(QFileInfo(indexPath).size(), qint64(strlen("{not json")));
}

void TerrainServiceTest::previewAndThumbnail()
{
    TerrainService service(m_dir.path());
    // 独立平坦项目：共享项目可能已被其他用例改写为正弦/重采样数据。
    const auto flat = service.create(QStringLiteral("预览专用"), 100, 60, 2, 5);
    QVERIFY2(flat.value("ok").toBool(), qPrintable(flat.value("error").toString()));
    const QString previewId = flat.value("data").toMap().value("id").toString();
    const auto preview = service.readPreview(previewId, 33);
    QVERIFY2(preview.value("ok").toBool(), qPrintable(preview.value("error").toString()));
    const auto data = preview.value("data").toMap();
    const int width = data.value("gridWidth").toInt();
    const int depth = data.value("gridDepth").toInt();
    QVERIFY(width <= 33 && depth <= 33 && width >= 2 && depth >= 2);
    const QByteArray heights = data.value("heights").toByteArray();
    QCOMPARE(heights.size(), width * depth * 4);
    // 预览是原高度的盒式平均：平坦 5 米场仍为 5。
    const float value = *reinterpret_cast<const float*>(heights.constData());
    QCOMPARE(value, 5.0f);
    // 缩略图是有效的 PNG 缓存。
    const auto thumbnail = service.readThumbnail(previewId);
    const QByteArray png = thumbnail.value("data").toMap().value("image").toByteArray();
    QVERIFY(png.size() > 8);
    QImage image;
    QVERIFY(image.loadFromData(png, "PNG"));
    QVERIFY(image.width() >= 2 && image.height() >= 2);
}

void TerrainServiceTest::exportHeightmap()
{
    TerrainService service(m_dir.path());
    const QString id = service.snapshot().value("data").toMap()
        .value("items").toList().first().toMap().value("id").toString();

    // 导出间距小于编辑间距被拒绝。
    QVERIFY(!service.beginExport(id, m_dir.path() + QStringLiteral("/x.png"),
        51, 31, 1.0, 0, 10).value("ok").toBool());

    // 构造 17×17 的 16 位渐变（编辑数据 34×16 米、间距 2，导出间距 2 → 17×17）。
    const auto small = service.create(QStringLiteral("导出用"), 32, 32, 2, 0);
    const QString exportId = small.value("data").toMap().value("id").toString();
    const int pngSide = 17;
    QByteArray rows(pngSide * pngSide * 2, Qt::Uninitialized);
    for (int index = 0; index < pngSide * pngSide; ++index) {
        const quint16 value = static_cast<quint16>((index * 3931) % 65536);
        qToLittleEndian<quint16>(value, reinterpret_cast<uchar*>(rows.data()) + index * 2);
    }
    // 中文路径导出。
    const QString exportDir = m_dir.path() + QStringLiteral("/中文导出目录");
    QVERIFY(QDir().mkpath(exportDir));
    const QString pngPath = exportDir + QStringLiteral("/地形高度图.png");

    const auto begin = service.beginExport(exportId, pngPath, pngSide, pngSide, 2.0, 0.0, 65535.0);
    QVERIFY2(begin.value("ok").toBool(), qPrintable(begin.value("error").toString()));
    const auto info = begin.value("data").toMap();
    const QString token = info.value("token").toString();
    const int chunkSize = info.value("chunkSize").toInt();
    int index = 0;
    for (int offset = 0; offset < rows.size(); offset += chunkSize) {
        const auto result = service.writeExportChunk(token, index++,
            QString::fromLatin1(rows.mid(offset, chunkSize).toBase64()));
        QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString()));
    }
    const auto commit = service.commitExport(token);
    QVERIFY2(commit.value("ok").toBool(), qPrintable(commit.value("error").toString()));
    const auto result = commit.value("data").toMap();
    QCOMPARE(result.value("bitDepth").toInt(), 16);
    QCOMPARE(result.value("colorType").toInt(), 0);

    // 独立校验 1：直接解析 PNG 文件头。
    QFile file(pngPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray pngBytes = file.readAll();
    int width = 0;
    int height = 0;
    int bitDepth = 0;
    int colorType = 0;
    QVERIFY(parseIhdr(pngBytes, &width, &height, &bitDepth, &colorType));
    QCOMPARE(width, pngSide);
    QCOMPARE(height, pngSide);
    QCOMPARE(bitDepth, 16);   // 真正的 16 位。
    QCOMPARE(colorType, 0);   // 单通道灰度。

    // 独立校验 2：QImage 解码后逐像素比对。
    QImage image;
    QVERIFY(image.loadFromData(pngBytes, "PNG"));
    QCOMPARE(image.format(), QImage::Format_Grayscale16);
    for (int row = 0; row < pngSide; ++row) {
        const uchar* line = image.constScanLine(row);
        for (int column = 0; column < pngSide; ++column) {
            quint16 expected;
            std::memcpy(&expected, rows.constData() + (row * pngSide + column) * 2, 2);
            quint16 actual;
            std::memcpy(&actual, line + column * 2, 2);
            QCOMPARE(actual, expected);
        }
    }

    // JSON 说明文件与关键内容。
    QFile jsonFile(exportDir + QStringLiteral("/地形高度图.json"));
    QVERIFY(jsonFile.open(QIODevice::ReadOnly));
    const QJsonObject json = QJsonDocument::fromJson(jsonFile.readAll()).object();
    QCOMPARE(json.value("formatVersion").toInt(), 1);
    QCOMPARE(json.value("pngWidth").toInt(), pngSide);
    QCOMPARE(json.value("heightUnit").toString(), QStringLiteral("m"));
    QVERIFY(json.value("orientation").isObject());
    QVERIFY(json.value("unreal").toObject().contains(QStringLiteral("suggestedResolution")));
    QVERIFY(json.value("verification").toObject().value("bitDepth").toInt() == 16);

    // 导出数据不完整被拒绝。
    const auto begin2 = service.beginExport(exportId, exportDir + QStringLiteral("/截断.png"),
        pngSide, pngSide, 2.0, 0, 65535);
    const auto info2 = begin2.value("data").toMap();
    QVERIFY(!service.commitExport(info2.value("token").toString()).value("ok").toBool());
    QVERIFY(!QFileInfo::exists(exportDir + QStringLiteral("/截断.png")));
}

QTEST_MAIN(TerrainServiceTest)
#include "TerrainServiceTest.moc"
