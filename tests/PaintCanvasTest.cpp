#include "app/PaintCanvas.h"
#include "app/PaintWindow.h"
#include <QApplication>
#include <QCheckBox>
#include <QAction>
#include <QFileDialog>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QTabletEvent>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

// 覆盖用户核心诉求及基本绘图行为，使用真实 Qt 输入事件而非直接改写内部状态。
class PaintCanvasTest final : public QObject
{
    Q_OBJECT
private:
    // 向画布发送带明确左键状态的鼠标事件。
    void mouse(PaintCanvas& canvas, QEvent::Type type, const QPoint& point, bool down)
    {
        const auto button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent event(type, point, canvas.mapToGlobal(point), button, down ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &event);
    }
    // 初始化小尺寸白色画布并让其接收键盘事件。
    void prepare(PaintCanvas& canvas)
    {
        QImage blank(240, 160, QImage::Format_ARGB32);
        blank.fill(Qt::white);
        canvas.setImage(blank);
        canvas.show();
        canvas.activateWindow();
        canvas.setFocus();
        QTest::qWait(20);
    }
private slots:
    // 普通绘图、历史、清空以及保存标记保持一致。
    void normalAndHistory()
    {
        PaintCanvas canvas;
        prepare(canvas);
        mouse(canvas, QEvent::MouseButtonPress, {20, 20}, true);
        mouse(canvas, QEvent::MouseMove, {80, 20}, true);
        mouse(canvas, QEvent::MouseButtonRelease, {90, 20}, false);
        QVERIFY(canvas.image().pixelColor(50, 20) != QColor(Qt::white));
        QVERIFY(canvas.isModified());
        canvas.undo();
        QVERIFY(!canvas.isModified());
        QVERIFY(!canvas.canUndo());
        canvas.redo();
        QVERIFY(canvas.isModified());
        canvas.markSaved();
        canvas.clear();
        QVERIFY(canvas.isModified());
        canvas.undo();
        QVERIFY(!canvas.isModified());
    }
    // 左键持续按住时，空格只控制当前独立笔画，定位轨迹不得连线。
    void tabletGateAndFocus()
    {
        PaintCanvas canvas;
        prepare(canvas);
        canvas.setTabletMode(true);
        mouse(canvas, QEvent::MouseButtonPress, {10, 40}, true);
        mouse(canvas, QEvent::MouseMove, {30, 40}, true);
        QVERIFY(!canvas.isModified());
        QTest::keyPress(&canvas, Qt::Key_Space);
        mouse(canvas, QEvent::MouseMove, {60, 40}, true);
        QTest::keyRelease(&canvas, Qt::Key_Space);
        mouse(canvas, QEvent::MouseMove, {110, 40}, true);
        QCOMPARE(canvas.image().pixelColor(85, 40), QColor(Qt::white));
        QTest::keyPress(&canvas, Qt::Key_Space);
        mouse(canvas, QEvent::MouseMove, {140, 40}, true);
        QTest::keyRelease(&canvas, Qt::Key_Space);
        QCOMPARE(canvas.image().pixelColor(85, 40), QColor(Qt::white));
        QVERIFY(canvas.image().pixelColor(120, 40) != QColor(Qt::white));
        canvas.undo();
        QCOMPARE(canvas.image().pixelColor(120, 40), QColor(Qt::white));
        QVERIFY(canvas.image().pixelColor(45, 40) != QColor(Qt::white));
        canvas.setDrawingKey(Qt::Key_F);
        mouse(canvas, QEvent::MouseButtonPress, {20, 90}, true);
        QTest::keyPress(&canvas, Qt::Key_Space);
        mouse(canvas, QEvent::MouseMove, {40, 90}, true);
        QCOMPARE(canvas.image().pixelColor(30, 90), QColor(Qt::white));
        QTest::keyRelease(&canvas, Qt::Key_Space);
        QTest::keyPress(&canvas, Qt::Key_F);
        mouse(canvas, QEvent::MouseMove, {60, 90}, true);
        QEvent deactivate(QEvent::WindowDeactivate);
        QApplication::sendEvent(&canvas, &deactivate);
        mouse(canvas, QEvent::MouseMove, {110, 90}, true);
        QCOMPARE(canvas.image().pixelColor(85, 90), QColor(Qt::white));
        QTest::keyRelease(&canvas, Qt::Key_F);
    }
    // 原生笔事件与鼠标兼容模式使用相同门控，未授权笔触不得修改图像。
    void nativePen()
    {
        PaintCanvas canvas;
        prepare(canvas);
        canvas.setTabletMode(true);
        QPointingDevice pen(QStringLiteral("测试数位笔"), 42, QInputDevice::DeviceType::Stylus,
            QPointingDevice::PointerType::Pen, QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 1);
        // 模拟笔尖接触、移动和抬笔，检查事件已被接收以避免重复合成鼠标输入。
        const auto send = [&](QEvent::Type type, const QPoint& point, bool down) {
            QTabletEvent event(type, &pen, point, canvas.mapToGlobal(point), down ? 0.5 : 0.0,
                0, 0, 0, 0, 0, Qt::NoModifier, type == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton,
                down ? Qt::LeftButton : Qt::NoButton);
            QApplication::sendEvent(&canvas, &event);
            QVERIFY(event.isAccepted());
        };
        send(QEvent::TabletPress, {20, 60}, true);
        send(QEvent::TabletMove, {50, 60}, true);
        QVERIFY(!canvas.isModified());
        QTest::keyPress(&canvas, Qt::Key_Space);
        send(QEvent::TabletMove, {80, 60}, true);
        QTest::keyRelease(&canvas, Qt::Key_Space);
        send(QEvent::TabletMove, {140, 60}, true);
        send(QEvent::TabletRelease, {140, 60}, false);
        QVERIFY(canvas.image().pixelColor(65, 60) != QColor(Qt::white));
        QCOMPARE(canvas.image().pixelColor(110, 60), QColor(Qt::white));
    }
    // 形状预览、区域填色和缩放坐标保持原图像素准确。
    void shapesAndFill()
    {
        PaintCanvas canvas;
        prepare(canvas);
        canvas.setZoom(2.0);
        canvas.setTool(PaintCanvas::Tool::Rectangle);
        mouse(canvas, QEvent::MouseButtonPress, {40, 40}, true);
        mouse(canvas, QEvent::MouseMove, {160, 120}, true);
        QVERIFY(!canvas.isModified());
        mouse(canvas, QEvent::MouseButtonRelease, {160, 120}, false);
        QVERIFY(canvas.image().pixelColor(20, 40) != QColor(Qt::white));
        canvas.setTool(PaintCanvas::Tool::Fill);
        canvas.setColor(Qt::red);
        mouse(canvas, QEvent::MouseButtonPress, {80, 80}, true);
        mouse(canvas, QEvent::MouseButtonRelease, {80, 80}, false);
        QCOMPARE(canvas.image().pixelColor(40, 40), QColor(Qt::red));
        QCOMPARE(canvas.image().pixelColor(100, 100), QColor(Qt::white));
        canvas.undo();
        QCOMPARE(canvas.image().pixelColor(40, 40), QColor(Qt::white));
    }
    // 使用临时数据目录验证窗口构造及数位板偏好恢复，并生成布局检查图。
    void windowPreferences()
    {
        QTemporaryDir data;
        QVERIFY(data.isValid());
        auto* window = new PaintWindow(data.path());
        window->show();
        auto* mode = window->findChild<QCheckBox*>(QStringLiteral("paint-tablet-mode"));
        QVERIFY(mode);
        mode->setChecked(true);
        QTest::qWait(30);
        const QString capture = qEnvironmentVariable("DESKTOPTOOL_PAINT_CAPTURE");
        if (!capture.isEmpty()) QVERIFY(window->grab().save(capture));
        QVERIFY(window->close());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        auto* reopened = new PaintWindow(data.path());
        QVERIFY(reopened->findChild<QCheckBox*>(QStringLiteral("paint-tablet-mode"))->isChecked());
        QVERIFY(reopened->close());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    // 验证关闭取消确实保留作品，以及保存 PNG 后可从磁盘还原像素。
    void saveAndClose()
    {
        QTemporaryDir data;
        QVERIFY(data.isValid());
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        auto* window = new PaintWindow(data.path());
        window->show();
        auto* canvas = window->findChild<PaintCanvas*>();
        QVERIFY(canvas);
        mouse(*canvas, QEvent::MouseButtonPress, {20, 20}, true);
        mouse(*canvas, QEvent::MouseButtonRelease, {50, 20}, false);
        QVERIFY(canvas->isModified());
        QTimer::singleShot(0, [] {
            if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                dialog->done(QMessageBox::Cancel);
        });
        QVERIFY(!window->close());
        QVERIFY(window->isVisible());
        const QString path = data.filePath(QStringLiteral("测试画作.png"));
        // 在真正的文件对话框中选择临时目录，避免接触用户作品。
        QTimer::singleShot(0, [path] {
            if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
                dialog->selectFile(path);
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
            }
        });
        QAction* save = nullptr;
        for (auto* action : window->findChildren<QAction*>())
            if (action->text() == QStringLiteral("保存")) save = action;
        QVERIFY(save);
        save->trigger();
        QVERIFY(!canvas->isModified());
        const QImage restored(path);
        QVERIFY(!restored.isNull());
        QCOMPARE(restored.pixelColor(35, 20), canvas->image().pixelColor(35, 20));
        QCOMPARE(restored.size(), canvas->image().size());
        QVERIFY(window->close());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
};

QTEST_MAIN(PaintCanvasTest)
#include "PaintCanvasTest.moc"
