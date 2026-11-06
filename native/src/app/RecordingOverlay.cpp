#include "InterfaceLanguage.h"
#include "RecordingOverlay.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QHBoxLayout>

namespace {
// 拖拽过小视为误触的最小选区尺寸（遮罩逻辑像素）。
constexpr qreal kMinimumSelection = 8;
constexpr qreal kToolbarMargin = 8;
// 选区到工具栏的间距。
constexpr qreal kToolbarGap = 10;
}

RecordingOverlay::RecordingOverlay(const QImage& screenImage, QWidget* parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool)
    , m_image(screenImage)
{
    // 测试注入的图像可能没有 DPR；截屏得到的图像已由 Qt 设置。
    if (m_image.devicePixelRatio() <= 0)
        m_image.setDevicePixelRatio(1.0);
    m_imageLogical = QSizeF(m_image.size()) / m_image.devicePixelRatio();
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    createToolbar();
    m_toolbar->hide();
}

void RecordingOverlay::createToolbar()
{
    m_toolbar = new QWidget(this);
    m_toolbar->setObjectName(QStringLiteral("recordingToolbar"));
    m_toolbar->setFocusPolicy(Qt::NoFocus);
    m_toolbar->setStyleSheet(QStringLiteral(
        "#recordingToolbar{background:#1f2937;border:1px solid #374151;border-radius:8px;}"
        "#recordingToolbar QPushButton{background:transparent;color:#e5e7eb;border:1px solid #4b5563;"
        "border-radius:5px;min-width:52px;min-height:28px;font-size:12px;padding:0 10px;}"
        "#recordingToolbar QPushButton:hover{background:#374151;}"));
    auto* layout = new QHBoxLayout(m_toolbar);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(6);

    auto* cancel = new QPushButton(InterfaceLanguage::text(QStringLiteral("✕ 取消")), m_toolbar);
    InterfaceLanguage::bind(cancel, "text", QStringLiteral("✕ 取消"));
    cancel->setFocusPolicy(Qt::NoFocus);
    connect(cancel, &QPushButton::clicked, this, [this]() { cancelSelection(); });
    layout->addWidget(cancel);
    auto* start = new QPushButton(InterfaceLanguage::text(QStringLiteral("● 开始录屏")), m_toolbar);
    InterfaceLanguage::bind(start, "text", QStringLiteral("● 开始录屏"));
    start->setFocusPolicy(Qt::NoFocus);
    start->setStyleSheet(QStringLiteral(
        "QPushButton{background:#d92d2d;color:#fff;border-color:#d92d2d;font-weight:600;}"
        "QPushButton:hover{background:#b72424;}"));
    connect(start, &QPushButton::clicked, this, [this]() { confirmSelection(); });
    layout->addWidget(start);
}

void RecordingOverlay::startNewSelection(const QPointF& position)
{
    m_pressSelection = m_selection;
    m_drag = DragRubberBand;
    m_pressPos = position;
    m_selection = QRectF();
    m_state = Selecting;
    // 旧选区被替换：锚定其上的工具栏随旧选区一起消失，松开时按新选区重新摆放。
    m_toolbar->hide();
    // 多显示器下其余屏幕的遮罩也要收起各自的旧选区。
    emit newSelectionStarted();
}

void RecordingOverlay::dismissSelection()
{
    if (m_done)
        return;
    // 另一块屏幕开始重新框选：本屏的旧选区与工具栏一并收起，回到待框选状态。
    m_state = Selecting;
    m_selection = QRectF();
    m_toolbar->hide();
    update();
}

void RecordingOverlay::updateToolbarGeometry()
{
    if (!m_toolbar)
        return;
    m_toolbar->adjustSize();
    const QSize size = m_toolbar->size();
    const int margin = int(kToolbarMargin);
    // 优先放在选区下方居中；放不下移到选区上方，再放不下夹紧到屏幕内。
    int x = int(m_selection.center().x()) - size.width() / 2;
    int y = int(m_selection.bottom()) + int(kToolbarGap);
    if (y + size.height() > height() - margin)
        y = int(m_selection.top()) - size.height() - int(kToolbarGap);
    if (y < margin)
        y = int(m_selection.bottom()) + int(kToolbarGap);
    x = qBound(margin, x, qMax(margin, width() - size.width() - margin));
    y = qBound(margin, y, qMax(margin, height() - size.height() - margin));
    m_toolbar->move(x, y);
    m_toolbar->show();
    m_toolbar->raise();
}

void RecordingOverlay::updateCursor(const QPointF& position)
{
    if (m_state == Selecting || m_drag == DragRubberBand) {
        setCursor(Qt::CrossCursor);
        return;
    }
    setCursor(m_selection.contains(position) ? Qt::SizeAllCursor : Qt::CrossCursor);
}

void RecordingOverlay::drawSelectionBorder(QPainter* painter) const
{
    painter->setRenderHint(QPainter::Antialiasing, false);
    QPen pen(QColor(217, 45, 45), 1);
    pen.setCosmetic(true);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    painter->drawRect(m_selection);
}

void RecordingOverlay::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    // 整幅屏幕，随后把选区内的部分以原始亮度重绘。
    painter.drawImage(QRectF(0, 0, width(), height()), m_image);
    painter.fillRect(rect(), QColor(0, 0, 0, 110));
    if (!m_selection.isNull()) {
        // 用与背景完全相同的整图绘制并夹剪到选区，保证选区内外无缝对齐；
        // 不使用带源矩形的重载，避免高 DPI 下源矩形坐标语义产生偏移。
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.save();
        painter.setClipRect(m_selection);
        painter.drawImage(QRectF(0, 0, m_imageLogical.width(), m_imageLogical.height()), m_image);
        painter.restore();
        drawSelectionBorder(&painter);
        // 尺寸标签：显示选区的设备像素大小。
        const double scaleX = double(m_image.width()) / qMax<qreal>(1.0, m_imageLogical.width());
        const double scaleY = double(m_image.height()) / qMax<qreal>(1.0, m_imageLogical.height());
        const QString label = QStringLiteral("%1 × %2")
            .arg(int(m_selection.width() * scaleX)).arg(int(m_selection.height() * scaleY));
        painter.setFont(font());
        const QFontMetrics metrics(font());
        const QSize textSize = metrics.size(Qt::TextSingleLine, label);
        QRectF box(QPointF(0, 0), QSizeF(textSize) + QSizeF(14, 6));
        const double boxY = m_selection.bottom() - box.height() - 4 <= m_selection.top()
            ? m_selection.bottom() + 4 : m_selection.bottom() - box.height() - 4;
        box.moveBottomRight(QPointF(m_selection.right(), boxY));
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(17, 24, 39, 210));
        painter.drawRoundedRect(box, 4, 4);
        painter.setPen(Qt::white);
        painter.drawText(box, Qt::AlignCenter, label);
    } else {
        // 尚未选择区域时在顶部显示操作提示。
        const QString hint = InterfaceLanguage::text(QStringLiteral("拖拽选择录屏区域 · 框内拖动移动选区 · 框外拖动重新选区 · 右键或 Esc 取消"));
        painter.setFont(font());
        const QFontMetrics metrics(font());
        const QSize textSize = metrics.size(Qt::TextSingleLine, hint);
        QRectF box(QPointF(0, 0), QSizeF(textSize) + QSizeF(24, 10));
        box.moveCenter(QPointF(width() / 2.0, 40));
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(17, 24, 39, 190));
        painter.drawRoundedRect(box, 6, 6);
        painter.setPen(Qt::white);
        painter.drawText(box, Qt::AlignCenter, hint);
    }
}

void RecordingOverlay::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::RightButton) {
        cancelSelection();
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    const QPointF position = event->position();
    if (m_state == Confirming && m_selection.contains(position)) {
        // 框内拖动整体移动选区，工具栏随后重新摆放。
        m_drag = DragMove;
        m_pressSelection = m_selection;
        m_pressPos = position;
        m_moveOffset = position - m_selection.topLeft();
        m_toolbar->hide();
        update();
        return;
    }
    // 无选区或框外按下：开始一次全新框选。
    startNewSelection(position);
    update();
}

void RecordingOverlay::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF position = event->position();
    if (m_drag == DragRubberBand) {
        m_selection = QRectF(m_pressPos, position).normalized();
        update();
        return;
    }
    if (m_drag == DragMove) {
        QRectF moved = m_pressSelection.translated(position - m_pressPos);
        moved.moveLeft(qBound(0.0, moved.left(), qMax(0.0, width() - moved.width())));
        moved.moveTop(qBound(0.0, moved.top(), qMax(0.0, height() - moved.height())));
        m_selection = moved;
        update();
        return;
    }
    updateCursor(position);
}

void RecordingOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const QPointF position = event->position();
    if (m_drag == DragRubberBand) {
        m_drag = DragNone;
        m_selection = QRectF(m_pressPos, position).normalized();
        if (m_selection.width() < kMinimumSelection || m_selection.height() < kMinimumSelection) {
            // 过小的拖拽视为误触：恢复原选区；原本没有选区时回到初始状态。
            m_selection = m_pressSelection;
            m_state = m_selection.isNull() ? Selecting : Confirming;
            if (m_state == Confirming)
                updateToolbarGeometry();
        } else {
            m_state = Confirming;
            updateToolbarGeometry();
        }
        updateCursor(position);
        update();
    } else if (m_drag == DragMove) {
        m_drag = DragNone;
        updateToolbarGeometry();
        updateCursor(position);
        update();
    }
}

void RecordingOverlay::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        cancelSelection();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        && m_state == Confirming) {
        confirmSelection();
        return;
    }
    QWidget::keyPressEvent(event);
}

void RecordingOverlay::confirmSelection()
{
    if (m_state != Confirming || m_done)
        return;
    m_done = true;
    emit confirmRequested(m_selection);
    close();
}

void RecordingOverlay::cancelSelection()
{
    if (m_done)
        return;
    m_done = true;
    emit cancelled();
    close();
}

void RecordingOverlay::closeEvent(QCloseEvent* event)
{
    // 非本类发起的关闭（如 Alt+F4）按取消处理。
    event->accept();
    if (!m_done) {
        m_done = true;
        emit cancelled();
    }
}
