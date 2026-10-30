#pragma once

#include <QMainWindow>

class PaintCanvas;
class QAction;
class QLabel;
class QCheckBox;
class QComboBox;

// 独立画板窗口，提供文件操作、基本绘图工具与数位板按键门控。
class PaintWindow final : public QMainWindow
{
    Q_OBJECT
public:
    // 创建画板并从应用数据目录恢复数位板使用偏好。
    explicit PaintWindow(const QString& dataRoot, QWidget* parent = nullptr);
protected:
    // 关闭时检查未保存作品，取消或保存失败均保持窗口。
    void closeEvent(QCloseEvent* event) override;
private:
    // 新建指定像素大小的白色画布。
    void newDocument();
    // 选择并加载本地图像，限制像素数量防止耗尽内存。
    void openDocument();
    // 保存或另存作品，通过原子替换避免破坏已有文件。
    bool saveDocument(bool saveAs = false);
    // 询问是否保存当前修改。
    bool confirmDiscard();
    // 根据作品和输入状态刷新标题、撤销按钮及状态栏。
    void refresh();
    // 把数位板模式和控制键保存在应用数据目录。
    void savePreferences();
    // 原生位图画布。
    PaintCanvas* m_canvas;
    // 当前文件路径与独立画板设置文件。
    QString m_path, m_preferences;
    // 撤销、重做及前景色按钮。
    QAction *m_undo, *m_redo, *m_color;
    // 数位板开关、控制键选择及实时提示。
    QCheckBox* m_tablet;
    QComboBox* m_key;
    QLabel* m_hint;
    // 当前额外按键是否允许落笔。
    bool m_allowed = false;
};
