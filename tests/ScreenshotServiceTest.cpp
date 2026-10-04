#include "app/ScreenshotOverlay.h"
#include "services/ScreenshotService.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>

namespace {
// 提取服务的成功状态。
bool ok(const QVariantMap& result) { return result.value("ok").toBool(); }
// 提取服务返回的数据对象。
QVariantMap data(const QVariantMap& result) { return result.value("data").toMap(); }
// 获取当前历史摘要列表。
QVariantList items(const ScreenshotService& service) { return data(service.snapshot()).value("items").toList(); }
// 当前时间戳，格式与服务内部一致。
QString nowIso()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}
// 生成一张带内容的测试截图。
QImage sampleImage(int width, int height, const QColor& color)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(color);
    QPainter painter(&image);
    painter.fillRect(width / 4, height / 4, width / 2, height / 2, Qt::white);
    painter.end();
    return image;
}
// 在临时目录写入文件，用于构造损坏的索引。
bool writeFile(const QString& path, const QByteArray& content)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}
QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
// 颜色按通道近似比较，容忍抗锯齿边缘。
bool colorNear(const QColor& actual, const QColor& expected, int tolerance = 40)
{
    return qAbs(actual.red() - expected.red()) <= tolerance
        && qAbs(actual.green() - expected.green()) <= tolerance
        && qAbs(actual.blue() - expected.blue()) <= tolerance;
}

// 直接向遮罩投递鼠标事件；offscreen 平台的 QTest::mouseMove 依赖真实游标，不可靠。
void sendMouse(ScreenshotOverlay& overlay, QEvent::Type type, const QPoint& position, Qt::MouseButton button)
{
    QMouseEvent event(type, QPointF(position), QPointF(overlay.mapToGlobal(position)),
        button, button, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &event);
}
}

// 覆盖截图历史的增删持久化与保护，以及遮罩的选区、标注、撤销与合成输出。
class ScreenshotServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    // 保存写入 PNG 与缩略图，快照与完整读取字段齐全。
    void addsAndReads();
    // 单条删除删除文件，清空清空全部文件。
    void removesAndClears();
    // 超过 100 条自动移除最早的记录及文件。
    void prunesOldest();
    // 空图像、无效时间被拒绝。
    void rejectsInvalidInput();
    // 同一数据目录重新加载后记录保持一致。
    void reloadsPersistedData();
    // 索引损坏时进入只读保护，拒绝覆盖原文件。
    void protectsCorruptedIndex();

    // 遮罩：拖拽进入标注阶段，Enter 合成带标注的图像。
    void overlaySelectionAndAnnotation();
    // 遮罩：撤销移除最后一步标注，Esc 取消整次截图。
    void overlayUndoAndCancel();
    // 遮罩摆在非原点几何（模拟副屏）时映射只依赖局部坐标。
    void overlaySecondaryScreenGeometry();
};

void ScreenshotServiceTest::addsAndReads()
{
    QTemporaryDir root;
    ScreenshotService service(root.path());
    int changes = 0;
    connect(&service, &ScreenshotService::changed, [&changes]() { ++changes; });

    const QImage image = sampleImage(320, 200, QColor("#3d78c8"));
    const auto added = service.add(image, nowIso());
    QVERIFY(ok(added));
    const QVariantMap record = data(added);
    const QString id = record.value("id").toString();
    QVERIFY(!id.isEmpty());
    QCOMPARE(record.value("width").toInt(), 320);
    QCOMPARE(record.value("height").toInt(), 200);
    QVERIFY(record.value("bytes").toLongLong() > 0);
    QVERIFY(record.value("thumbnail").toString().startsWith(QStringLiteral("data:image/png;base64,")));
    QCOMPARE(changes, 1);

    // 快照不含原图与路径。
    const QVariantList summaries = items(service);
    QCOMPARE(summaries.size(), 1);
    const QVariantMap summary = summaries.first().toMap();
    QVERIFY(!summary.contains(QStringLiteral("image")));
    QVERIFY(!summary.contains(QStringLiteral("path")));
    QVERIFY(QDateTime::fromString(summary.value("capturedAt").toString(), Qt::ISODateWithMs).isValid());

    // 完整读取包含数据地址与磁盘路径，文件确实存在。
    const QVariantMap full = data(service.read(id));
    QVERIFY(full.value("image").toString().startsWith(QStringLiteral("data:image/png;base64,")));
    const QString path = full.value("path").toString();
    QVERIFY(QFileInfo::exists(path));
    QVERIFY(path.endsWith(QStringLiteral(".png")));
    QVERIFY(!ok(service.read(QStringLiteral("missing"))));
}

void ScreenshotServiceTest::removesAndClears()
{
    QTemporaryDir root;
    ScreenshotService service(root.path());
    QVERIFY(ok(service.add(sampleImage(200, 120, QColor("#3d78c8")), nowIso())));
    QVERIFY(ok(service.add(sampleImage(240, 160, QColor("#e02f2f")), nowIso())));
    const QString firstId = items(service).first().toMap().value("id").toString();
    const QString firstPath = data(service.read(firstId)).value("path").toString();
    QVERIFY(QFileInfo::exists(firstPath));

    // 删除时忽略不存在的标识。
    QVERIFY(ok(service.remove({firstId, QStringLiteral("missing")})));
    QCOMPARE(items(service).size(), 1);
    QVERIFY(!QFileInfo::exists(firstPath));

    QVERIFY(ok(service.clear()));
    QCOMPARE(items(service).size(), 0);
    QDir directory(QFileInfo(firstPath).absolutePath());
    QCOMPARE(directory.entryList({QStringLiteral("*.png")}).size(), 0);
}

void ScreenshotServiceTest::prunesOldest()
{
    QTemporaryDir root;
    ScreenshotService service(root.path());
    QString oldestId;
    QString oldestPath;
    for (int index = 0; index < 101; ++index) {
        const auto added = service.add(sampleImage(64, 48, QColor::fromHsv(index * 3 % 360, 200, 200)), nowIso());
        QVERIFY(ok(added));
        if (index == 0) {
            oldestId = data(added).value("id").toString();
            oldestPath = data(service.read(oldestId)).value("path").toString();
        }
    }
    QCOMPARE(items(service).size(), 100);
    QVERIFY(!ok(service.read(oldestId)));
    QVERIFY(!QFileInfo::exists(oldestPath));
    // 最新一条在最前。
    QVERIFY(!ok(service.read(QStringLiteral("missing"))));
    QVERIFY(items(service).first().toMap().value("id").toString() != oldestId);
}

void ScreenshotServiceTest::rejectsInvalidInput()
{
    QTemporaryDir root;
    ScreenshotService service(root.path());
    QVERIFY(!ok(service.add(QImage(), nowIso())));
    QVERIFY(!ok(service.add(sampleImage(10, 10, Qt::gray), QStringLiteral("不是时间"))));
    QCOMPARE(items(service).size(), 0);
}

void ScreenshotServiceTest::reloadsPersistedData()
{
    QTemporaryDir root;
    QVariantMap before;
    {
        ScreenshotService service(root.path());
        QVERIFY(ok(service.add(sampleImage(300, 180, QColor("#3d78c8")), nowIso())));
        QVERIFY(ok(service.add(sampleImage(220, 140, QColor("#37a356")), nowIso())));
        before = service.snapshot();
        QVERIFY(ok(before));
    }
    ScreenshotService reopened(root.path());
    QCOMPARE(reopened.snapshot(), before);
    // 文件仍然可以完整读取。
    const QString id = items(reopened).first().toMap().value("id").toString();
    QVERIFY(data(reopened.read(id)).contains(QStringLiteral("image")));
}

void ScreenshotServiceTest::protectsCorruptedIndex()
{
    QTemporaryDir root;
    const QString index = root.filePath(QStringLiteral("screenshots/index.v1.json"));
    QVERIFY(writeFile(index, QByteArray("{broken")));
    ScreenshotService service(root.path());
    QVERIFY(!ok(service.snapshot()));
    QVERIFY(!ok(service.add(sampleImage(100, 80, Qt::gray), nowIso())));
    QVERIFY(!ok(service.clear()));
    QCOMPARE(readFile(index), QByteArray("{broken"));

    // 有效 JSON 但记录字段非法时同样保护。
    QVERIFY(writeFile(index, QByteArray("{\"version\":2,\"items\":[]}")));
    ScreenshotService wrongVersion(root.path());
    QVERIFY(!ok(wrongVersion.snapshot()));
    QVERIFY(!ok(wrongVersion.clear()));
    QCOMPARE(readFile(index), QByteArray("{\"version\":2,\"items\":[]}"));
}

void ScreenshotServiceTest::overlaySelectionAndAnnotation()
{
    // 400×300 的合成桌面，四角颜色可辨，便于断言裁剪偏移。
    QImage desktop(400, 300, QImage::Format_RGB32);
    desktop.fill(QColor(250, 250, 250));
    QPainter painter(&desktop);
    painter.fillRect(0, 0, 20, 20, QColor(10, 10, 10));
    painter.end();

    ScreenshotOverlay overlay(desktop);
    overlay.setGeometry(0, 0, 400, 300); // 与图像 1:1，便于坐标换算。
    overlay.show();

    // 尚未选择区域时 Enter 不应结束截图。
    QTest::keyClick(&overlay, Qt::Key_Return);
    QVERIFY(!overlay.isAnnotating());

    // 拖拽出选区 (20,20)-(220,170)。
    sendMouse(overlay, QEvent::MouseButtonPress, QPoint(20, 20), Qt::LeftButton);
    sendMouse(overlay, QEvent::MouseMove, QPoint(120, 80), Qt::LeftButton);
    sendMouse(overlay, QEvent::MouseButtonRelease, QPoint(220, 170), Qt::LeftButton);
    QVERIFY(overlay.isAnnotating());
    QCOMPARE(overlay.selection(), QRectF(20, 20, 200, 150));

    // 画笔画一条红色折线，落点在选区内。
    sendMouse(overlay, QEvent::MouseButtonPress, QPoint(30, 30), Qt::LeftButton);
    sendMouse(overlay, QEvent::MouseMove, QPoint(60, 60), Qt::LeftButton);
    sendMouse(overlay, QEvent::MouseMove, QPoint(100, 100), Qt::LeftButton);
    sendMouse(overlay, QEvent::MouseButtonRelease, QPoint(100, 100), Qt::LeftButton);

    QImage composed;
    bool cancelled = true;
    connect(&overlay, &ScreenshotOverlay::finished, [&composed, &cancelled](const QImage& image, bool wasCancelled) {
        composed = image;
        cancelled = wasCancelled;
    });
    QTest::keyClick(&overlay, Qt::Key_Return);
    QVERIFY(!composed.isNull());
    QVERIFY(!cancelled);
    QCOMPARE(composed.size(), QSize(200, 150));
    // 选区左上角对应桌面 (20,20)，应为背景色而非左上角的黑色方块。
    QVERIFY(colorNear(composed.pixelColor(2, 2), QColor(250, 250, 250)));
    // 笔迹经过 (65,65)：合成图中该点应为红色。
    QVERIFY(colorNear(composed.pixelColor(65, 65), QColor(230, 47, 47)));
    // 笔迹外的点保持背景色。
    QVERIFY(colorNear(composed.pixelColor(180, 140), QColor(250, 250, 250)));
}

void ScreenshotServiceTest::overlayUndoAndCancel()
{
    QImage desktop(400, 300, QImage::Format_RGB32);
    desktop.fill(QColor(250, 250, 250));
    ScreenshotOverlay overlay(desktop);
    overlay.setGeometry(0, 0, 400, 300);
    overlay.show();

    sendMouse(overlay, QEvent::MouseButtonPress, QPoint(20, 20), Qt::LeftButton);
    sendMouse(overlay, QEvent::MouseButtonRelease, QPoint(220, 170), Qt::LeftButton);
    QVERIFY(overlay.isAnnotating());

    // 两条画笔线：第一条穿过 (65,65)，第二条竖线在 (180,*)。
    sendMouse(overlay, QEvent::MouseButtonPress, QPoint(30, 30), Qt::LeftButton);
    sendMouse(overlay, QEvent::MouseMove, QPoint(100, 100), Qt::LeftButton);
    sendMouse(overlay, QEvent::MouseButtonRelease, QPoint(100, 100), Qt::LeftButton);
    sendMouse(overlay, QEvent::MouseButtonPress, QPoint(180, 40), Qt::LeftButton);
    sendMouse(overlay, QEvent::MouseMove, QPoint(180, 130), Qt::LeftButton);
    sendMouse(overlay, QEvent::MouseButtonRelease, QPoint(180, 130), Qt::LeftButton);

    // 通过工具提示找到撤销按钮并点击。
    QToolButton* undo = nullptr;
    for (QToolButton* button : overlay.findChildren<QToolButton*>()) {
        if (button->toolTip() == QStringLiteral("撤销上一步标注"))
            undo = button;
    }
    QVERIFY(undo);
    undo->click();

    QImage composed;
    connect(&overlay, &ScreenshotOverlay::finished, [&composed](const QImage& image, bool) {
        composed = image;
    });
    QTest::keyClick(&overlay, Qt::Key_Return);
    QVERIFY(!composed.isNull());
    QVERIFY(colorNear(composed.pixelColor(65, 65), QColor(230, 47, 47)));
    // 第二条线已被撤销，(180,80) 应保持背景色。
    QVERIFY(colorNear(composed.pixelColor(180, 80), QColor(250, 250, 250)));

    // 新遮罩：Esc 取消后发出空图像且对象销毁。
    QPointer<ScreenshotOverlay> second = new ScreenshotOverlay(desktop);
    second->setGeometry(0, 0, 400, 300);
    second->show();
    bool cancelNotified = false;
    QImage cancelledImage;
    connect(second, &ScreenshotOverlay::finished, [&cancelNotified, &cancelledImage](const QImage& image, bool cancelled) {
        cancelNotified = cancelled;
        cancelledImage = image;
    });
    QTest::keyClick(second, Qt::Key_Escape);
    QVERIFY(cancelNotified);
    QVERIFY(cancelledImage.isNull());
    // 取消后由调用方负责销毁（与 AppBridge 的 deleteLater 一致）。
    second->deleteLater();
    QTest::qWait(50);
    QVERIFY(second.isNull());
}

void ScreenshotServiceTest::overlaySecondaryScreenGeometry()
{
    // 模拟副屏：遮罩摆在非原点的全局几何位置，映射必须只依赖遮罩局部坐标，
    // 否则多显示器下选区与裁剪会随屏幕偏移错位。
    QImage desktop(400, 300, QImage::Format_RGB32);
    desktop.fill(QColor(250, 250, 250));
    QPainter painter(&desktop);
    painter.fillRect(0, 0, 20, 20, QColor(10, 10, 10));
    painter.end();
    ScreenshotOverlay overlay(desktop);
    overlay.setGeometry(1920, 100, 400, 300);
    overlay.show();

    sendMouse(overlay, QEvent::MouseButtonPress, QPoint(20, 20), Qt::LeftButton);
    sendMouse(overlay, QEvent::MouseButtonRelease, QPoint(220, 170), Qt::LeftButton);
    QVERIFY(overlay.isAnnotating());
    QCOMPARE(overlay.selection(), QRectF(20, 20, 200, 150));

    sendMouse(overlay, QEvent::MouseButtonPress, QPoint(30, 30), Qt::LeftButton);
    sendMouse(overlay, QEvent::MouseMove, QPoint(100, 100), Qt::LeftButton);
    sendMouse(overlay, QEvent::MouseButtonRelease, QPoint(100, 100), Qt::LeftButton);

    QImage composed;
    connect(&overlay, &ScreenshotOverlay::finished, [&composed](const QImage& image, bool) {
        composed = image;
    });
    QTest::keyClick(&overlay, Qt::Key_Return);
    QCOMPARE(composed.size(), QSize(200, 150));
    // 选区从本屏 (20,20) 开始：屏幕自身的黑色角块不会进入裁剪结果。
    QVERIFY(colorNear(composed.pixelColor(2, 2), QColor(250, 250, 250)));
    QVERIFY(colorNear(composed.pixelColor(65, 65), QColor(230, 47, 47)));
}

QTEST_MAIN(ScreenshotServiceTest)
#include "ScreenshotServiceTest.moc"
