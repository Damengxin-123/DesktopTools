#pragma once

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QWidget>
#include <QVector>

class QComboBox;
class QLineEdit;
class QToolButton;
class QWidget;

// 屏幕截图遮罩：覆盖一块屏幕，拖拽选择区域，随后在选区内添加标注，
// 确认时合成与预览一致的图像。取消或直关闭时发出空图像。
// 多显示器时每块屏幕一个遮罩，选区限制在所在屏幕内；遮罩坐标按
// “截图逻辑尺寸 / 遮罩尺寸”映射到截图坐标，因此各屏幕按自身 DPR 精确取图。
// 任一遮罩开始新的框选时，其余遮罩应收起旧选区与工具栏：持有方把
// newSelectionStarted 转发给其余遮罩的 dismissSelection。
// 选区出现后的默认工具是“移动”：框内拖动整体移动选区，边框上的 8 个手柄
// 微调大小，框外拖动重新选区；文字工具点击选区内放置行内输入框。
class ScreenshotOverlay final : public QWidget
{
    Q_OBJECT
public:
    // screenImage 为所在屏幕的截图（设备像素）；调用方负责把遮罩摆到对应屏幕。
    explicit ScreenshotOverlay(const QImage& screenImage, QWidget* parent = nullptr);
    // 当前是否处于选区标注阶段（供测试与状态查询）。
    bool isAnnotating() const { return m_state == Annotating; }
    // 当前选区（遮罩逻辑坐标）。
    QRectF selection() const { return m_selection; }
    // 已提交的标注数量（供测试与状态查询）。
    int annotationCount() const { return m_strokes.size(); }
    // 收起本遮罩的选区与工具栏，回到待框选状态。
    // 多显示器下另一屏幕开始重新框选时由持有方调用。
    void dismissSelection();
signals:
    // 确认时 image 为选区内带标注的合成图像；取消时 image 为空。
    void finished(const QImage& image, bool cancelled);
    // 本遮罩开始一次全新的框选（首次框选或替换旧选区）时发出。
    void newSelectionStarted();
protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    // 拦截文字编辑器内的 Enter/Esc，避免误触整次截图的确认或取消。
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    // 标注工具：移动（默认）用于移动与缩放选区，其余在选区内绘制。
    enum Tool { MoveTool, TextTool, Pen, Line, RectTool, Ellipse, Arrow };
    // 选区边框上的 8 个缩放手柄：四角与四边中点。
    enum Handle { NoHandle = -1, TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left };
    // 一次拖拽的种类。
    enum DragKind { DragNone, DragRubberBand, DragMove, DragResize, DragStroke };
    // 一次标注：画笔保存折线，形状与箭头保存起止两点，文字保存锚点。
    struct Stroke {
        Tool tool = Pen;
        QColor color;
        double width = 3;
        QVector<QPointF> points;
        QString text;      // 文字标注内容。
        int fontSize = 0;  // 文字标注的像素字号。
    };
    // 遮罩的两个阶段：拖拽选区、在选区内标注与调整。
    enum State { Selecting, Annotating };

    // 创建底部标注工具栏（真实控件，保证可用性与悬停反馈）。
    void createToolbar();
    // 切换工具并同步工具栏选中态；切换前提交未完成的文字。
    void setTool(Tool tool);
    // 在点击处放置文字输入框。
    void startTextEditing(const QPointF& position);
    // 提交（或静默丢弃）正在编辑的文字并销毁输入框。
    void commitTextEditor();
    void discardTextEditor();
    // 从选区外按下开始一次全新的框选，替换当前选区。
    void startNewSelection(const QPointF& position);
    // 确认时合成选区内的图像并结束截图。
    void finishCapture();
    // 取消整次截图。
    void cancelCapture();
    // 把工具栏摆放到选区右下角，空间不足时移到选区上方并夹紧到屏幕内。
    void updateToolbarGeometry();
    // 根据悬停位置与当前工具切换鼠标指针。
    void updateCursor(const QPointF& position);
    // 遮罩坐标 → 截图逻辑坐标。
    QPointF toImagePoint(const QPointF& position) const;
    // 选区在截图中的设备像素矩形。
    QRectF imageSelection() const;
    // 8 个缩放手柄的矩形。
    QVector<QRectF> handleRects() const;
    // 命中的手柄序号；不在手柄上时返回 NoHandle。
    int handleAt(const QPointF& position) const;
    // 绘制全部标注（含正在绘制的），坐标为遮罩逻辑坐标。
    void paintStrokes(QPainter* painter) const;
    // 绘制选区边框与手柄。
    void drawSelectionBorder(QPainter* painter) const;

    // 所在屏幕的截图（设备像素）。
    QImage m_image;
    // 截图逻辑尺寸（设备像素 / DPR）。
    QSizeF m_imageLogical;
    // 当前阶段。
    State m_state = Selecting;
    // 选区（遮罩逻辑坐标）。
    QRectF m_selection;
    // 拖拽开始前的选区备份，供移动、缩放与重新框选回退。
    QRectF m_pressSelection;
    // 进行中的拖拽种类与相关状态。
    DragKind m_drag = DragNone;
    int m_handle = NoHandle;
    QPointF m_pressPos;
    QPointF m_moveOffset;
    // 已确认与正在绘制的标注。
    QVector<Stroke> m_strokes;
    Stroke m_live;
    bool m_liveActive = false;
    // 当前工具、颜色、笔画宽度与文字字号。
    Tool m_tool = MoveTool;
    QColor m_color{230, 47, 47};
    double m_width = 4;
    int m_fontSize = 22;
    // 底部标注工具栏与按枚举顺序排列的工具按钮。
    QWidget* m_toolbar = nullptr;
    QList<QToolButton*> m_toolButtons;
    // 文字标注的行内输入框。
    QLineEdit* m_textEditor = nullptr;
    QPointF m_textAnchor;
    // finished 是否已经发出；close 时用于补发取消。
    bool m_finished = false;
};
