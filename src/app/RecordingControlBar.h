#pragma once

#include <QElapsedTimer>
#include <QWidget>

class QLabel;
class QPushButton;
class QTimer;

// 录制中的悬浮控制条：显示录制时长与状态，提供“暂停/继续”和“结束”按钮。
// 出现在选区外（选区内出现的任何窗口都会被录进视频）。整条可拖动换位，
// 始终置顶且不进入任务栏。
class RecordingControlBar final : public QWidget
{
    Q_OBJECT
public:
    explicit RecordingControlBar(QWidget* parent = nullptr);
    // 把控制条摆到选区外：selection 为全局坐标矩形，screen 为所在屏幕的全局矩形。
    // 依次尝试下方、上方、右侧、左侧，都放不下时贴屏幕底部（此时会被录进视频）。
    void placeOutside(const QRect& selection, const QRect& screen);
    // 显示有效录制时长；由持有方转发 ScreenRecorder 的 tick。
    void setElapsed(qint64 milliseconds);
    // 切换暂停态显示。
    void setPaused(bool paused);
    // 进入保存中状态：按钮全部禁用并提示落盘。
    void setSaving();
signals:
    void pauseRequested();
    void resumeRequested();
    void finishRequested();
protected:
    // 非按钮区域按下拖动整条控制条。
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
private:
    QLabel* m_dot = nullptr;
    QLabel* m_time = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_pauseButton = nullptr;
    QPushButton* m_finishButton = nullptr;
    QPointF m_dragOffset;
    bool m_dragging = false;
};
