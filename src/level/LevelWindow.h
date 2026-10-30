#pragma once
#include "LevelDocument.h"
#include <QMainWindow>
#include <memory>

class LevelCanvas;
class QLockFile;
class QListWidget;
class QLineEdit;
class QDoubleSpinBox;
class QPushButton;
class QComboBox;
class QLabel;
class QAction;

// 独立关卡矢量编辑窗口，可由主程序或轻量独立进程承载。
class LevelWindow final : public QMainWindow {
    Q_OBJECT
public:
    // 初始化工具栏、画布和属性面板。
    explicit LevelWindow(const QString& dataRoot, QWidget* parent = nullptr);
    // 在锁对象完整定义处释放工程锁。
    ~LevelWindow() override;
    // 打开文件并获取跨进程编辑锁，错误时保留现有项目。
    bool openProject(const QString& path, QString& error);
    // 返回当前项目文件的绝对路径。
    QString projectPath() const { return m_path; }
    // 提供画布用于必要的输入和持久化验证。
    LevelCanvas* canvas() const { return m_canvas; }
protected:
    // 关闭前处理未保存文档与绘图草稿。
    void closeEvent(QCloseEvent* event) override;
private:
    // 创建左侧绘图工具及上方功能栏。
    void buildTools();
    // 创建右侧对象属性及列表。
    QWidget* buildInspector();
    // 同步列表及当前对象属性，防止反馈触发编辑。
    void refresh();
    // 更新标题及未保存标记。
    void refreshTitle();
    // 将右侧可编辑属性写回对象或默认绘图样式。
    void applyInspector();
    // 选择填充或轮廓颜色。
    void chooseColor(bool fill);
    // 判断当前项目是否存在未保存编辑。
    bool modified() const;
    // 请求保存、放弃或取消，返回是否允许离开当前文档。
    bool canLeave();
    // 保存工程，首次保存自动加入主程序项目目录。
    bool save(bool saveAs = false);
    // 完成或阻止未结束的绘图，供保存和导出共用。
    bool finishDraft();
    // 新建项目并询问名称。
    void newProject();
    // 用文件对话框打开项目。
    void chooseProject();
    // 显示导出选项并生成 PNG 或 SVG。
    void exportProject(bool svg);
    QString m_dataRoot; // 与主程序共享的数据目录。
    QString m_path; // 当前工程文件路径。
    QJsonObject m_saved; // 上次成功保存的快照。
    std::unique_ptr<QLockFile> m_lock; // 跨进程独占编辑锁。
    LevelCanvas* m_canvas = nullptr; // 中央视口。
    QListWidget* m_objects = nullptr; // 对象选择列表。
    QLineEdit* m_name = nullptr; // 对象名称编辑框。
    QComboBox* m_preset = nullptr; // 对象预设语义。
    QPushButton* m_fill = nullptr; // 填充颜色选择器。
    QPushButton* m_stroke = nullptr; // 线颜色选择器。
    QDoubleSpinBox* m_width = nullptr; // 线宽，单位米。
    QDoubleSpinBox* m_diameter = nullptr; // 点直径，单位米。
    QDoubleSpinBox* m_x = nullptr; // 当前节点横坐标，单位米。
    QDoubleSpinBox* m_y = nullptr; // 当前节点纵坐标，单位米。
    QComboBox* m_nodes = nullptr; // 节点选择列表。
    QLabel* m_info = nullptr; // 图形类型及节点数量。
    QDoubleSpinBox* m_angleStep = nullptr; // 最小角度编辑框。
    QDoubleSpinBox* m_gridStep = nullptr; // 网格间距编辑框。
    QComboBox* m_unit = nullptr; // 长度显示单位。
    QAction* m_grid = nullptr; // 网格开关。
    QAction* m_angles = nullptr; // 常用角吸附开关。
    QAction* m_quantize = nullptr; // 最小角倍数开关。
    QAction* m_ruler = nullptr; // 长度标尺开关。
    QAction* m_crosshair = nullptr; // 十字对齐线开关。
    bool m_refreshing = false; // 防止界面更新再次提交属性。
};
