#include "RecordingRegionFrame.h"

#include <QPainter>
#include <QTimer>

namespace {
// 提示框边宽（选区外侧的带状区域），同时是窗口相对选区的外扩量。
constexpr int kBorderWidth = 4;
// 呼吸闪烁的切换间隔。
constexpr int PulseIntervalMilliseconds = 650;
constexpr qreal BrightOpacity = 1.0;
constexpr qreal DimOpacity = 0.55;
} // namespace

RecordingRegionFrame::RecordingRegionFrame(QWidget* parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool
          | Qt::WindowTransparentForInput)
{
    // 半透明窗口只画一圈边框；对输入完全穿透，不激活、不抢焦点。
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    m_pulseTimer = new QTimer(this);
    m_pulseTimer->setInterval(PulseIntervalMilliseconds);
    connect(m_pulseTimer, &QTimer::timeout, this, [this]() {
        m_bright = !m_bright;
        setWindowOpacity(m_bright ? BrightOpacity : DimOpacity);
    });
}

void RecordingRegionFrame::setFrameRegion(const QRect& selection)
{
    // 窗口覆盖选区外扩 kBorderWidth 的矩形，边框画在外扩带内，正好包住选区。
    setGeometry(selection.adjusted(-kBorderWidth, -kBorderWidth, kBorderWidth, kBorderWidth));
}

void RecordingRegionFrame::setPausedVisual(bool paused)
{
    m_color = paused ? QColor(247, 213, 29) : QColor(236, 72, 58);
    update();
}

void RecordingRegionFrame::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    m_bright = true;
    setWindowOpacity(BrightOpacity);
    m_pulseTimer->start();
}

void RecordingRegionFrame::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    m_pulseTimer->stop();
}

void RecordingRegionFrame::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.fillRect(0, 0, width(), kBorderWidth, m_color);                          // 上
    painter.fillRect(0, height() - kBorderWidth, width(), kBorderWidth, m_color);    // 下
    painter.fillRect(0, 0, kBorderWidth, height(), m_color);                         // 左
    painter.fillRect(width() - kBorderWidth, 0, kBorderWidth, height(), m_color);    // 右
}
