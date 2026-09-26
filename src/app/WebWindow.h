#pragma once

#include <QMainWindow>

class AppBridge;
class QCloseEvent;
class QSystemTrayIcon;
class QWebEngineView;

// 仅承载网页与系统托盘的原生窗口，业务界面由 HTML 负责。
class WebWindow final : public QMainWindow
{
    Q_OBJECT
public:
    // 创建页面及桥接；自动化测试禁用托盘与全局热键。
    explicit WebWindow(const QString& dataRoot, bool nativeIntegration = true, QWidget* parent = nullptr);
    // 提供网页视图以便执行无屏幕控制的集成测试。
    QWebEngineView* webView() const { return m_view; }
    // 恢复并激活窗口，供托盘、热键与第二实例共同使用。
    void activate();
    // 询问网页是否存在未保存内容，确认后退出。
    void requestQuit();
protected:
    // 有托盘时关闭按钮隐藏窗口，否则执行退出保护。
    void closeEvent(QCloseEvent* event) override;
private:
    // 创建系统托盘和动作。
    void createTray();
    // 承载应用 HTML 的唯一网页视图。
    QWebEngineView* m_view;
    // 暴露给受信任页面的桥接对象。
    AppBridge* m_bridge;
    // 系统托盘，测试模式为空。
    QSystemTrayIcon* m_tray = nullptr;
    // 防止重复触发关闭确认。
    bool m_quitPending = false;
    // 网页确认后允许真正关闭窗口。
    bool m_canQuit = false;
};
