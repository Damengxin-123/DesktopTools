#pragma once
#include "LevelDocument.h"
#include <QWidget>

// 使用米制坐标的无限画布，负责绘制、命中测试、节点编辑和历史记录。
class LevelCanvas final : public QWidget {
    Q_OBJECT
public:
    // 创建可接受鼠标及键盘操作的画布。
    explicit LevelCanvas(QWidget* parent = nullptr);
    // 当前正式文档；尚未结束的折线单独管理。
    const LevelDocument& document() const { return m_document; }
    // 获取当前选中对象索引。
    int selection() const { return m_selected; }
    // 判断是否存在尚未结束的绘制。
    bool hasDraft() const { return !m_draft.points.isEmpty(); }
    // 加载文档并清空历史。
    void load(const LevelDocument& document);
    // 提交一次可撤销的文档修改。
    void apply(const LevelDocument& document);
    // 更新选中对象并记录历史。
    void replaceSelected(const LevelObject& object);
    // 切换绘制工具，先完成已有图形。
    void setTool(const QString& tool);
    // 从对象列表选择对应图形。
    void select(int index);
    // 结束当前点集、折线或多边形。
    bool finish();
    // 取消当前临时图形。
    void cancelDraft();
    // 删除选中的整个对象。
    void removeSelected();
    // 复制当前对象并稍微偏移位置。
    void duplicateSelected();
    // 撤销最近的完整编辑操作。
    void undo();
    // 重做最近撤销的操作。
    void redo();
    // 将整个项目缩放到视口中。
    void fit();
    // 返回新绘图对象使用的默认样式。
    LevelObject drawingStyle() const { return m_style; }
    // 更改新图形样式，同时更新当前绘图草稿。
    void setDrawingStyle(const LevelObject& style);
    // 将米制坐标转换为视口坐标，亦供输入验证使用。
    QPointF screen(QPointF world) const;
    // 将视口坐标转换为米制坐标。
    QPointF world(QPointF screen) const;
signals:
    // 正式文档或临时绘图改变，窗口更新未保存标志。
    void changed();
    // 选择或节点坐标变化，刷新右侧属性。
    void selectionChanged();
    // 向状态栏显示操作提示。
    void message(const QString& text);
protected:
    // 绘制背景网格、所有对象和编辑辅助。
    void paintEvent(QPaintEvent*) override;
    // 添加节点、开始选中拖动或平移视口。
    void mousePressEvent(QMouseEvent* event) override;
    // 更新预览、选中对象位置和十字线。
    void mouseMoveEvent(QMouseEvent* event) override;
    // 完成一次拖动并记录历史。
    void mouseReleaseEvent(QMouseEvent* event) override;
    // 双击线段插入节点，绘图时结束当前对象。
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    // 以鼠标位置为中心缩放视口。
    void wheelEvent(QWheelEvent* event) override;
    // 处理绘图结束、取消、回退节点及删除。
    void keyPressEvent(QKeyEvent* event) override;
private:
    // 查找鼠标位置下最上层的对象。
    int hit(QPointF world) const;
    // 将当前文档追加到历史记录。
    void record();
    // 更新拖动中的顶点或整体位置。
    void dragTo(QPointF position);
    LevelDocument m_document; // 当前项目。
    QList<LevelDocument> m_history; // 完整操作历史，保留最近一百次。
    int m_historyIndex = 0; // 当前历史位置。
    int m_selected = -1; // 当前对象索引。
    int m_vertex = -1; // 当前拖动节点索引。
    QString m_tool = "select"; // 当前绘图或选择工具。
    LevelObject m_style; // 新建对象样式。
    LevelObject m_draft; // 未提交的连续绘图。
    LevelObject m_beforeDrag; // 拖动开始前的几何，防止累积误差。
    QPointF m_dragStart; // 拖动起点，单位米。
    QPointF m_mouse; // 当前鼠标视口坐标。
    QPointF m_panStart; // 上次视口平移位置。
    QPointF m_origin = QPointF(60, 60); // 世界原点对应的视口坐标。
    double m_scale = 12.00; // 每米对应屏幕像素。
    bool m_dragging = false; // 是否正在拖动对象。
    bool m_panning = false; // 是否正在平移视口。
};
