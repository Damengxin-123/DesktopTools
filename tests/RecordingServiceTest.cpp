#include "app/RecordingOverlay.h"
#include "services/RecordingService.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMouseEvent>
#include <QPointer>
#include <QTemporaryDir>
#include <QtTest>

namespace {
// 提取服务的成功状态。
bool ok(const QVariantMap& result) { return result.value("ok").toBool(); }
// 提取服务返回的数据对象。
QVariantMap data(const QVariantMap& result) { return result.value("data").toMap(); }
// 获取当前历史摘要列表。
QVariantList items(const RecordingService& service) { return data(service.snapshot()).value("items").toList(); }
// 当前时间戳，格式与服务内部一致。
QString nowIso()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}
// 写入指定内容，用于构造假视频文件与损坏索引。
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
// 直接向遮罩投递鼠标事件；offscreen 平台的 QTest::mouseMove 依赖真实游标，不可靠。
void sendMouse(RecordingOverlay& overlay, QEvent::Type type, const QPoint& position,
    Qt::MouseButton button)
{
    QMouseEvent event(type, QPointF(position), QPointF(overlay.mapToGlobal(position)),
        button, button, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &event);
}
} // namespace

// 覆盖录屏历史的登记、删除持久化与保护，以及选区遮罩的拖拽、确认与取消。
class RecordingServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    // 预约路径登记记录，快照与完整读取字段齐全。
    void reservesAndAdds();
    // 单条删除删除视频文件，清空清空全部文件。
    void removesAndClears();
    // 超过 100 条自动移除最早的记录及文件。
    void prunesOldest();
    // 路径不在数据目录、文件不存在、元数据无效时拒绝登记。
    void rejectsInvalidInput();
    // 同一数据目录重新加载后记录保持一致。
    void reloadsPersistedData();
    // 索引损坏时进入只读保护，拒绝覆盖原文件。
    void protectsCorruptedIndex();

    // 遮罩：拖拽进入确认阶段，工具栏可见，确认发出选区。
    void overlaySelectionAndConfirm();
    // 遮罩：框内拖动移动选区、框外拖动重新选区、过小拖拽视为误触。
    void overlayMoveAndReselect();
    // 遮罩：Esc、右键与直接关闭按取消处理。
    void overlayCancelPaths();
};

void RecordingServiceTest::reservesAndAdds()
{
    QTemporaryDir root;
    RecordingService service(root.path());
    int changes = 0;
    connect(&service, &RecordingService::changed, [&changes]() { ++changes; });

    // 预约的路径必须位于数据目录的 recordings 子目录中，且目录已创建。
    const auto reserved = service.reserve();
    QVERIFY(ok(reserved));
    const QString path = data(reserved).value("path").toString();
    QVERIFY(path.endsWith(QStringLiteral(".mp4")));
    QVERIFY(path.contains(QStringLiteral("recordings")));
    QVERIFY(QFileInfo(QDir::cleanPath(path)).absolutePath() == QDir(root.path()).absoluteFilePath(QStringLiteral("recordings")));
    QVERIFY(QDir(QFileInfo(path).absolutePath()).exists());

    // 模拟录制器写出的视频文件。
    QVERIFY(writeFile(path, QByteArray("fake mp4 payload")));
    QVariantMap meta;
    meta.insert(QStringLiteral("recordedAt"), nowIso());
    meta.insert(QStringLiteral("width"), 640);
    meta.insert(QStringLiteral("height"), 360);
    meta.insert(QStringLiteral("durationMs"), 4200);
    meta.insert(QStringLiteral("thumbnail"), QStringLiteral("data:image/png;base64,iVBORw0KGgo="));
    const auto added = service.add(path, meta);
    QVERIFY(ok(added));
    const QString id = data(added).value("id").toString();
    QVERIFY(!id.isEmpty());
    QCOMPARE(data(added).value("width").toInt(), 640);
    QCOMPARE(data(added).value("durationMs").toLongLong(), qint64(4200));
    QCOMPARE(changes, 1);

    // 快照不含路径。
    const QVariantList summaries = items(service);
    QCOMPARE(summaries.size(), 1);
    QVERIFY(!summaries.first().toMap().contains(QStringLiteral("path")));
    QVERIFY(QDateTime::fromString(
        summaries.first().toMap().value("recordedAt").toString(), Qt::ISODateWithMs).isValid());

    // 完整读取包含磁盘路径，文件确实存在。
    const QVariantMap full = data(service.read(id));
    QVERIFY(QFileInfo::exists(full.value("path").toString()));
    QVERIFY(full.value("path").toString().endsWith(QStringLiteral(".mp4")));
    QVERIFY(!ok(service.read(QStringLiteral("missing"))));
}

void RecordingServiceTest::removesAndClears()
{
    QTemporaryDir root;
    RecordingService service(root.path());
    // lambda 内不能使用 QVERIFY（失败时 return; 会破坏返回类型推导），先写入再登记。
    const auto makeRecord = [&service](const QByteArray& payload) -> QVariantMap {
        const auto reserved = service.reserve();
        const QString path = data(reserved).value("path").toString();
        writeFile(path, payload);
        QVariantMap meta;
        meta.insert(QStringLiteral("recordedAt"), nowIso());
        meta.insert(QStringLiteral("width"), 320);
        meta.insert(QStringLiteral("height"), 200);
        meta.insert(QStringLiteral("durationMs"), 1000);
        return service.add(path, meta);
    };
    QVERIFY(ok(makeRecord(QByteArray("first"))));
    QVERIFY(ok(makeRecord(QByteArray("second"))));
    const QString firstId = items(service).first().toMap().value("id").toString();
    const QString firstPath = data(service.read(firstId)).value("path").toString();
    QVERIFY(QFileInfo::exists(firstPath));

    // 删除时忽略不存在的标识。
    QVERIFY(ok(service.remove({firstId, QStringLiteral("missing")})));
    QCOMPARE(items(service).size(), 1);
    QVERIFY(!QFileInfo::exists(firstPath));

    QVERIFY(ok(service.clear()));
    QCOMPARE(items(service).size(), 0);
    QDir directory(root.path() + QStringLiteral("/recordings"));
    QCOMPARE(directory.entryList({QStringLiteral("*.mp4")}).size(), 0);
}

void RecordingServiceTest::prunesOldest()
{
    QTemporaryDir root;
    RecordingService service(root.path());
    QString oldestId;
    QString oldestPath;
    for (int index = 0; index < 101; ++index) {
        const auto reserved = service.reserve();
        const QString path = data(reserved).value("path").toString();
        QVERIFY(writeFile(path, QByteArray("payload")));
        QVariantMap meta;
        meta.insert(QStringLiteral("recordedAt"), nowIso());
        meta.insert(QStringLiteral("width"), 64);
        meta.insert(QStringLiteral("height"), 48);
        meta.insert(QStringLiteral("durationMs"), index);
        const auto added = service.add(path, meta);
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
    QVERIFY(items(service).first().toMap().value("id").toString() != oldestId);
}

void RecordingServiceTest::rejectsInvalidInput()
{
    QTemporaryDir root;
    RecordingService service(root.path());
    const auto reserved = service.reserve();
    QVERIFY(ok(reserved));
    const QString path = data(reserved).value("path").toString();

    // 文件不存在。
    QVariantMap meta;
    meta.insert(QStringLiteral("recordedAt"), nowIso());
    meta.insert(QStringLiteral("width"), 320);
    meta.insert(QStringLiteral("height"), 200);
    meta.insert(QStringLiteral("durationMs"), 10);
    QVERIFY(!ok(service.add(path, meta)));
    QVERIFY(writeFile(path, QByteArray("payload")));
    // 无效时间。
    QVariantMap badTime = meta;
    badTime.insert(QStringLiteral("recordedAt"), QStringLiteral("不是时间"));
    QVERIFY(!ok(service.add(path, badTime)));
    // 无效尺寸。
    QVariantMap badSize = meta;
    badSize.insert(QStringLiteral("width"), 0);
    QVERIFY(!ok(service.add(path, badSize)));
    // 无效缩略图。
    QVariantMap badThumb = meta;
    badThumb.insert(QStringLiteral("thumbnail"), QStringLiteral("http://example.com/a.png"));
    QVERIFY(!ok(service.add(path, badThumb)));
    // 路径不在数据目录中。
    QVERIFY(writeFile(root.path() + QStringLiteral("/outside.mp4"), QByteArray("x")));
    QVERIFY(!ok(service.add(root.path() + QStringLiteral("/outside.mp4"), meta)));
    QCOMPARE(items(service).size(), 0);

    // 登记成功后再验证空缩略图（录制极短时的合法情况）。
    QVariantMap noThumb = meta;
    noThumb.insert(QStringLiteral("thumbnail"), QString());
    QVERIFY(ok(service.add(path, noThumb)));
    QCOMPARE(data(service.read(data(service.snapshot()).value("items").toList().first().toMap().value("id").toString()))
                 .value("thumbnail").toString(), QString());
}

void RecordingServiceTest::reloadsPersistedData()
{
    QTemporaryDir root;
    QVariantMap before;
    QString filePath;
    {
        RecordingService service(root.path());
        for (int index = 0; index < 2; ++index) {
            const auto reserved = service.reserve();
            const QString path = data(reserved).value("path").toString();
            QVERIFY(writeFile(path, QByteArray("payload")));
            QVariantMap meta;
            meta.insert(QStringLiteral("recordedAt"), nowIso());
            meta.insert(QStringLiteral("width"), 300);
            meta.insert(QStringLiteral("height"), 180);
            meta.insert(QStringLiteral("durationMs"), 500 + index);
            QVERIFY(ok(service.add(path, meta)));
            filePath = path;
        }
        before = service.snapshot();
        QVERIFY(ok(before));
    }
    RecordingService reopened(root.path());
    QCOMPARE(reopened.snapshot(), before);
    QVERIFY(QFileInfo::exists(filePath));
}

void RecordingServiceTest::protectsCorruptedIndex()
{
    QTemporaryDir root;
    const QString index = root.filePath(QStringLiteral("recordings/index.v1.json"));
    QVERIFY(writeFile(index, QByteArray("{broken")));
    RecordingService service(root.path());
    QVERIFY(!ok(service.snapshot()));
    QVERIFY(!ok(service.reserve()));
    QVERIFY(!ok(service.clear()));
    QCOMPARE(readFile(index), QByteArray("{broken"));

    // 有效 JSON 但记录字段非法时同样保护。
    QVERIFY(writeFile(index,
        QByteArray("{\"version\":1,\"items\":[{\"id\":\"a\",\"file\":\"../evil.mp4\"}]}")));
    RecordingService invalid(root.path());
    QVERIFY(!ok(invalid.snapshot()));
    QVERIFY(!ok(invalid.clear()));
}

void RecordingServiceTest::overlaySelectionAndConfirm()
{
    QImage desktop(400, 300, QImage::Format_RGB32);
    desktop.fill(QColor(250, 250, 250));
    RecordingOverlay overlay(desktop);
    overlay.setGeometry(0, 0, 400, 300); // 与图像 1:1，便于坐标换算。
    overlay.show();

    // 尚未选择区域时 Enter 不应确认。
    QTest::keyClick(&overlay, Qt::Key_Return);
    QVERIFY(!overlay.isConfirming());

    // 拖拽出选区 (20,20)-(220,170)。
    sendMouse(overlay, QEvent::MouseButtonPress, QPoint(20, 20), Qt::LeftButton);
    sendMouse(overlay, QEvent::MouseMove, QPoint(120, 80), Qt::LeftButton);
    sendMouse(overlay, QEvent::MouseButtonRelease, QPoint(220, 170), Qt::LeftButton);
    QVERIFY(overlay.isConfirming());
    QCOMPARE(overlay.selection(), QRectF(20, 20, 200, 150));
    // 确认工具栏可见。
    QWidget* toolbar = overlay.findChild<QWidget*>(QStringLiteral("recordingToolbar"));
    QVERIFY(toolbar);
    QVERIFY(toolbar->isVisible());

    // 松开鼠标后选区必须锁定：无按键的移动不得再改变选区。
    sendMouse(overlay, QEvent::MouseMove, QPoint(300, 250), Qt::NoButton);
    QCOMPARE(overlay.selection(), QRectF(20, 20, 200, 150));

    // Enter 确认：发出选区并关闭。
    QRectF confirmed;
    bool confirmClosed = false;
    connect(&overlay, &RecordingOverlay::confirmRequested,
        [&confirmed](const QRectF& selection) { confirmed = selection; });
    connect(&overlay, &RecordingOverlay::cancelled, [&confirmClosed]() { confirmClosed = true; });
    QTest::keyClick(&overlay, Qt::Key_Return);
    QCOMPARE(confirmed, QRectF(20, 20, 200, 150));
    QVERIFY(!confirmClosed);
    QVERIFY(!overlay.isVisible());
}

void RecordingServiceTest::overlayMoveAndReselect()
{
    QImage desktop(400, 300, QImage::Format_RGB32);
    desktop.fill(QColor(250, 250, 250));
    RecordingOverlay overlay(desktop);
    overlay.setGeometry(0, 0, 400, 300);
    overlay.show();

    sendMouse(overlay, QEvent::MouseButtonPress, QPoint(100, 100), Qt::LeftButton);
    sendMouse(overlay, QEvent::MouseButtonRelease, QPoint(300, 200), Qt::LeftButton);
    QVERIFY(overlay.isConfirming());
    QWidget* toolbar = overlay.findChild<QWidget*>(QStringLiteral("recordingToolbar"));
    QVERIFY(toolbar && toolbar->isVisible());

    // 框内拖动整体平移选区，工具栏重新摆放且仍然可见。
    sendMouse(overlay, QEvent::MouseButtonPress, QPoint(200, 150), Qt::LeftButton);
    QVERIFY(!toolbar->isVisible());
    sendMouse(overlay, QEvent::MouseMove, QPoint(220, 170), Qt::LeftButton);
    sendMouse(overlay, QEvent::MouseButtonRelease, QPoint(220, 170), Qt::LeftButton);
    QCOMPARE(overlay.selection(), QRectF(120, 120, 200, 100));
    QVERIFY(toolbar->isVisible());

    // 框外拖动表示重新框选，替换旧选区。
    sendMouse(overlay, QEvent::MouseButtonPress, QPoint(20, 20), Qt::LeftButton);
    QVERIFY(!toolbar->isVisible());
    sendMouse(overlay, QEvent::MouseButtonRelease, QPoint(90, 90), Qt::LeftButton);
    QCOMPARE(overlay.selection(), QRectF(20, 20, 70, 70));
    QVERIFY(overlay.isConfirming());
    QVERIFY(toolbar->isVisible());

    // 过小的拖拽视为误触：恢复原选区而不是清空。
    sendMouse(overlay, QEvent::MouseButtonPress, QPoint(350, 150), Qt::LeftButton);
    sendMouse(overlay, QEvent::MouseButtonRelease, QPoint(353, 152), Qt::LeftButton);
    QCOMPARE(overlay.selection(), QRectF(20, 20, 70, 70));
    QVERIFY(overlay.isConfirming());
}

void RecordingServiceTest::overlayCancelPaths()
{
    QImage desktop(400, 300, QImage::Format_RGB32);
    desktop.fill(QColor(250, 250, 250));

    // Esc 取消。
    {
        RecordingOverlay overlay(desktop);
        overlay.setGeometry(0, 0, 400, 300);
        overlay.show();
        sendMouse(overlay, QEvent::MouseButtonPress, QPoint(20, 20), Qt::LeftButton);
        sendMouse(overlay, QEvent::MouseButtonRelease, QPoint(220, 170), Qt::LeftButton);
        QVERIFY(overlay.isConfirming());
        bool cancelled = false;
        bool confirmed = false;
        connect(&overlay, &RecordingOverlay::cancelled, [&cancelled]() { cancelled = true; });
        connect(&overlay, &RecordingOverlay::confirmRequested, [&confirmed](const QRectF&) { confirmed = true; });
        QTest::keyClick(&overlay, Qt::Key_Escape);
        QVERIFY(cancelled);
        QVERIFY(!confirmed);
        QVERIFY(!overlay.isVisible());
    }
    // 右键取消。
    {
        RecordingOverlay overlay(desktop);
        overlay.setGeometry(0, 0, 400, 300);
        overlay.show();
        bool cancelled = false;
        connect(&overlay, &RecordingOverlay::cancelled, [&cancelled]() { cancelled = true; });
        sendMouse(overlay, QEvent::MouseButtonPress, QPoint(200, 150), Qt::RightButton);
        QVERIFY(cancelled);
    }
    // 直接关闭按取消处理。
    {
        QPointer<RecordingOverlay> overlay = new RecordingOverlay(desktop);
        overlay->setGeometry(0, 0, 400, 300);
        overlay->show();
        bool cancelled = false;
        connect(overlay, &RecordingOverlay::cancelled, [&cancelled]() { cancelled = true; });
        overlay->close();
        QVERIFY(cancelled);
        // 取消后由调用方负责销毁（与 AppBridge 的 deleteLater 一致）。
        overlay->deleteLater();
        QTest::qWait(50);
        QVERIFY(overlay.isNull());
    }
}

QTEST_MAIN(RecordingServiceTest)
#include "RecordingServiceTest.moc"
