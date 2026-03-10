#pragma once

#include <QWidget>
#include <QPoint>
#include <QVector>
#include "LxMainWindowData.h"

namespace Ui {
    class LxCustomWindow;
}
class QMouseEvent;
class QPaintEvent;
class QPushButton;
class QSystemTrayIcon;
class QMenu;
class QAction;

/// <summary>
/// 所有窗口的基本样式，自定义了窗口的外观和行为
/// </summary>
class LxCustomWindow : public QWidget
{
    Q_OBJECT

public:
    LxCustomWindow(QWidget *parent = nullptr);
    ~LxCustomWindow();

signals:
    // 窗口停止缩放信号
    void windowSizeChanged();

private slots:
    // 最小化窗口
    void on_uButShowMin_clicked();
    // 最大化窗口
    void on_uButShowMax_clicked();
    // 关闭窗口
    void on_uButClose_clicked();

    // 左侧菜单栏按钮点击事件
    void on_toolButtonClicked(LxToolButType type);

private:
    /*******************************窗口相关行为*********************************/
    // 鼠标按下事件
    void mousePressEvent(QMouseEvent* event) override;
    // 鼠标移动事件
    void mouseMoveEvent(QMouseEvent* event) override;
    // 鼠标松开事件
    void mouseReleaseEvent(QMouseEvent* event) override;
    // 窗口大小改变
    void resizeEvent(QResizeEvent* event) override;
    // 关闭事件
    void closeEvent(QCloseEvent* event) override;
    // 是否在拖动窗口
    bool m_bIsDragging = false;
    // 鼠标与窗口的偏移量
    QPoint m_dragPosition;
    // 窗口缩放相关变量
    int hitTest(const QPoint& pos);

    QPoint dragPos;
    int resizeRegion = ResizeRegion::None;
    // 触发调整大小的边缘宽度
    const int borderWidth = 12;  
    /*******************************窗口相关行为*********************************/
    /*******************************窗口布局/样式*********************************/

    // 绘制事件
    void paintEvent(QPaintEvent* event) override;

    // 初始化窗口样式
    void initWindowStyle();
    // 初始化左侧菜单栏
    void initLeftMenuBar();

    // 左侧按钮列表
    QVector<QPushButton*> m_menuButtons;
    /*******************************窗口布局/样式*********************************/

    // UI界面
    Ui::LxCustomWindow* m_pUi;

    /****************************主窗口嵌套窗口*************************************/
    QWidget* m_pCurrentWidget = nullptr; // 登录窗口

    // 托盘菜单
    QSystemTrayIcon* m_pTrayIcon = nullptr;
    QMenu* m_pTrayMenu = nullptr;
    QAction* m_pShowAction = nullptr;
    QAction* m_pExitAction = nullptr;

    void createTrayIcon();
};
