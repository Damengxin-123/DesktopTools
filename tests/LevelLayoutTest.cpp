#include "level/LevelCanvas.h"
#include "level/LevelWindow.h"
#include <QApplication>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>
#include <QXmlStreamReader>
#include <QDir>
#include <QMouseEvent>
#include <QMessageBox>
#include <QAbstractButton>
#include <QTimer>
#include <cmath>

// 以实际输入、项目文件和导出文件验证关卡编辑的核心流程。
class LevelLayoutTest final : public QObject {
    Q_OBJECT
private:
    // 向视口发送明确的拖动事件，不依赖物理鼠标。
    void mouse(LevelCanvas& canvas, QEvent::Type type, QPointF point, bool down)
    {
        QMouseEvent event(type,point,canvas.mapToGlobal(point.toPoint()),type==QEvent::MouseMove?Qt::NoButton:Qt::LeftButton,
                          down?Qt::LeftButton:Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(&canvas,&event);
    }
private slots:
    // 保存超过常见固定数组大小的节点，并确认完整恢复样式与设置。
    void roundTrip()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        LevelDocument source; source.title=QStringLiteral("海滨广场"); source.angleStep=10; source.quantize=true; source.unit="cm";
        auto object=LevelDrawing::preset(QStringLiteral("绿化"),QPointF(12,15));
        object.points.clear(); object.fill=QColor(12,180,80,170); object.width=.45;
        for (int i=0;i<1024;++i) { const double angle=i*6.28318530718/1024; object.points.append(QPointF(std::cos(angle)*10,std::sin(angle)*10)); }
        source.objects.append(object);
        const auto project=directory.filePath("海滨.level.json"); QString error;
        QVERIFY2(source.write(project,error),qPrintable(error));
        LevelDocument restored; QVERIFY2(LevelDocument::read(project,restored,error),qPrintable(error));
        QCOMPARE(restored.json(),source.json()); QCOMPARE(restored.objects[0].points.size(),1024);
        auto damaged=source.json(); auto entries=damaged["objects"].toArray(); auto bad=entries[0].toObject();
        bad["points"]=QJsonArray{QJsonArray{1,"错误坐标"}}; entries[0]=bad; damaged["objects"]=entries;
        QVERIFY(!LevelDocument::parse(damaged,restored,error)); QCOMPARE(restored.json(),source.json());
        damaged=source.json(); entries=damaged["objects"].toArray(); entries.append(entries.first()); damaged["objects"]=entries;
        QVERIFY(!LevelDocument::parse(damaged,restored,error));
    }
    // 使用旋转后的前一段验证吸附基准为顶点夹角，并保持鼠标长度。
    void relativeAngles()
    {
        LevelDocument document; document.quantize=true; document.angleStep=10;
        const QPolygonF points{QPointF(0,0),QPointF(3,4)};
        for (int degree=-179;degree<=179;++degree) {
            const QPointF target=points.last()+QPointF(std::cos(degree*.01745329252)*7,std::sin(degree*.01745329252)*7);
            const auto snapped=LevelDrawing::snapped(points,target,document);
            const auto a=points.first()-points.last(),b=snapped-points.last();
            const double angle=std::abs(std::remainder((std::atan2(b.y(),b.x())-std::atan2(a.y(),a.x()))*180/3.141592653589793,360.));
            QVERIFY(std::abs(angle/10-std::round(angle/10))<1e-8);
            QVERIFY(std::abs(QLineF(points.last(),snapped).length()-7)<1e-8);
        }
        document.quantize=false; document.angles=true;
        const auto result=LevelDrawing::snapped(QPolygonF{QPointF(0,0)},QPointF(10,.2),document);
        QVERIFY(std::abs(result.y())<1e-8);
        document.angles=false; QCOMPARE(LevelDrawing::snapped(points,QPointF(11,7),document),QPointF(11,7));
    }
    // 真实鼠标绘制折线、编辑节点并验证撤销恢复几何。
    void drawingAndEditing()
    {
        LevelCanvas canvas; canvas.resize(800,600); canvas.show(); canvas.setTool("polyline");
        QTest::mouseClick(&canvas,Qt::LeftButton,Qt::NoModifier,{100,100});
        QTest::mouseClick(&canvas,Qt::LeftButton,Qt::NoModifier,{220,100});
        QTest::mouseClick(&canvas,Qt::LeftButton,Qt::NoModifier,{220,220});
        QTest::keyClick(&canvas,Qt::Key_Return);
        QCOMPARE(canvas.document().objects.size(),1); QCOMPARE(canvas.document().objects[0].points.size(),3);
        const auto original=canvas.document().objects[0];
        canvas.setTool("select"); canvas.select(0);
        mouse(canvas,QEvent::MouseButtonPress,{220,100},true);
        mouse(canvas,QEvent::MouseMove,{260,120},true);
        mouse(canvas,QEvent::MouseButtonRelease,{260,120},false);
        QCOMPARE(canvas.document().objects[0].points[1],canvas.world({260,120}));
        QCOMPARE(canvas.document().objects[0].points[0],original.points[0]);
        canvas.undo(); QCOMPARE(canvas.document().objects[0].json(),original.json());
        canvas.redo(); QVERIFY(canvas.document().objects[0].points[1]!=original.points[1]);
        canvas.select(0); canvas.duplicateSelected(); QCOMPARE(canvas.document().objects.size(),2);
        QVERIFY(canvas.document().objects[0].id!=canvas.document().objects[1].id);
        canvas.removeSelected(); QCOMPARE(canvas.document().objects.size(),1);
        canvas.setTool("polygon"); QTest::mouseClick(&canvas,Qt::LeftButton,Qt::NoModifier,{300,300});
        QVERIFY(!canvas.finish()); canvas.cancelDraft(); QVERIFY(!canvas.hasDraft());
    }
    // 预设逐点生成任意形状，草稿不提前入库，样式与道路连续节点完整保存。
    void presetDrawing()
    {
        LevelCanvas canvas; canvas.resize(800,600); canvas.show();
        canvas.setTool(QStringLiteral("preset:绿化"));
        QCOMPARE(canvas.drawingStyle().preset,QStringLiteral("绿化"));
        QVERIFY(canvas.drawingStyle().points.isEmpty());
        const QList<QPoint> boundary{{100,100},{260,100},{260,160},{180,160},{180,240},{100,240}};
        for (const auto& point : boundary) {
            QTest::mouseClick(&canvas,Qt::LeftButton,Qt::NoModifier,point);
            QCOMPARE(canvas.document().objects.size(),0);
        }
        auto style=canvas.drawingStyle(); style.fill=QColor("#73b98b"); canvas.setDrawingStyle(style);
        QTest::keyClick(&canvas,Qt::Key_Return);
        QCOMPARE(canvas.document().objects.size(),1);
        const auto green=canvas.document().objects.first();
        QCOMPARE(green.kind,QString("polygon")); QCOMPARE(green.preset,QStringLiteral("绿化"));
        QCOMPARE(green.fill,style.fill); QCOMPARE(green.points.size(),boundary.size());
        for (qsizetype i=0;i<boundary.size();++i) QCOMPARE(green.points[i],canvas.world(boundary[i]));
        QVERIFY(green.name.startsWith(QStringLiteral("绿化")));
        canvas.setTool(QStringLiteral("preset:道路"));
        QCOMPARE(canvas.drawingStyle().width,4.00);
        const QList<QPoint> route{{330,120},{420,160},{450,280},{580,330}};
        for (const auto& point : route) QTest::mouseClick(&canvas,Qt::LeftButton,Qt::NoModifier,point);
        QTest::keyClick(&canvas,Qt::Key_Backspace);
        QTest::mouseClick(&canvas,Qt::LeftButton,Qt::NoModifier,route.last());
        style=canvas.drawingStyle(); style.width=6.00; canvas.setDrawingStyle(style);
        QTest::mouseClick(&canvas,Qt::RightButton);
        QCOMPARE(canvas.document().objects.size(),2);
        const auto road=canvas.document().objects.last();
        QCOMPARE(road.kind,QString("polyline")); QCOMPARE(road.preset,QStringLiteral("道路"));
        QCOMPARE(road.width,6.00); QCOMPARE(road.points.size(),route.size());
        for (qsizetype i=0;i<route.size();++i) QCOMPARE(road.points[i],canvas.world(route[i]));
        canvas.undo(); QCOMPARE(canvas.document().objects.size(),1);
        canvas.redo(); QCOMPARE(canvas.document().objects.last().json(),road.json());
        QTest::mouseClick(&canvas,Qt::LeftButton,Qt::NoModifier,{600,400});
        QVERIFY(!canvas.finish()); QTest::keyClick(&canvas,Qt::Key_Escape);
        QVERIFY(!canvas.hasDraft()); QCOMPARE(canvas.document().objects.size(),2);
        canvas.setTool("polygon"); QVERIFY(canvas.drawingStyle().preset.isEmpty());
        QTemporaryDir directory; QString error; LevelDocument restored;
        const auto path=directory.filePath("逐点绘制.level.json");
        QVERIFY2(canvas.document().write(path,error),qPrintable(error));
        QVERIFY2(LevelDocument::read(path,restored,error),qPrintable(error));
        QCOMPARE(restored.json(),canvas.document().json());
    }
    // PNG 含实际像素，SVG 是可解析矢量并携带每个对象的语义名称。
    void exportAndCatalog()
    {
        QTemporaryDir directory; LevelDocument document; document.title=QStringLiteral("导出 <广场> & 公园");
        document.objects={LevelDrawing::preset(QStringLiteral("绿化"),{0,0}),LevelDrawing::preset(QStringLiteral("道路"),{0,6}),LevelDrawing::preset(QStringLiteral("家具"),{8,2})};
        document.objects[0].name=QStringLiteral("花园 <北侧>"); QString error;
        const auto png=directory.filePath("关卡.png"),svg=directory.filePath("关卡.svg");
        QVERIFY2(LevelDrawing::exportFile(document,png,false,true,true,1000,error),qPrintable(error));
        const QImage image(png); QVERIFY(!image.isNull()); QCOMPARE(image.width(),1000);
        bool colored=false;
        for (int y=0;y<image.height() && !colored;y+=5) for (int x=0;x<image.width();x+=5) if (image.pixelColor(x,y)!=QColor(Qt::white)) { colored=true; break; }
        QVERIFY(colored);
        QVERIFY2(LevelDrawing::exportFile(document,svg,true,true,true,1000,error),qPrintable(error));
        QFile file(svg); QVERIFY(file.open(QIODevice::ReadOnly)); QXmlStreamReader xml(file.readAll());
        int objects=0; bool vegetation=false,metadata=false;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name()==u"g" && xml.attributes().hasAttribute("data-preset")) {
                ++objects; if (xml.attributes().value("data-name")==QStringLiteral("绿化")) vegetation=true;
            }
            if (xml.isStartElement() && xml.name()==u"metadata") {
                const auto data=QJsonDocument::fromJson(xml.readElementText().toUtf8());
                QCOMPARE(data.object(),document.json()); metadata=true;
            }
        }
        QVERIFY(!xml.hasError()); QCOMPARE(objects,3); QVERIFY(vegetation); QVERIFY(metadata);
        QVERIFY(QDir(directory.path()).mkdir("level-layouts"));
        QVERIFY(document.write(directory.filePath("level-layouts/a.level.json"),error));
        const auto catalog=LevelDocument::catalog(directory.path()); QCOMPARE(catalog.size(),1);
        QCOMPARE(catalog.first().toMap()["title"].toString(),document.title);
    }
    // 同一工程不能在两个窗口中同时编辑，并输出实际控件布局供视觉检查。
    void windowAndLock()
    {
        QTemporaryDir directory; LevelDocument document; document.title=QStringLiteral("海滨新城 · 广场布局");
        document.objects={LevelDrawing::preset(QStringLiteral("绿化"),{2,2}),LevelDrawing::preset(QStringLiteral("道路"),{3,10}),LevelDrawing::preset(QStringLiteral("家具"),{10,4}),LevelDrawing::preset(QStringLiteral("建筑"),{15,-4}),LevelDrawing::preset(QStringLiteral("水域"),{-8,-4})};
        QString error; const auto path=directory.filePath("广场.level.json"); QVERIFY(document.write(path,error));
        LevelWindow first(directory.path()),second(directory.path());
        first.setAttribute(Qt::WA_DeleteOnClose,false); second.setAttribute(Qt::WA_DeleteOnClose,false);
        QVERIFY2(first.openProject(path,error),qPrintable(error)); QVERIFY(!second.openProject(path,error)); QVERIFY(!error.isEmpty());
        first.show(); QTest::qWait(30); first.canvas()->fit(); first.canvas()->select(0);
        QTest::qWait(30);
        const auto preview=qEnvironmentVariable("LEVEL_LAYOUT_PREVIEW"); if (!preview.isEmpty()) QVERIFY(first.grab().save(preview));
        QVERIFY(first.close());
    }
    // 实际保存快捷键完成草稿并写入共享目录，取消关闭可保留未保存修改。
    void saveAndCloseProtection()
    {
        QTemporaryDir directory;
        LevelWindow window(directory.path()); window.setAttribute(Qt::WA_DeleteOnClose,false); window.show();
        window.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&window));
        auto* canvas=window.canvas(); canvas->setTool("points");
        QTest::mouseClick(canvas,Qt::LeftButton,Qt::NoModifier,{100,100});
        QVERIFY(canvas->hasDraft());
        QTest::keyClick(canvas,Qt::Key_S,Qt::ControlModifier);
        QTRY_VERIFY(!window.projectPath().isEmpty());
        QVERIFY(!canvas->hasDraft()); QCOMPARE(LevelDocument::catalog(directory.path()).size(),1);
        LevelDocument restored; QString error;
        QVERIFY(LevelDocument::read(window.projectPath(),restored,error)); QCOMPARE(restored.objects.size(),1);
        auto changed=canvas->document(); changed.title=QStringLiteral("尚未保存的名字"); canvas->apply(changed);
        QTimer::singleShot(0,&window,[&window] {
            if (auto* box=window.findChild<QMessageBox*>()) box->button(QMessageBox::Cancel)->click();
        });
        QVERIFY(!window.close()); QVERIFY(window.isVisible());
        // 无屏幕平台在模态框关闭后需先完成焦点恢复，再验证快捷键。
        QTest::qWait(10); window.activateWindow(); canvas->setFocus();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        QTest::keyClick(canvas,Qt::Key_S,Qt::ControlModifier);
        QVERIFY(LevelDocument::read(window.projectPath(),restored,error)); QCOMPARE(restored.title,changed.title);
        QVERIFY(window.close());
    }
};
QTEST_MAIN(LevelLayoutTest)
#include "LevelLayoutTest.moc"
