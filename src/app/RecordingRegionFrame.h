#pragma once

#include <QColor>
#include <QWidget>

class QTimer;

// 录制中的区域提示框：沿选区外侧描出一圈醒目的红色边框（占选区外 4px 的带），
// 因此永远不会被录进视频；窗口对鼠标完全穿透（点击、拖拽都落在下层应用上），
// 不影响录制区域的正常使用。录制中红框呼吸闪烁，暂停时变为黄色提示。
class RecordingRegionFrame final : public QWidget
{
    Q_OBJECT
public:
    explicit RecordingRegionFrame(QWidget* parent = nullptr);
    // selection 为选区的全局坐标矩形；窗口整体外扩一个边框宽度。
    void setFrameRegion(const QRect& selection);
    // 切换录制/暂停的边框颜色。
    void setPausedVisual(bool paused);
protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
private:
    // 边框颜色（录制中红色，暂停黄色）。
    QColor m_color{236, 72, 58};
    // 呼吸闪烁：明暗两档交替。
    QTimer* m_pulseTimer = nullptr;
    bool m_bright = true;
};
