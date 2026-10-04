#pragma once

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QWidget>
#include <QVector>

class QComboBox;
class QWidget;

// 全屏截图遮罩：按下热键后覆盖桌面，拖拽选择区域，随后在选区内添加标注，
// 确认时合成与预览一致的图像。取消时发出空图像。
// 遮罩坐标按“截图逻辑尺寸 / 遮罩尺寸”的比例映射到截图坐标，正常全屏使用下比值为 1。
class ScreenshotOverlay final : public QWidget
{
    Q_OBJECT
public:
    // desktopImage 为整个虚拟桌面的截图（设备像素）。
    explicit ScreenshotOverlay(const QImage& desktopImage, QWidget* parent = nullptr);
    // 当前是否处于选区标注阶段（供测试与状态查询）。
    bool isAnnotating() const { return m_state == Annotating; }
    // 当前选区（遮罩逻辑坐标）。
    QRectF selection() const { return m_selection; }
signals:
    // 确认时 image 为选区内带标注的合成图像；取消时 image 为空。
    void finished(const QImage& image, bool cancelled);
protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    // 系统直关闭（如 Alt+F4）视为取消，保证 finished 恰好发出一次。
    void closeEvent(QCloseEvent* event) override;
private:
    // 标注工具类型。
    enum Tool { Pen, Line, RectTool, Ellipse, Arrow };
    // 一次标注：画笔保存折线，形状与箭头保存起止两点。
    struct Stroke {
        Tool tool = Pen;
        QColor color;
        double width = 3;
        QVector<QPointF> points;
    };
    // 遮罩的两个阶段：拖拽选区、在选区内标注。
    enum State { Selecting, Annotating };

    // 创建底部标注工具栏（真实控件，保证可用性与悬停反馈）。
    void createToolbar();
    // 把工具栏摆放到选区右下角，空间不足时移到选区上方并夹紧到屏幕内。
    void updateToolbarGeometry();
    // 合成选区内的图像并结束截图。
    void finishCapture();
    // 取消整次截图。
    void cancelCapture();
    // 遮罩坐标 → 截图像素坐标。
    QPointF toImagePoint(const QPointF& position) const;
    // 选区的截图像素矩形。
    QRectF imageSelection() const;
    // 绘制全部标注（含正在绘制的），坐标为遮罩逻辑坐标。
    void paintStrokes(QPainter* painter) const;
    // 绘制选区边框。
    void drawSelectionBorder(QPainter* painter) const;

    // 桌面截图（设备像素）。
    QImage m_image;
    // 截图逻辑尺寸（设备像素 / DPR）。
    QSizeF m_imageLogical;
    // 当前阶段。
    State m_state = Selecting;
    // 选区（遮罩逻辑坐标）。
    QRectF m_selection;
    // 正在拖拽选区。
    bool m_dragging = false;
    QPointF m_dragStart;
    // 已确认与正在绘制的标注。
    QVector<Stroke> m_strokes;
    Stroke m_live;
    bool m_liveActive = false;
    // 当前工具、颜色与笔画宽度。
    Tool m_tool = Pen;
    QColor m_color{230, 47, 47};
    double m_width = 3;
    // 底部标注工具栏。
    QWidget* m_toolbar = nullptr;
    // finished 是否已经发出；close 时用于补发取消。
    bool m_finished = false;
};
