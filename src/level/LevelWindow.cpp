#include "LevelWindow.h"
#include "LevelCanvas.h"
#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLockFile>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QUuid>
#include <QScrollArea>
#include <QSpinBox>
#include <algorithm>

namespace {
// 创建两位小数的米制尺寸或角度输入框。
QDoubleSpinBox* decimal(QWidget* parent, double low, double high, const QString& suffix)
{
    auto* input = new QDoubleSpinBox(parent); input->setRange(low, high); input->setDecimals(2);
    input->setSuffix(suffix); input->setKeyboardTracking(false); input->setSingleStep(.10); return input;
}
}

LevelWindow::LevelWindow(const QString& dataRoot, QWidget* parent)
    : QMainWindow(parent), m_dataRoot(QDir(dataRoot).absolutePath()), m_canvas(new LevelCanvas(this))
{
    setWindowFlag(Qt::Window); setAttribute(Qt::WA_DeleteOnClose); resize(1380, 880);
    setMinimumSize(1000, 640); setObjectName("levelWindow");
    setStyleSheet("QToolBar {spacing: 6px; padding: 6px;} QToolButton {padding: 7px;} "
                  "QToolButton:checked {background: #d4e9e4; border-radius: 4px;} "
                  "QLineEdit, QDoubleSpinBox, QComboBox {min-height: 26px;} QListWidget {border: 1px solid #dce2df;}");
    auto* splitter = new QSplitter(this); splitter->addWidget(m_canvas);
    auto* scroll = new QScrollArea(splitter); scroll->setWidgetResizable(true); scroll->setWidget(buildInspector());
    scroll->setMinimumWidth(260); scroll->setMaximumWidth(380); splitter->addWidget(scroll);
    splitter->setStretchFactor(0,1); splitter->setSizes({1000,290}); setCentralWidget(splitter);
    buildTools();
    m_saved = m_canvas->document().json();
    connect(m_canvas,&LevelCanvas::changed,this,[this] { refreshTitle(); refresh(); });
    connect(m_canvas,&LevelCanvas::selectionChanged,this,&LevelWindow::refresh);
    connect(m_canvas,&LevelCanvas::message,this,[this](const QString& text) { statusBar()->showMessage(text); });
    statusBar()->showMessage(QStringLiteral("左侧选择工具开始绘制；中键平移，滚轮缩放；所有几何尺寸以米保存。"));
    refresh(); refreshTitle();
}

LevelWindow::~LevelWindow() = default;

void LevelWindow::buildTools()
{
    auto* file = addToolBar(QStringLiteral("项目功能")); file->setMovable(false);
    file->addAction(QStringLiteral("新建项目"),QKeySequence::New,this,&LevelWindow::newProject);
    file->addAction(QStringLiteral("打开项目"),QKeySequence::Open,this,&LevelWindow::chooseProject);
    file->addAction(QStringLiteral("保存"),QKeySequence::Save,this,[this] { save(); });
    file->addAction(QStringLiteral("另存为"),QKeySequence::SaveAs,this,[this] { save(true); });
    file->addAction(QStringLiteral("项目名称"),this,[this] {
        bool ok; const auto name = QInputDialog::getText(this,QStringLiteral("项目名称"),QStringLiteral("名称"),QLineEdit::Normal,m_canvas->document().title,&ok).trimmed();
        if (ok && !name.isEmpty()) { auto document = m_canvas->document(); document.title = name; m_canvas->apply(document); }
    });
    file->addSeparator();
    file->addAction(QStringLiteral("导出 PNG"),this,[this] { exportProject(false); });
    file->addAction(QStringLiteral("导出 SVG"),this,[this] { exportProject(true); });
    file->addSeparator();
    file->addAction(QStringLiteral("撤销"),QKeySequence::Undo,m_canvas,&LevelCanvas::undo);
    file->addAction(QStringLiteral("重做"),QKeySequence::Redo,m_canvas,&LevelCanvas::redo);
    file->addAction(QStringLiteral("查看全部"),m_canvas,&LevelCanvas::fit);
    addToolBarBreak();
    auto* aids = addToolBar(QStringLiteral("绘图辅助")); aids->setMovable(false);
    // 每个辅助开关作为独立操作提交，撤销时可恢复设置。
    const auto toggle = [this,aids](const QString& text, bool LevelDocument::*field) {
        auto* action = aids->addAction(text); action->setCheckable(true);
        connect(action,&QAction::toggled,this,[this,field](bool value) {
            if (m_refreshing) return; auto document = m_canvas->document(); document.*field = value; m_canvas->apply(document);
        }); return action;
    };
    m_angles = toggle(QStringLiteral("常用角吸附"),&LevelDocument::angles);
    m_angles->setToolTip(QStringLiteral("夹角靠近 0、30、45、60、90、120、135、150、180 度时吸附；容差 4 度。"));
    m_quantize = toggle(QStringLiteral("最小角倍数"),&LevelDocument::quantize);
    m_quantize->setToolTip(QStringLiteral("强制前一点两侧的夹角为设置角度的倍数，优先于常用角吸附。首段以水平轴为基准。"));
    m_angleStep = decimal(aids,.01,180,QStringLiteral(" °")); m_angleStep->setFixedWidth(100); aids->addWidget(m_angleStep);
    m_ruler = toggle(QStringLiteral("长度标尺"),&LevelDocument::ruler);
    m_unit = new QComboBox(aids); m_unit->addItem(QStringLiteral("米"),"m"); m_unit->addItem(QStringLiteral("厘米"),"cm"); m_unit->addItem(QStringLiteral("千米"),"km"); aids->addWidget(m_unit);
    m_crosshair = toggle(QStringLiteral("十字对齐线"),&LevelDocument::crosshair);
    m_grid = toggle(QStringLiteral("网格"),&LevelDocument::grid);
    m_gridStep = decimal(aids,.01,10000,QStringLiteral(" 米/格")); m_gridStep->setFixedWidth(130); aids->addWidget(m_gridStep);
    connect(m_angleStep,&QDoubleSpinBox::valueChanged,this,[this](double value) { if (!m_refreshing) { auto d=m_canvas->document(); d.angleStep=value; m_canvas->apply(d); } });
    connect(m_gridStep,&QDoubleSpinBox::valueChanged,this,[this](double value) { if (!m_refreshing) { auto d=m_canvas->document(); d.gridStep=value; m_canvas->apply(d); } });
    connect(m_unit,&QComboBox::currentIndexChanged,this,[this] { if (!m_refreshing) { auto d=m_canvas->document(); d.unit=m_unit->currentData().toString(); m_canvas->apply(d); } });
    auto* drawing = new QToolBar(QStringLiteral("绘图工具"),this); drawing->setMovable(false);
    drawing->setToolButtonStyle(Qt::ToolButtonTextOnly); addToolBar(Qt::LeftToolBarArea,drawing);
    auto* group = new QActionGroup(this); group->setExclusive(true);
    const QList<QPair<QString,QString>> tools{{QStringLiteral("选择 / 编辑"),"select"},{QStringLiteral("点集"),"points"},
        {QStringLiteral("连续折线"),"polyline"},{QStringLiteral("多边形"),"polygon"},
        {QStringLiteral("绿化"),QStringLiteral("preset:绿化")},{QStringLiteral("道路"),QStringLiteral("preset:道路")},
        {QStringLiteral("家具"),QStringLiteral("preset:家具")},{QStringLiteral("水域"),QStringLiteral("preset:水域")},
        {QStringLiteral("建筑"),QStringLiteral("preset:建筑")}};
    for (const auto& item : tools) {
        auto* action = drawing->addAction(item.first); action->setCheckable(true); action->setData(item.second); group->addAction(action);
        if (item.second=="select") action->setChecked(true);
        if (item.second.startsWith("preset:"))
            action->setToolTip(item.second == QStringLiteral("preset:道路")
                ? QStringLiteral("逐点绘制整条道路；线粗细即道路宽度，Enter 或右键完成。")
                : QStringLiteral("逐点勾勒多边形边界，Enter 或右键闭合；自动应用预设颜色与纹样。"));
        connect(action,&QAction::triggered,this,[this,action,group] {
            // 不足最小节点数的草稿阻止切换，并恢复原先已选工具。
            if (!finishDraft()) {
                for (auto* candidate : group->actions()) candidate->setChecked(candidate->property("lastTool").toBool());
                return;
            }
            for (auto* candidate : group->actions()) candidate->setProperty("lastTool",candidate==action);
            m_canvas->setTool(action->data().toString());
        });
        action->setProperty("lastTool",item.second=="select");
    }
    drawing->addSeparator();
    drawing->addAction(QStringLiteral("完成图形"),this,[this] { finishDraft(); });
    drawing->addAction(QStringLiteral("取消绘制"),m_canvas,&LevelCanvas::cancelDraft);
    drawing->addSeparator();
    drawing->addAction(QStringLiteral("复制对象"),QKeySequence("Ctrl+D"),m_canvas,&LevelCanvas::duplicateSelected);
    drawing->addAction(QStringLiteral("删除对象"),m_canvas,&LevelCanvas::removeSelected);
}

QWidget* LevelWindow::buildInspector()
{
    auto* panel = new QWidget(this); auto* layout = new QVBoxLayout(panel);
    auto* heading = new QLabel(QStringLiteral("矢量信息"),panel); heading->setStyleSheet("font-size: 18px; font-weight: 600;"); layout->addWidget(heading);
    m_info = new QLabel(panel); m_info->setWordWrap(true); layout->addWidget(m_info);
    auto* form = new QFormLayout;
    m_name = new QLineEdit(panel); form->addRow(QStringLiteral("名称"),m_name);
    m_preset = new QComboBox(panel); m_preset->addItem(QStringLiteral("普通图形"),"");
    for (const auto& key : {QStringLiteral("绿化"),QStringLiteral("道路"),QStringLiteral("家具"),QStringLiteral("水域"),QStringLiteral("建筑")}) m_preset->addItem(key,key);
    form->addRow(QStringLiteral("预设语义"),m_preset);
    m_fill = new QPushButton(QStringLiteral("填充颜色"),panel); m_stroke = new QPushButton(QStringLiteral("轮廓颜色"),panel);
    form->addRow(m_fill,m_stroke);
    m_width = decimal(panel,.01,1000,QStringLiteral(" 米")); m_diameter = decimal(panel,.01,1000,QStringLiteral(" 米"));
    form->addRow(QStringLiteral("线粗细"),m_width); form->addRow(QStringLiteral("点直径"),m_diameter);
    m_nodes = new QComboBox(panel); form->addRow(QStringLiteral("编辑节点"),m_nodes);
    m_x = decimal(panel,-1e7,1e7,QStringLiteral(" 米")); m_y = decimal(panel,-1e7,1e7,QStringLiteral(" 米"));
    form->addRow(QStringLiteral("横坐标 X"),m_x); form->addRow(QStringLiteral("纵坐标 Y"),m_y);
    layout->addLayout(form);
    auto* help = new QLabel(QStringLiteral("选中对象后可拖动红色节点。双击线段插入节点；点选节点后按 Delete 删除该节点。预设同样逐点绘制：绿化勾勒多边形，道路连续连线；Enter 或右键完成。"),panel);
    help->setWordWrap(true); help->setStyleSheet("color: #657579; padding: 6px 0;"); layout->addWidget(help);
    layout->addWidget(new QLabel(QStringLiteral("对象列表（后绘制的对象位于上层）"),panel));
    m_objects = new QListWidget(panel); m_objects->setMinimumHeight(140); layout->addWidget(m_objects,1);
    connect(m_objects,&QListWidget::currentRowChanged,this,[this](int row) {
        if (m_refreshing) return;
        if (!finishDraft()) { refresh(); return; }
        // 从列表选对象时同步切回选择工具，允许立即拖动节点。
        for (auto* action : findChildren<QAction*>()) {
            if (action->data().toString() == "select") { action->trigger(); break; }
        }
        m_canvas->select(row); m_canvas->setFocus();
    });
    connect(m_name,&QLineEdit::editingFinished,this,&LevelWindow::applyInspector);
    connect(m_preset,&QComboBox::currentIndexChanged,this,&LevelWindow::applyInspector);
    connect(m_width,&QDoubleSpinBox::valueChanged,this,&LevelWindow::applyInspector);
    connect(m_diameter,&QDoubleSpinBox::valueChanged,this,&LevelWindow::applyInspector);
    connect(m_fill,&QPushButton::clicked,this,[this] { chooseColor(true); });
    connect(m_stroke,&QPushButton::clicked,this,[this] { chooseColor(false); });
    connect(m_nodes,&QComboBox::currentIndexChanged,this,[this](int index) {
        if (m_refreshing || m_canvas->selection()<0 || index<0) return;
        const auto& points=m_canvas->document().objects[m_canvas->selection()].points;
        if (index>=points.size()) return;
        m_refreshing=true; m_x->setValue(points[index].x()); m_y->setValue(points[index].y()); m_refreshing=false;
    });
    // 数字输入只改变当前节点，保持其他节点和对象样式。
    const auto updateNode = [this] {
        if (m_refreshing || m_canvas->selection()<0 || m_nodes->currentIndex()<0) return;
        auto object=m_canvas->document().objects[m_canvas->selection()];
        object.points[m_nodes->currentIndex()]=QPointF(m_x->value(),m_y->value()); m_canvas->replaceSelected(object);
    };
    connect(m_x,&QDoubleSpinBox::valueChanged,this,updateNode); connect(m_y,&QDoubleSpinBox::valueChanged,this,updateNode);
    return panel;
}

void LevelWindow::refresh()
{
    if (m_refreshing) return;
    m_refreshing=true;
    const auto& document=m_canvas->document(); const int selected=m_canvas->selection();
    const int node=m_nodes->currentIndex();
    // 列表只在内容变化时重建，避免绘图每次添加节点都滚回顶部。
    bool rebuild=m_objects->count()!=document.objects.size();
    if (!rebuild) for (int i=0;i<m_objects->count();++i) if (m_objects->item(i)->text()!=document.objects[i].name) { rebuild=true; break; }
    if (rebuild) { m_objects->clear(); for (const auto& object:document.objects) m_objects->addItem(object.name); }
    m_objects->setCurrentRow(selected);
    const auto object=selected>=0 ? document.objects[selected] : m_canvas->drawingStyle();
    m_name->setText(selected>=0 ? object.name : QString()); m_name->setEnabled(selected>=0);
    m_preset->setCurrentIndex(std::max(0,m_preset->findData(object.preset))); m_preset->setEnabled(selected>=0);
    m_width->setValue(object.width); m_diameter->setValue(object.diameter);
    m_fill->setStyleSheet("background:"+object.fill.name()+";color:"+(object.fill.lightness()>130?"#172722":"white")+";");
    m_stroke->setStyleSheet("background:"+object.stroke.name()+";color:"+(object.stroke.lightness()>130?"#172722":"white")+";");
    m_info->setText(selected<0 ? (object.preset.isEmpty() ? QStringLiteral("未选择对象 · 下方颜色与粗细用于新图形")
        : QStringLiteral("正在绘制：%1 · %2\n下方颜色与粗细用于当前绘图").arg(object.preset,
            object.kind == "polyline" ? QStringLiteral("连续折线") : QStringLiteral("多边形"))) :
        QStringLiteral("%1 · %2 个节点").arg(object.kind=="polygon"?QStringLiteral("面矢量"):object.kind=="polyline"?QStringLiteral("线矢量"):QStringLiteral("点矢量")).arg(object.points.size()));
    m_nodes->clear();
    if (selected>=0) for (qsizetype i=0;i<object.points.size();++i) m_nodes->addItem(QStringLiteral("节点 %1").arg(i+1));
    if (selected>=0) {
        const int current=std::clamp(node,0,int(object.points.size())-1); m_nodes->setCurrentIndex(current);
        m_x->setValue(object.points[current].x()); m_y->setValue(object.points[current].y());
    }
    m_x->setEnabled(selected>=0); m_y->setEnabled(selected>=0); m_nodes->setEnabled(selected>=0);
    if (m_grid) {
        m_grid->setChecked(document.grid); m_angles->setChecked(document.angles); m_quantize->setChecked(document.quantize);
        m_ruler->setChecked(document.ruler); m_crosshair->setChecked(document.crosshair); m_angleStep->setValue(document.angleStep);
        m_gridStep->setValue(document.gridStep); m_unit->setCurrentIndex(m_unit->findData(document.unit));
    }
    m_refreshing=false;
}

void LevelWindow::applyInspector()
{
    if (m_refreshing) return;
    const int selected=m_canvas->selection(); auto object=selected>=0?m_canvas->document().objects[selected]:m_canvas->drawingStyle();
    object.width=m_width->value(); object.diameter=m_diameter->value();
    if (selected>=0) { object.name=m_name->text().trimmed(); object.preset=m_preset->currentData().toString(); m_canvas->replaceSelected(object); }
    else m_canvas->setDrawingStyle(object);
}

void LevelWindow::chooseColor(bool fill)
{
    const int selected=m_canvas->selection(); auto object=selected>=0?m_canvas->document().objects[selected]:m_canvas->drawingStyle();
    const auto color=QColorDialog::getColor(fill?object.fill:object.stroke,this,fill?QStringLiteral("填充颜色"):QStringLiteral("轮廓颜色"),QColorDialog::ShowAlphaChannel);
    if (!color.isValid()) return;
    if (fill) object.fill=color; else object.stroke=color;
    if (selected>=0) m_canvas->replaceSelected(object); else { m_canvas->setDrawingStyle(object); refresh(); }
}

bool LevelWindow::modified() const { return m_canvas->hasDraft() || m_canvas->document().json()!=m_saved; }
void LevelWindow::refreshTitle() { setWindowTitle(m_canvas->document().title+(modified()?" *":"")+QStringLiteral(" — 关卡布局")); }

bool LevelWindow::finishDraft()
{
    if (m_canvas->finish()) return true;
    QMessageBox::information(this,QStringLiteral("绘图未完成"),QStringLiteral("折线至少需要两个节点，多边形至少需要三个节点。请继续绘制，或点击“取消绘制”。")); return false;
}

bool LevelWindow::canLeave()
{
    if (!modified()) return true;
    const auto choice=QMessageBox::question(this,QStringLiteral("保存关卡项目"),QStringLiteral("当前项目有未保存的修改，是否保存？"),QMessageBox::Save|QMessageBox::Discard|QMessageBox::Cancel,QMessageBox::Save);
    return choice==QMessageBox::Discard || (choice==QMessageBox::Save && save());
}

bool LevelWindow::openProject(const QString& path, QString& error)
{
    const QString absolute=QFileInfo(path).absoluteFilePath();
    if (absolute==m_path) return true;
    LevelDocument document;
    if (!LevelDocument::read(absolute,document,error)) return false;
    auto lock=std::make_unique<QLockFile>(absolute+".lock"); lock->setStaleLockTime(0);
    if (!lock->tryLock()) { error=QStringLiteral("此项目已在另一个编辑窗口中打开，请先关闭该窗口。"); return false; }
    if (!canLeave()) { error.clear(); return false; }
    m_lock=std::move(lock); m_path=absolute; m_saved=document.json(); m_canvas->load(document); refreshTitle(); return true;
}

bool LevelWindow::save(bool saveAs)
{
    if (!finishDraft()) return false;
    QString target=m_path;
    if (saveAs) {
        target=QFileDialog::getSaveFileName(this,QStringLiteral("另存关卡项目"),m_path.isEmpty()?QDir(m_dataRoot).filePath("level-layouts/关卡.level.json"):m_path,QStringLiteral("关卡项目 (*.level.json)"));
        if (target.isEmpty()) return false;
        if (!target.endsWith(".level.json",Qt::CaseInsensitive)) target+=".level.json";
    } else if (target.isEmpty()) {
        const QString directory=QDir(m_dataRoot).filePath("level-layouts");
        if (!QDir().mkpath(directory)) { QMessageBox::warning(this,QStringLiteral("保存失败"),QStringLiteral("无法创建项目目录。")); return false; }
        target=QDir(directory).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces)+".level.json");
    }
    target=QFileInfo(target).absoluteFilePath();
    std::unique_ptr<QLockFile> lock;
    if (target!=m_path) {
        lock=std::make_unique<QLockFile>(target+".lock"); lock->setStaleLockTime(0);
        if (!lock->tryLock()) { QMessageBox::warning(this,QStringLiteral("保存失败"),QStringLiteral("目标项目正在另一个窗口编辑，无法覆盖。")); return false; }
    }
    QString error;
    if (!m_canvas->document().write(target,error)) { QMessageBox::warning(this,QStringLiteral("保存失败"),error); return false; }
    if (lock) m_lock=std::move(lock);
    m_path=target; m_saved=m_canvas->document().json(); refreshTitle();
    statusBar()->showMessage(QStringLiteral("已保存：")+target); return true;
}

void LevelWindow::newProject()
{
    if (!canLeave()) return;
    bool ok; const auto title=QInputDialog::getText(this,QStringLiteral("新建关卡项目"),QStringLiteral("项目名称"),QLineEdit::Normal,QStringLiteral("新关卡"),&ok).trimmed();
    if (!ok || title.isEmpty()) return;
    LevelDocument document; document.title=title; m_path.clear(); m_lock.reset(); m_saved={}; m_canvas->load(document);
}

void LevelWindow::chooseProject()
{
    const auto path=QFileDialog::getOpenFileName(this,QStringLiteral("打开关卡项目"),QDir(m_dataRoot).filePath("level-layouts"),QStringLiteral("关卡项目 (*.level.json)"));
    if (path.isEmpty()) return;
    QString error; if (!openProject(path,error) && !error.isEmpty()) QMessageBox::warning(this,QStringLiteral("打开失败"),error);
}

void LevelWindow::exportProject(bool svg)
{
    if (!finishDraft()) return;
    QDialog dialog(this); dialog.setWindowTitle(svg?QStringLiteral("导出 SVG 矢量图"):QStringLiteral("导出 PNG 图片"));
    auto* layout=new QFormLayout(&dialog);
    auto* names=new QCheckBox(QStringLiteral("使用预设语义作为 SVG 对象名称"),&dialog); names->setChecked(true); names->setEnabled(svg); layout->addRow(names);
    auto* legend=new QCheckBox(QStringLiteral("附加预设图例"),&dialog); legend->setChecked(true); layout->addRow(legend);
    auto* width=new QSpinBox(&dialog); width->setRange(256,8192); width->setValue(2400); width->setSuffix(QStringLiteral(" 像素")); layout->addRow(QStringLiteral("输出宽度"),width);
    auto* note=new QLabel(QStringLiteral("导出全部对象的完整范围；网格、节点和辅助线不参与导出。SVG 保留对象名称、预设语义和米制项目元数据。"),&dialog); note->setWordWrap(true); layout->addRow(note);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog); layout->addRow(buttons);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept); connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if (dialog.exec()!=QDialog::Accepted) return;
    QString path=QFileDialog::getSaveFileName(this,QStringLiteral("导出关卡布局"),QStringLiteral("关卡布局")+(svg?".svg":".png"),svg?QStringLiteral("SVG 矢量图 (*.svg)"):QStringLiteral("PNG 图片 (*.png)"));
    if (path.isEmpty()) return;
    const QString extension=svg?".svg":".png"; if (!path.endsWith(extension,Qt::CaseInsensitive)) path+=extension;
    QString error;
    if (!LevelDrawing::exportFile(m_canvas->document(),path,svg,names->isChecked(),legend->isChecked(),width->value(),error)) QMessageBox::warning(this,QStringLiteral("导出失败"),error);
    else statusBar()->showMessage(QStringLiteral("已导出：")+path);
}

void LevelWindow::closeEvent(QCloseEvent* event)
{
    if (canLeave()) event->accept(); else event->ignore();
}
