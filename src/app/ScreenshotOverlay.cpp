#include "ScreenshotOverlay.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QToolButton>
#include <cmath>

namespace {
// 可选标注颜色（红、蓝、黄、绿、黑、白）。
const QColor kColors[]{{230, 47, 47}, {47, 111, 224}, {247, 213, 29},
    {55, 163, 86}, {17, 24, 39}, {255, 255, 255}};
// 笔画宽度选项（遮罩逻辑像素）。
const double kWidths[]{2, 4, 7};
}

ScreenshotOverlay::ScreenshotOverlay(const QImage& desktopImage, QWidget* parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool)
    , m_image(desktopImage)
{
    // 测试注入的图像可能没有 DPR；截屏得到的图像已由 Qt 设置。
    if (m_image.devicePixelRatio() <= 0)
        m_image.setDevicePixelRatio(1.0);
    m_imageLogical = QSizeF(m_image.size()) / m_image.devicePixelRatio();
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    const QScreen* screen = QGuiApplication::primaryScreen();
    const QRect geometry = screen ? screen->virtualGeometry() : QRect(0, 0, 800, 600);
    setGeometry(geometry);
    createToolbar();
    m_toolbar->hide();
}

void ScreenshotOverlay::createToolbar()
{
    m_toolbar = new QWidget(this);
    m_toolbar->setObjectName(QStringLiteral("screenshotToolbar"));
    m_toolbar->setFocusPolicy(Qt::NoFocus);
    m_toolbar->setStyleSheet(QStringLiteral(
        "#screenshotToolbar{background:#1f2937;border:1px solid #374151;border-radius:8px;}"
        "#screenshotToolbar QToolButton{background:transparent;color:#e5e7eb;border:1px solid transparent;"
        "border-radius:5px;min-width:30px;min-height:28px;font-size:15px;}"
        "#screenshotToolbar QToolButton:hover{background:#374151;}"
        "#screenshotToolbar QToolButton:checked{background:#315ee7;color:#fff;}"
        "#screenshotToolbar QComboBox{background:#111827;color:#e5e7eb;border:1px solid #374151;"
        "border-radius:5px;min-height:26px;font-size:12px;padding:0 4px;}"
        "#screenshotToolbar QPushButton{background:transparent;color:#e5e7eb;border:1px solid #4b5563;"
        "border-radius:5px;min-width:44px;min-height:28px;font-size:12px;padding:0 10px;}"
        "#screenshotToolbar QPushButton:hover{background:#374151;}"));
    auto* layout = new QHBoxLayout(m_toolbar);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(4);

    // 五个标注工具，单选。
    auto* tools = new QButtonGroup(m_toolbar);
    const struct {
        const char* label;
        const char* tip;
        Tool tool;
    } toolSpecs[]{{"✎", "画笔", Pen}, {"╱", "直线", Line}, {"▭", "矩形", RectTool},
        {"◯", "椭圆", Ellipse}, {"➚", "箭头", Arrow}};
    for (const auto& spec : toolSpecs) {
        auto* button = new QToolButton(m_toolbar);
        button->setText(QString::fromUtf8(spec.label));
        button->setToolTip(QString::fromUtf8(spec.tip));
        button->setCheckable(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setChecked(spec.tool == m_tool);
        connect(button, &QToolButton::clicked, this, [this, tool = spec.tool]() { m_tool = tool; });
        tools->addButton(button);
        layout->addWidget(button);
    }

    // 六个颜色块，单选。
    auto* colors = new QButtonGroup(m_toolbar);
    for (const QColor& color : kColors) {
        auto* button = new QToolButton(m_toolbar);
        button->setFixedSize(22, 22);
        button->setCheckable(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setChecked(color == m_color);
        button->setStyleSheet(QStringLiteral(
            "QToolButton{background:%1;border:2px solid #4b5563;border-radius:4px;min-width:0;min-height:0;}"
            "QToolButton:checked{border-color:#fff;}").arg(color.name()));
        connect(button, &QToolButton::clicked, this, [this, color]() { m_color = color; });
        colors->addButton(button);
        layout->addWidget(button);
    }

    auto* widthBox = new QComboBox(m_toolbar);
    widthBox->setFocusPolicy(Qt::NoFocus);
    widthBox->addItem(QStringLiteral("细"), kWidths[0]);
    widthBox->addItem(QStringLiteral("中"), kWidths[1]);
    widthBox->addItem(QStringLiteral("粗"), kWidths[2]);
    widthBox->setCurrentIndex(1);
    widthBox->setToolTip(QStringLiteral("笔画粗细"));
    connect(widthBox, &QComboBox::activated, this, [this, widthBox](int index) {
        m_width = widthBox->itemData(index).toDouble();
    });
    layout->addWidget(widthBox);

    auto* undo = new QToolButton(m_toolbar);
    undo->setText(QStringLiteral("↶"));
    undo->setToolTip(QStringLiteral("撤销上一步标注"));
    undo->setFocusPolicy(Qt::NoFocus);
    connect(undo, &QToolButton::clicked, this, [this]() {
        if (!m_strokes.isEmpty())
            m_strokes.removeLast();
        update();
    });
    layout->addWidget(undo);

    layout->addSpacing(6);
    auto* cancel = new QPushButton(QStringLiteral("✕ 取消"), m_toolbar);
    cancel->setFocusPolicy(Qt::NoFocus);
    connect(cancel, &QPushButton::clicked, this, [this]() { cancelCapture(); });
    layout->addWidget(cancel);
    auto* confirm = new QPushButton(QStringLiteral("✓ 完成"), m_toolbar);
    confirm->setFocusPolicy(Qt::NoFocus);
    confirm->setStyleSheet(QStringLiteral(
        "#screenshotToolbar QPushButton:checked{background:#315ee7;}"
        "QPushButton{background:#315ee7;color:#fff;border-color:#315ee7;font-weight:600;}"
        "QPushButton:hover{background:#244fd3;}"));
    connect(confirm, &QPushButton::clicked, this, [this]() { finishCapture(); });
    layout->addWidget(confirm);
}

void ScreenshotOverlay::updateToolbarGeometry()
{
    if (!m_toolbar)
        return;
    m_toolbar->adjustSize();
    const QSize size = m_toolbar->size();
    const int margin = 8;
    // 优先放在选区右下角下方；放不下移到选区上方，横向夹紧到屏幕内。
    int x = int(m_selection.right()) - size.width();
    int y = int(m_selection.bottom()) + margin;
    if (y + size.height() > height() - margin)
        y = int(m_selection.top()) - size.height() - margin;
    x = qBound(margin, x, qMax(margin, width() - size.width() - margin));
    y = qBound(margin, y, qMax(margin, height() - size.height() - margin));
    m_toolbar->move(x, y);
    m_toolbar->show();
    m_toolbar->raise();
}

QPointF ScreenshotOverlay::toImagePoint(const QPointF& position) const
{
    const double scaleX = m_imageLogical.width() / qMax(1.0, double(width()));
    const double scaleY = m_imageLogical.height() / qMax(1.0, double(height()));
    return QPointF(position.x() * scaleX, position.y() * scaleY);
}

QRectF ScreenshotOverlay::imageSelection() const
{
    return QRectF(toImagePoint(m_selection.topLeft()), toImagePoint(m_selection.bottomRight()));
}

void ScreenshotOverlay::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    // 整幅桌面，随后把选区内的部分以原始亮度重绘。
    painter.drawImage(QRectF(0, 0, width(), height()), m_image);
    painter.fillRect(rect(), QColor(0, 0, 0, 110));
    if (!m_selection.isNull()) {
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.drawImage(m_selection, m_image, imageSelection());
        drawSelectionBorder(&painter);
        painter.save();
        painter.setClipRect(m_selection);
        painter.setRenderHint(QPainter::Antialiasing, true);
        paintStrokes(&painter);
        painter.restore();
        // 尺寸标签：显示选区的设备像素大小。
        const QRectF imageRect = imageSelection();
        const QString label = QStringLiteral("%1 × %2").arg(int(imageRect.width())).arg(int(imageRect.height()));
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
        const QString hint = QStringLiteral("拖拽选择截图区域 · 右键或 Esc 取消");
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

void ScreenshotOverlay::drawSelectionBorder(QPainter* painter) const
{
    painter->setRenderHint(QPainter::Antialiasing, false);
    QPen pen(QColor(49, 94, 231), 1);
    pen.setCosmetic(true);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    painter->drawRect(m_selection);
}

void ScreenshotOverlay::paintStrokes(QPainter* painter) const
{
    QVector<Stroke> all = m_strokes;
    if (m_liveActive)
        all.append(m_live);
    for (const Stroke& stroke : all) {
        QPen pen(stroke.color, stroke.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        switch (stroke.tool) {
        case Pen:
            if (stroke.points.size() == 1) {
                // 单击画笔留下圆点。
                painter->setBrush(stroke.color);
                painter->drawEllipse(stroke.points.first(), stroke.width / 2, stroke.width / 2);
            } else {
                painter->drawPolyline(stroke.points.constData(), stroke.points.size());
            }
            break;
        case Line:
            painter->drawLine(stroke.points.first(), stroke.points.last());
            break;
        case RectTool:
            painter->drawRect(QRectF(stroke.points.first(), stroke.points.last()));
            break;
        case Ellipse:
            painter->drawEllipse(QRectF(stroke.points.first(), stroke.points.last()));
            break;
        case Arrow: {
            const QLineF line(stroke.points.first(), stroke.points.last());
            painter->drawLine(line);
            // 箭头末端按行进方向 ±150° 画出两条短线。
            const double angle = std::atan2(line.dy(), line.dx());
            const double head = qMax(12.0, stroke.width * 4);
            const QPointF tip = line.p2();
            for (const double offset : {M_PI * 5.0 / 6.0, -M_PI * 5.0 / 6.0}) {
                const QPointF wing(tip.x() + head * std::cos(angle + offset),
                    tip.y() + head * std::sin(angle + offset));
                painter->drawLine(tip, wing);
            }
            break;
        }
        }
    }
}

void ScreenshotOverlay::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::RightButton) {
        cancelCapture();
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    if (m_state == Selecting) {
        m_dragging = true;
        m_dragStart = event->position();
        m_selection = QRectF();
        update();
        return;
    }
    // 标注阶段：把起点夹紧到选区内。
    const QPointF clamped(qBound(m_selection.left(), double(event->position().x()), m_selection.right()),
        qBound(m_selection.top(), double(event->position().y()), m_selection.bottom()));
    m_live = Stroke{m_tool, m_color, m_width, {clamped, clamped}};
    m_liveActive = true;
    update();
}

void ScreenshotOverlay::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging) {
        m_selection = QRectF(m_dragStart, event->position()).normalized();
        update();
        return;
    }
    if (!m_liveActive)
        return;
    const QPointF clamped(qBound(m_selection.left(), double(event->position().x()), m_selection.right()),
        qBound(m_selection.top(), double(event->position().y()), m_selection.bottom()));
    if (m_live.tool == Pen)
        m_live.points.append(clamped);
    else
        m_live.points[1] = clamped;
    update();
}

void ScreenshotOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    if (m_dragging) {
        m_dragging = false;
        m_selection = QRectF(m_dragStart, event->position()).normalized();
        // 过小的拖拽视为误触，继续等待选区。
        if (m_selection.width() < 8 || m_selection.height() < 8) {
            m_selection = QRectF();
            update();
            return;
        }
        m_state = Annotating;
        updateToolbarGeometry();
        update();
        return;
    }
    if (!m_liveActive)
        return;
    m_liveActive = false;
    if (m_live.tool != Pen && m_live.points.first() == m_live.points.last())
        return; // 形状起止重合时丢弃，避免残留看不见的笔画。
    m_strokes.append(m_live);
    update();
}

void ScreenshotOverlay::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        cancelCapture();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        && m_state == Annotating) {
        finishCapture();
        return;
    }
    QWidget::keyPressEvent(event);
}

void ScreenshotOverlay::finishCapture()
{
    if (m_state != Annotating || m_finished)
        return;
    const QRect crop = imageSelection().toAlignedRect().intersected(m_image.rect());
    if (crop.isEmpty()) {
        cancelCapture();
        return;
    }
    QImage composed(crop.size(), QImage::Format_ARGB32_Premultiplied);
    composed.setDevicePixelRatio(1.0);
    composed.fill(Qt::transparent);
    QPainter painter(&composed);
    painter.drawImage(QPoint(0, 0), m_image.copy(crop));
    // 与屏幕预览使用同一套绘制逻辑：缩放到设备像素并平移到选区原点。
    const double scaleX = m_imageLogical.width() / qMax(1.0, double(width()));
    const double scaleY = m_imageLogical.height() / qMax(1.0, double(height()));
    painter.scale(scaleX, scaleY);
    painter.translate(-m_selection.topLeft());
    painter.setClipRect(m_selection);
    painter.setRenderHint(QPainter::Antialiasing, true);
    paintStrokes(&painter);
    painter.end();
    m_finished = true;
    emit finished(composed, false);
    close();
}

void ScreenshotOverlay::cancelCapture()
{
    if (m_finished)
        return;
    m_finished = true;
    emit finished(QImage(), true);
    close();
}

void ScreenshotOverlay::closeEvent(QCloseEvent* event)
{
    // 非本类发起的关闭（如 Alt+F4）按取消处理。
    event->accept();
    if (!m_finished) {
        m_finished = true;
        emit finished(QImage(), true);
    }
}
