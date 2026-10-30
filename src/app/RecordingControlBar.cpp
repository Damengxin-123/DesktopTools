#include "InterfaceLanguage.h"
#include "RecordingControlBar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>

namespace {
// 控制条与选区边缘的间距。
constexpr int kGap = 12;
// 全屏选区时的屏幕边缘兜底间距。
constexpr int kEdgeMargin = 8;

QString formatDuration(qint64 milliseconds)
{
    const qint64 total = milliseconds / 1000;
    const qint64 hours = total / 3600;
    const qint64 minutes = (total % 3600) / 60;
    const qint64 seconds = total % 60;
    if (hours > 0)
        return QStringLiteral("%1:%2:%3").arg(hours).arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
}
} // namespace

RecordingControlBar::RecordingControlBar(QWidget* parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool)
{
    setObjectName(QStringLiteral("recordingControlBar"));
    setFocusPolicy(Qt::NoFocus);
    setStyleSheet(QStringLiteral(
        "#recordingControlBar{background:#1f2937;border:1px solid #374151;border-radius:9px;}"
        "#recordingControlBar QLabel{color:#e5e7eb;font-size:13px;font-weight:600;background:transparent;}"
        "#recordingControlBar QLabel#recordingDot{color:#f04438;font-size:15px;}"
        "#recordingControlBar QLabel#recordingStatus{color:#99a3b5;font-size:11px;font-weight:400;}"
        "#recordingControlBar QPushButton{background:transparent;color:#e5e7eb;border:1px solid #4b5563;"
        "border-radius:5px;min-width:56px;min-height:28px;font-size:12px;padding:0 10px;}"
        "#recordingControlBar QPushButton:hover{background:#374151;}"
        "#recordingControlBar QPushButton:disabled{color:#6b7280;}"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 7, 8, 7);
    layout->setSpacing(8);

    m_dot = new QLabel(QStringLiteral("●"), this);
    m_dot->setObjectName(QStringLiteral("recordingDot"));
    m_time = new QLabel(QStringLiteral("0:00"), this);
    m_status = new QLabel(InterfaceLanguage::text(QStringLiteral("录制中")), this);
    InterfaceLanguage::bind(m_status, "text", QStringLiteral("录制中"));
    m_status->setObjectName(QStringLiteral("recordingStatus"));

    m_pauseButton = new QPushButton(InterfaceLanguage::text(QStringLiteral("⏸ 暂停")), this);
    InterfaceLanguage::bind(m_pauseButton, "text", QStringLiteral("⏸ 暂停"));
    m_pauseButton->setFocusPolicy(Qt::NoFocus);
    connect(m_pauseButton, &QPushButton::clicked, this, [this]() {
        emit pauseRequested();
    });
    m_finishButton = new QPushButton(InterfaceLanguage::text(QStringLiteral("⏹ 结束")), this);
    InterfaceLanguage::bind(m_finishButton, "text", QStringLiteral("⏹ 结束"));
    m_finishButton->setFocusPolicy(Qt::NoFocus);
    m_finishButton->setStyleSheet(QStringLiteral(
        "QPushButton{background:#315ee7;color:#fff;border-color:#315ee7;font-weight:600;}"
        "QPushButton:hover{background:#244fd3;}"
        "QPushButton:disabled{background:#4a5568;color:#9aa5b8;}"));
    connect(m_finishButton, &QPushButton::clicked, this, &RecordingControlBar::finishRequested);

    layout->addWidget(m_dot);
    layout->addWidget(m_time);
    layout->addWidget(m_status);
    layout->addSpacing(4);
    layout->addWidget(m_pauseButton);
    layout->addWidget(m_finishButton);
    adjustSize();
    setFixedSize(size());
}

void RecordingControlBar::placeOutside(const QRect& selection, const QRect& screen)
{
    const QSize size = this->size();
    // 依次尝试下方、上方、右侧、左侧；最后贴屏幕底部兜底（选区几乎占满屏幕时）。
    const int belowY = selection.bottom() + kGap;
    const int aboveY = selection.top() - kGap - size.height();
    const int rightX = selection.right() + kGap;
    const int leftX = selection.left() - kGap - size.width();
    int x = selection.left();
    int y = belowY;
    if (belowY + size.height() <= screen.bottom() - kEdgeMargin) {
        x = selection.left();
        y = belowY;
    } else if (aboveY >= screen.top() + kEdgeMargin) {
        x = selection.left();
        y = aboveY;
    } else if (rightX + size.width() <= screen.right() - kEdgeMargin) {
        x = rightX;
        y = selection.top();
    } else if (leftX >= screen.left() + kEdgeMargin) {
        x = leftX;
        y = selection.top();
    } else {
        x = selection.center().x() - size.width() / 2;
        y = screen.bottom() - size.height() - kEdgeMargin;
    }
    // 无论哪一侧都夹紧到屏幕内，避免遮挡相邻屏幕或越界。
    x = qBound(screen.left() + kEdgeMargin, x, screen.right() - size.width() - kEdgeMargin);
    y = qBound(screen.top() + kEdgeMargin, y, screen.bottom() - size.height() - kEdgeMargin);
    move(x, y);
}

void RecordingControlBar::setElapsed(qint64 milliseconds)
{
    m_time->setText(formatDuration(milliseconds));
}

void RecordingControlBar::setPaused(bool paused)
{
    m_dot->setText(paused ? QStringLiteral("⏸") : QStringLiteral("●"));
    m_dot->setStyleSheet(paused
        ? QStringLiteral("color:#f7d529;font-size:15px;")
        : QStringLiteral("color:#f04438;font-size:15px;"));
    InterfaceLanguage::bind(m_status, "text", paused ? QStringLiteral("已暂停") : QStringLiteral("录制中"));
    InterfaceLanguage::bind(m_pauseButton, "text", paused ? QStringLiteral("▶ 继续") : QStringLiteral("⏸ 暂停"));
}

void RecordingControlBar::setSaving()
{
    m_pauseButton->setEnabled(false);
    m_finishButton->setEnabled(false);
    InterfaceLanguage::bind(m_status, "text", QStringLiteral("正在保存…"));
}

void RecordingControlBar::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_dragOffset = event->globalPosition() - QPointF(pos());
        return;
    }
    QWidget::mousePressEvent(event);
}

void RecordingControlBar::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging)
        move((event->globalPosition() - m_dragOffset).toPoint());
    else
        QWidget::mouseMoveEvent(event);
}

void RecordingControlBar::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        return;
    }
    QWidget::mouseReleaseEvent(event);
}
