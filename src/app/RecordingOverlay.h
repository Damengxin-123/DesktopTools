#pragma once

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QWidget>
#include <QVector>

class QPushButton;

// 录屏选区遮罩：覆盖一块屏幕，拖拽选择录制区域。选区确认后出现“开始录屏 / 取消”
// 工具栏（摆放在选区外），Enter 或点击开始时发出 confirmRequested；取消、右键、
// Esc 或直接关闭时发出 cancelled。多显示器时每块屏幕一个遮罩，选区限制在所在
// 屏幕内，坐标按“截图逻辑尺寸 / 遮罩尺寸”映射，与截图遮罩使用同一套高 DPI 约定。
// 选区出现后：框内拖动整体移动选区，框外拖动重新框选。
class RecordingOverlay final : public QWidget
{
    Q_OBJECT
public:
    // screenImage 为所在屏幕的截图（设备像素）；调用方负责把遮罩摆到对应屏幕。
    explicit RecordingOverlay(const QImage& screenImage, QWidget* parent = nullptr);
    // 当前是否处于选区确认阶段（供测试与状态查询）。
    bool isConfirming() const { return m_state == Confirming; }
    // 当前选区（遮罩逻辑坐标）。
    QRectF selection() const { return m_selection; }
    // 收起本遮罩的选区与工具栏，回到待框选状态。
    // 多显示器下另一屏幕开始重新框选时由持有方调用。
    void dismissSelection();
signals:
    // 选区确认：selection 为遮罩逻辑坐标，由持有方换算到设备像素后开始录制。
    void confirmRequested(QRectF selection);
    // 取消整次录屏流程（含遮罩被动关闭，如 Alt+F4）。
    void cancelled();
    // 本遮罩开始一次全新的框选（首次框选或替换旧选区）时发出。
    void newSelectionStarted();
protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
private:
    // 拖拽种类：框选新区域、移动选区。
    enum DragKind { DragNone, DragRubberBand, DragMove };
    // 遮罩的两个阶段：拖拽选区、确认选区（工具栏可见）。
    enum State { Selecting, Confirming };

    // 创建“开始录屏 / 取消”工具栏（真实控件，保证可用性与悬停反馈）。
    void createToolbar();
    // 从选区外按下开始一次全新的框选，替换当前选区。
    void startNewSelection(const QPointF& position);
    // 确认选区并请求开始录制。
    void confirmSelection();
    // 取消整次录屏。
    void cancelSelection();
    // 把工具栏摆放到选区外（下方优先，其次上方），夹紧到屏幕内。
    void updateToolbarGeometry();
    // 根据悬停位置切换鼠标指针。
    void updateCursor(const QPointF& position);
    // 选区边框与尺寸标签。
    void drawSelectionBorder(QPainter* painter) const;

    // 所在屏幕的截图（设备像素），仅用于选区阶段的背景与亮度预览。
    QImage m_image;
    // 截图逻辑尺寸（设备像素 / DPR）。
    QSizeF m_imageLogical;
    // 当前阶段。
    State m_state = Selecting;
    // 选区（遮罩逻辑坐标）。
    QRectF m_selection;
    // 拖拽开始前的选区备份，供移动与重新框选回退。
    QRectF m_pressSelection;
    // 进行中的拖拽与相关状态。
    DragKind m_drag = DragNone;
    QPointF m_pressPos;
    QPointF m_moveOffset;
    // “开始录屏 / 取消”工具栏。
    QWidget* m_toolbar = nullptr;
    // cancelled / confirmRequested 是否已经发出；close 时用于补发取消。
    bool m_done = false;
};
