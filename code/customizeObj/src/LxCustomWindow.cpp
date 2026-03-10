#include <QVBoxLayout>
#include <QGraphicsDropShadowEffect>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QPushButton>
#include "widgets/inc/LxLoginWidget.h"
#include "widgets/inc/LxNoteWindow.h"
#include "ui_LxCustomWindow.h"
#include "LxCustomWindow.h"
#include "systemTool/inc/LxGlobalHotkeyWin.h"
LxGlobalHotkeyWin* hotkey;

LxCustomWindow::LxCustomWindow(QWidget *parent)
    : QWidget(parent), m_pUi(new Ui::LxCustomWindow)
{
    m_pUi->setupUi(this);

    initWindowStyle();

    createTrayIcon();

    hotkey = new LxGlobalHotkeyWin(this);

    // 注册 F8 无修饰键
    hotkey->registerHotkey(0, VK_F8);

    connect(hotkey, &LxGlobalHotkeyWin::hotkeyPressed, this, [=]() {
        this->showNormal();
        this->raise();
        this->activateWindow();
        });

    // 设置标题图标
    this->setWindowIcon(QIcon(":/images/app_icon.ico"));
    // 设置窗口名称
    this->setWindowTitle("桌面小工具");
}

LxCustomWindow::~LxCustomWindow()
{
}



void LxCustomWindow::on_uButShowMax_clicked()
{
    // 切换窗口最大化和还原状态
    if (this->windowState() & Qt::WindowMaximized) {
        this->showNormal(); // 如果是最大化状态，则还原
    }
    else {
        this->showMaximized(); // 否则最大化窗口
    }
}
void LxCustomWindow::on_uButShowMin_clicked()
{
    // 最小化窗口
    this->showMinimized();
}

void LxCustomWindow::on_uButClose_clicked()
{
    this->hide();

    
}

void LxCustomWindow::on_toolButtonClicked(LxToolButType type)
{

    QWidget* currentWidget = nullptr;
    if(!initMainWindowData(m_pUi->uMainBodyWidget).contains(type))
    {
        // 如果没有对应的窗口类型，直接返回
        return; 
    }
    else
    {
        currentWidget = initMainWindowData(m_pUi->uMainBodyWidget)[type];
    }
    if(currentWidget == nullptr)
    {
        // 如果对应的窗口为空，直接返回
        return; 
    }
    // 为特定的窗口绑定信号
    static bool bindingSignal = false;
    if (!bindingSignal && type == LxToolButType::Note_Type)
    {
        LxNoteWindow* noteWindow = qobject_cast<LxNoteWindow*>(currentWidget);
        connect(this, &LxCustomWindow::windowSizeChanged, noteWindow, &LxNoteWindow::on_minWindowSizeChanged);
        bindingSignal = true;
    }


    // 清除当前主窗口的内容
    if (m_pCurrentWidget)
    {
        m_pCurrentWidget->hide();
    }
    // 更新当前窗口指针
    m_pCurrentWidget = currentWidget; 

    m_pCurrentWidget->resize(m_pUi->uMainBodyWidget->size());
    // 查找是否已添加到主窗口
    if (!m_pUi->uMainBodyLayout->indexOf(m_pCurrentWidget)) {
        // 如果没有添加，则添加到主窗口
        m_pUi->uMainBodyLayout->addWidget(m_pCurrentWidget);

    }
    // 显示当前窗口
    m_pCurrentWidget->show(); 
}

void LxCustomWindow::mousePressEvent(QMouseEvent* event)
{
    // 如果鼠标左键按下，记录拖动状态和位置
    // 将事件的坐标转换为控件的局部坐标系
    QPoint localPos = m_pUi->uCustomTitleidget->mapFromGlobal(event->globalPos());
    if (event->button() == Qt::LeftButton && m_pUi->uCustomTitleidget->geometry().contains(localPos)) {
        m_bIsDragging = true;
        m_dragPosition = event->globalPos() - frameGeometry().topLeft();
        event->accept();

    }
    else if (event->button() == Qt::LeftButton)
    {
        // 检测是否在调整大小区域
        resizeRegion = hitTest(event->pos());
        dragPos = event->globalPos();
        if (resizeRegion != ResizeRegion::None) {
            event->accept();
        }
    }
    else {
        m_bIsDragging = false;
    }
}

void LxCustomWindow::mouseMoveEvent(QMouseEvent* event)
{
    // 如果正在拖动窗口，更新窗口位置
    if (m_bIsDragging) {
        move(event->globalPos() - m_dragPosition);
        event->accept();
    }
    else if (resizeRegion != ResizeRegion::None && (event->buttons() & Qt::LeftButton))
    {
        QPoint delta = event->globalPos() - dragPos;
        QRect geom = geometry();

        if (resizeRegion & ResizeRegion::Left) {
            int dx = delta.x();
            geom.setLeft(geom.left() + dx);
        }
        if (resizeRegion & ResizeRegion::Right) {
            int dx = delta.x();
            geom.setRight(geom.right() + dx);
        }
        if (resizeRegion & ResizeRegion::Top) {
            int dy = delta.y();
            geom.setTop(geom.top() + dy);
        }
        if (resizeRegion & ResizeRegion::Bottom) {
            int dy = delta.y();
            geom.setBottom(geom.bottom() + dy);
        }

        setGeometry(geom);
        dragPos = event->globalPos();

        // 设置鼠标样式（可选）
        int region = hitTest(event->pos());
        Qt::CursorShape cursorShape = Qt::ArrowCursor;
        switch (region) {
            case ResizeRegion::Top:
            case ResizeRegion::Bottom:
                cursorShape = Qt::SizeVerCursor;
                break;
            case ResizeRegion::Left:
            case ResizeRegion::Right:
                cursorShape = Qt::SizeHorCursor;
                break;
            case ResizeRegion::TopLeft:
            case ResizeRegion::BottomRight:
                cursorShape = Qt::SizeFDiagCursor;
                break;
            case ResizeRegion::TopRight:
            case ResizeRegion::BottomLeft:
                cursorShape = Qt::SizeBDiagCursor;
                break;
        }
        setCursor(cursorShape);
    }
    else {
        Qt::CursorShape cursorShape = Qt::ArrowCursor;
        setCursor(cursorShape);
        QWidget::mouseMoveEvent(event); // 调用基类处理其他鼠标移动事件
    }
}

void LxCustomWindow::mouseReleaseEvent(QMouseEvent* event)
{
    // 鼠标释放时，停止拖动
    if (event->button() == Qt::LeftButton) {
        m_bIsDragging = false;
        event->accept();
    }
    else {

        QWidget::mouseReleaseEvent(event); // 调用基类处理其他鼠标释放事件
    }
    if (resizeRegion != ResizeRegion::None)
    {
        resizeRegion = ResizeRegion::None;
        emit windowSizeChanged(); // 发送窗口大小改变信号
    }
    // 调用基类的鼠标释放事件
    Qt::CursorShape cursorShape = Qt::ArrowCursor;
    setCursor(cursorShape);
    QWidget::mouseReleaseEvent(event);
}

void LxCustomWindow::resizeEvent(QResizeEvent* event)
{
    // 调整窗口大小时，更新布局
    QWidget::resizeEvent(event);
    if (m_pCurrentWidget) {
        m_pCurrentWidget->resize(m_pUi->uMainBodyWidget->size());
    }
}

void LxCustomWindow::closeEvent(QCloseEvent* event)
{
    // 缩小到托盘
    if (m_pTrayIcon->isVisible()) {
        this->hide();
        event->ignore(); // 不关闭窗口
        // m_pTrayIcon->showMessage("已最小化", "程序仍在后台运行", QSystemTrayIcon::Information, 3000);
    }
}

int LxCustomWindow::hitTest(const QPoint& pos)
{
    int region = ResizeRegion::None;
    QRect r = rect();

    if (pos.x() <= borderWidth)
        region |= ResizeRegion::Left;
    else if (pos.x() >= r.width() - borderWidth)
        region |= ResizeRegion::Right;

    if (pos.y() <= borderWidth)
        region |= ResizeRegion::Top;
    else if (pos.y() >= r.height() - borderWidth)
        region |= ResizeRegion::Bottom;

    return region;
}

void LxCustomWindow::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    // 绘制窗口背景
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true); // 开启抗锯齿
    
    // 参数定义
    const int radius = 5;
    const int shadowWidth = 3; // 阴影边框宽度
    const int margin = 10;     // 窗口内容距离边缘
    const int x = margin;
    const int y = margin;
    const int w = width() - 2 * margin;
    const int titleHeight = 50;
    const int bodyY = y + titleHeight;
    const int bodyHeight = height() - bodyY - margin;

    // === 第一步：绘制阴影边框 ===
    QPainterPath shadowPath;
    shadowPath.addRoundedRect(QRectF(x - shadowWidth, y - shadowWidth,
        w + 2 * shadowWidth, titleHeight + bodyHeight + 2 * shadowWidth),
        radius + shadowWidth, radius + shadowWidth);

    QColor shadowColor(0, 0, 0, 50); // 半透明黑色，可调整为灰色如 QColor(100, 100, 100, 80)
    painter.fillPath(shadowPath, shadowColor);


    // === 第二步：绘制标题栏（上方两个圆角） ===
    QPainterPath titlePath;
    titlePath.moveTo(x + radius, y);
    titlePath.lineTo(x + w - radius, y);
    titlePath.quadTo(x + w, y, x + w, y + radius);
    titlePath.lineTo(x + w, y + titleHeight);
    titlePath.lineTo(x, y + titleHeight);
    titlePath.lineTo(x, y + radius);
    titlePath.quadTo(x, y, x + radius, y);
    titlePath.closeSubpath();

    painter.setPen(Qt::NoPen);
    painter.fillPath(titlePath, QColor(255, 255, 255)); // 白色标题栏背景


    // === 第三步：绘制主体区域（下方两个圆角） ===
    QPainterPath bodyPath;
    bodyPath.moveTo(x, bodyY);
    bodyPath.lineTo(x + w, bodyY);
    bodyPath.lineTo(x + w, bodyY + bodyHeight - radius);
    bodyPath.quadTo(x + w, bodyY + bodyHeight, x + w - radius, bodyY + bodyHeight);
    bodyPath.lineTo(x + radius, bodyY + bodyHeight);
    bodyPath.quadTo(x, bodyY + bodyHeight, x, bodyY + bodyHeight - radius);
    bodyPath.lineTo(x, bodyY);
    bodyPath.closeSubpath();

    painter.fillPath(bodyPath, QColor(219, 229, 238)); // 主体区域背景

    // 绘制完成
    QWidget::paintEvent(event); // 调用基类的绘制事件

}

void LxCustomWindow::initWindowStyle()
{
    // 设置窗口属性
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint);
    setAttribute(Qt::WA_TranslucentBackground, true);

    m_pUi->uButShowMax->hide();

    // 设置左侧菜单列表滚动条样式

    m_pUi->uMenuButList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);  // 永远不显示横向滚动条
    m_pUi->uMenuButList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);     // 垂直滚动条需要时显示
    m_pUi->uMenuButList->setStyleSheet(R"(
        QScrollBar:vertical {
            background: transparent;       /* 背景透明 */
            width: 8px;                    /* 滚动条宽度 */
            margin: 0px;
        }
        
        QScrollBar::handle:vertical {
            background: #ccc;              /* 浅灰色滑块 */
            min-height: 20px;
            border-radius: 2px;           /* 圆角 2px */
        }
        
        QScrollBar::add-line:vertical,
        QScrollBar::sub-line:vertical,
        QScrollBar::add-page:vertical,
        QScrollBar::sub-page:vertical {
            background: none;             /* 不显示上下箭头及轨道 */
            height: 0px;
        }
        )");
    // 添加左侧菜单栏按钮
    initLeftMenuBar();
}
void LxCustomWindow::initLeftMenuBar()
{


    //遍历创建
    QPushButton* button = nullptr;
    for (auto& key : LxToolButTypeName.keys())
    {
        auto butName = LxToolButTypeName[key].first;
        auto butIcon = LxToolButTypeName[key].second;
        // 创建按钮
        button = new QPushButton(butName, this);
        button->setMinimumSize(145, 30);
        button->setMaximumSize(145, 30);
        button->setProperty("butType", key); // 设置按钮类型属性
        button->setObjectName(QString::fromUtf8("uButTool"));
        button->setIcon(QIcon(butIcon));
        button->setCheckable(true);
        // 添加到左侧菜单栏
        m_pUi->uMenuButListLayout->addWidget(button);

        // 添加槽函数
        connect(button, &QPushButton::clicked, this, [this, key, button]() {
            on_toolButtonClicked(key);
            // 设置按钮选中状态
            for (auto& btn : m_menuButtons) {
                if (btn != button) {
                    btn->setChecked(false); // 取消其他按钮选中状态
                }
            }
        });
        m_menuButtons.append(button); // 保存按钮到列表中
    }
    // 在布局中添加一个弹簧，保持布局的完整性
    m_pUi->uMenuButListLayout->addSpacerItem(new QSpacerItem(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding));
}

void LxCustomWindow::createTrayIcon()
{
    // 创建托盘菜单动作
    m_pShowAction = new QAction("打开软件", this);
    m_pExitAction = new QAction("退出软件", this);
    connect(m_pShowAction, &QAction::triggered, this, [=]() {
        this->showNormal();  // 或 this->show()
        this->raise();       // 确保在前
        this->activateWindow();
        });

    connect(m_pExitAction, &QAction::triggered, qApp, &QApplication::quit);

    // 创建菜单
    m_pTrayMenu = new QMenu(this);
    m_pTrayMenu->addAction(m_pShowAction);
    m_pTrayMenu->addSeparator();
    m_pTrayMenu->addAction(m_pExitAction);

    // 创建托盘图标
    m_pTrayIcon = new QSystemTrayIcon(this);
    m_pTrayIcon->setIcon(QIcon(":/images/app_icon.png")); // 设置图标（需存在）
    m_pTrayIcon->setContextMenu(m_pTrayMenu);
    m_pTrayIcon->show();

    // 双击托盘图标也可显示主界面
    connect(m_pTrayIcon, &QSystemTrayIcon::activated, this, [=](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::DoubleClick) {
            this->showNormal();
            this->raise();
            this->activateWindow();
        }
        });
}
