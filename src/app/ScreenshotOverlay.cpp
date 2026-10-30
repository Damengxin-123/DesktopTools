#include "InterfaceLanguage.h"
#include "ScreenshotOverlay.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QHBoxLayout>
#include <QToolButton>
#include <cmath>

namespace {
// 可选标注颜色（红、蓝、黄、绿、黑、白）。
const QColor kColors[]{{230, 47, 47}, {47, 111, 224}, {247, 213, 29},
    {55, 163, 86}, {17, 24, 39}, {255, 255, 255}};
// 笔画宽度选项（遮罩逻辑像素）与对应的文字字号。
const double kWidths[]{2, 4, 7};
const int kFontSizes[]{16, 22, 30};
// 缩放手柄的边长与命中外扩量（遮罩逻辑像素）。
constexpr qreal kHandleSize = 8;
constexpr qreal kHandleHitExtra = 3;
constexpr qreal kMinimumSelection = 8;
}

ScreenshotOverlay::ScreenshotOverlay(const QImage& screenImage, QWidget* parent)
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

void ScreenshotOverlay::createToolbar()
{
    m_toolbar = new QWidget(this);
    m_toolbar->setObjectName(QStringLiteral("screenshotToolbar"));
    m_toolbar->setFocusPolicy(Qt::NoFocus);
    m_toolbar->setStyleSheet(QStringLiteral(
        "#screenshotToolbar{background:#1f2937;border:1px solid #374151;border-radius:8px;}"
        "#screenshotToolbar QToolButton{background:transparent;color:#e5e7eb;border:1px solid transparent;"
        "border-radius:5px;min-width:30px;min-height:28px;font-size:14px;font-weight:600;}"
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

    // 七个工具按钮，单选；“移动”为默认工具。
    auto* tools = new QButtonGroup(m_toolbar);
    const struct {
        const char* label;
        const char* tip;
        Tool tool;
    } toolSpecs[]{{"✥", "移动/缩放截图框", MoveTool}, {"T", "添加文字", TextTool},
        {"✎", "画笔", Pen}, {"╱", "直线", Line}, {"▭", "矩形", RectTool},
        {"◯", "椭圆", Ellipse}, {"➚", "箭头", Arrow}};
    for (const auto& spec : toolSpecs) {
        auto* button = new QToolButton(m_toolbar);
        button->setText(QString::fromUtf8(spec.label));
        button->setToolTip(QString::fromUtf8(spec.tip));
        button->setCheckable(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setChecked(spec.tool == m_tool);
        connect(button, &QToolButton::clicked, this, [this, tool = spec.tool]() { setTool(tool); });
        tools->addButton(button);
        m_toolButtons.append(button);
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
    widthBox->addItem(InterfaceLanguage::text(QStringLiteral("细")), int(kWidths[0]));
    widthBox->addItem(InterfaceLanguage::text(QStringLiteral("中")), int(kWidths[1]));
    widthBox->addItem(InterfaceLanguage::text(QStringLiteral("粗")), int(kWidths[2]));
    widthBox->setCurrentIndex(1);
    InterfaceLanguage::bind(widthBox, "toolTip", QStringLiteral("笔画粗细 / 文字字号"));
    connect(widthBox, &QComboBox::activated, this, [this, widthBox](int index) {
        m_width = widthBox->itemData(index).toDouble();
        m_fontSize = kFontSizes[index];
    });
    layout->addWidget(widthBox);

    auto* undo = new QToolButton(m_toolbar);
    undo->setText(QStringLiteral("↶"));
    InterfaceLanguage::bind(undo, "toolTip", QStringLiteral("撤销上一步标注"));
    undo->setFocusPolicy(Qt::NoFocus);
    connect(undo, &QToolButton::clicked, this, [this]() {
        if (!m_strokes.isEmpty())
            m_strokes.removeLast();
        update();
    });
    layout->addWidget(undo);

    layout->addSpacing(6);
    auto* cancel = new QPushButton(InterfaceLanguage::text(QStringLiteral("✕ 取消")), m_toolbar);
    InterfaceLanguage::bind(cancel, "text", QStringLiteral("✕ 取消"));
    cancel->setFocusPolicy(Qt::NoFocus);
    connect(cancel, &QPushButton::clicked, this, [this]() { cancelCapture(); });
    layout->addWidget(cancel);
    auto* confirm = new QPushButton(InterfaceLanguage::text(QStringLiteral("✓ 完成")), m_toolbar);
    InterfaceLanguage::bind(confirm, "text", QStringLiteral("✓ 完成"));
    confirm->setFocusPolicy(Qt::NoFocus);
    confirm->setStyleSheet(QStringLiteral(
        "QPushButton{background:#315ee7;color:#fff;border-color:#315ee7;font-weight:600;}"
        "QPushButton:hover{background:#244fd3;}"));
    connect(confirm, &QPushButton::clicked, this, [this]() { finishCapture(); });
    layout->addWidget(confirm);
}

void ScreenshotOverlay::setTool(Tool tool)
{
    if (m_tool == tool)
        return;
    commitTextEditor();
    m_tool = tool;
    for (int index = 0; index < m_toolButtons.size(); ++index)
        m_toolButtons.at(index)->setChecked(index == int(tool));
    updateCursor(mapFromGlobal(QCursor::pos()));
    update();
}

void ScreenshotOverlay::startTextEditing(const QPointF& position)
{
    commitTextEditor();
    m_textAnchor = QPointF(
        qBound(m_selection.left(), position.x(), qMax(m_selection.left(), m_selection.right() - 60)),
        qBound(m_selection.top(), position.y(), qMax(m_selection.top(), m_selection.bottom() - m_fontSize - 10)));
    m_textEditor = new QLineEdit(this);
    QFont editorFont = font();
    editorFont.setPixelSize(m_fontSize);
    m_textEditor->setFont(editorFont);
    m_textEditor->setStyleSheet(QStringLiteral(
        "QLineEdit{background:rgba(255,255,255,235);color:%1;border:1px dashed #315ee7;padding:0 3px;}")
        .arg(m_color.name()));
    const qreal maxWidth = m_selection.right() - m_textAnchor.x() - 4;
    m_textEditor->setFixedSize(int(qMin<qreal>(280, qMax<qreal>(80, maxWidth))), m_fontSize + 10);
    m_textEditor->move(int(m_textAnchor.x()), int(m_textAnchor.y()));
    m_textEditor->installEventFilter(this);
    m_textEditor->show();
    m_textEditor->raise();
    m_textEditor->setFocus();
}

void ScreenshotOverlay::commitTextEditor()
{
    if (!m_textEditor)
        return;
    const QString text = m_textEditor->text().trimmed();
    const QPointF anchor = m_textAnchor;
    const QColor color = m_color;
    const int fontSize = m_fontSize;
    discardTextEditor();
    if (!text.isEmpty()) {
        Stroke stroke;
        stroke.tool = TextTool;
        stroke.color = color;
        stroke.fontSize = fontSize;
        stroke.points.append(anchor);
        stroke.text = text;
        m_strokes.append(stroke);
    }
    update();
}

void ScreenshotOverlay::discardTextEditor()
{
    if (!m_textEditor)
        return;
    m_textEditor->removeEventFilter(this);
    m_textEditor->deleteLater();
    m_textEditor = nullptr;
    update();
}

bool ScreenshotOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_textEditor && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            commitTextEditor();
            return true;
        }
        if (key->key() == Qt::Key_Escape) {
            discardTextEditor();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ScreenshotOverlay::startNewSelection(const QPointF& position)
{
    m_pressSelection = m_selection;
    m_drag = DragRubberBand;
    m_pressPos = position;
    m_selection = QRectF();
    // 旧选区被替换：锚定其上的工具栏随旧选区一起消失，松开时按新选区重新摆放。
    m_toolbar->hide();
    // 多显示器下其余屏幕的遮罩也要收起各自的旧选区。
    emit newSelectionStarted();
}

void ScreenshotOverlay::dismissSelection()
{
    if (m_finished)
        return;
    // 另一块屏幕开始重新框选：本屏的旧选区与工具栏一并收起，回到待框选状态。
    commitTextEditor();
    m_state = Selecting;
    m_selection = QRectF();
    m_toolbar->hide();
    update();
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

void ScreenshotOverlay::updateCursor(const QPointF& position)
{
    if (m_state == Selecting || m_drag == DragRubberBand) {
        setCursor(Qt::CrossCursor);
        return;
    }
    if (m_tool == TextTool) {
        setCursor(m_selection.contains(position) ? Qt::IBeamCursor : Qt::CrossCursor);
        return;
    }
    if (m_tool != MoveTool) {
        setCursor(Qt::CrossCursor);
        return;
    }
    const int handle = handleAt(position);
    if (handle == TopLeft || handle == BottomRight)
        setCursor(Qt::SizeFDiagCursor);
    else if (handle == TopRight || handle == BottomLeft)
        setCursor(Qt::SizeBDiagCursor);
    else if (handle == Top || handle == Bottom)
        setCursor(Qt::SizeVerCursor);
    else if (handle == Left || handle == Right)
        setCursor(Qt::SizeHorCursor);
    else if (m_selection.contains(position))
        setCursor(Qt::SizeAllCursor);
    else
        setCursor(Qt::CrossCursor);
}

QPointF ScreenshotOverlay::toImagePoint(const QPointF& position) const
{
    const double scaleX = m_imageLogical.width() / qMax(1.0, double(width()));
    const double scaleY = m_imageLogical.height() / qMax(1.0, double(height()));
    return QPointF(position.x() * scaleX, position.y() * scaleY);
}

// 选区在截图中的设备像素矩形。
QRectF ScreenshotOverlay::imageSelection() const
{
    const double scaleX = double(m_image.width()) / qMax(1.0, m_imageLogical.width());
    const double scaleY = double(m_image.height()) / qMax(1.0, m_imageLogical.height());
    return QRectF(m_selection.left() * scaleX, m_selection.top() * scaleY,
        m_selection.width() * scaleX, m_selection.height() * scaleY);
}

QVector<QRectF> ScreenshotOverlay::handleRects() const
{
    const QPointF points[8] = {
        {m_selection.left(), m_selection.top()},
        {m_selection.center().x(), m_selection.top()},
        {m_selection.right(), m_selection.top()},
        {m_selection.right(), m_selection.center().y()},
        {m_selection.right(), m_selection.bottom()},
        {m_selection.center().x(), m_selection.bottom()},
        {m_selection.left(), m_selection.bottom()},
        {m_selection.left(), m_selection.center().y()}
    };
    QVector<QRectF> rects;
    rects.reserve(8);
    for (const QPointF& point : points)
        rects.append(QRectF(point.x() - kHandleSize / 2, point.y() - kHandleSize / 2, kHandleSize, kHandleSize));
    return rects;
}

int ScreenshotOverlay::handleAt(const QPointF& position) const
{
    const QVector<QRectF> rects = handleRects();
    for (int index = 0; index < rects.size(); ++index) {
        if (rects.at(index).adjusted(-kHandleHitExtra, -kHandleHitExtra, kHandleHitExtra, kHandleHitExtra)
                .contains(position))
            return index;
    }
    return NoHandle;
}

void ScreenshotOverlay::paintEvent(QPaintEvent* event)
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
        const QString hint = InterfaceLanguage::text(QStringLiteral("拖拽选择截图区域 · 框外拖动重新选区 · 右键或 Esc 取消"));
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
    // 移动工具下显示 8 个缩放手柄。
    if (m_state == Annotating && m_tool == MoveTool && m_drag != DragRubberBand) {
        painter->setPen(QPen(QColor(49, 94, 231)));
        painter->setBrush(Qt::white);
        for (const QRectF& rect : handleRects())
            painter->drawRect(rect);
    }
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
        case MoveTool:
        case TextTool: {
            QFont textFont = painter->font();
            textFont.setPixelSize(qMax(8, stroke.fontSize));
            painter->setFont(textFont);
            painter->setPen(QPen(stroke.color));
            painter->drawText(QRectF(stroke.points.first(), QSizeF(16384, stroke.fontSize * 3)),
                Qt::AlignLeft | Qt::AlignTop | Qt::TextSingleLine, stroke.text);
            break;
        }
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
        discardTextEditor();
        cancelCapture();
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    const QPointF position = event->position();
    if (m_state == Selecting) {
        m_drag = DragRubberBand;
        m_pressPos = position;
        m_selection = QRectF();
        // 首次框选同样要收起其余屏幕遮罩的旧选区。
        emit newSelectionStarted();
        update();
        return;
    }
    // 点击其他位置前先提交未完成的文字。
    commitTextEditor();
    if (m_tool == MoveTool) {
        m_handle = handleAt(position);
        if (m_handle != NoHandle) {
            m_drag = DragResize;
            m_pressSelection = m_selection;
            m_pressPos = position;
        } else if (m_selection.contains(position)) {
            m_drag = DragMove;
            m_pressSelection = m_selection;
            m_pressPos = position;
            m_moveOffset = position - m_selection.topLeft();
        } else {
            startNewSelection(position);
        }
    } else if (m_tool == TextTool) {
        if (m_selection.contains(position))
            startTextEditing(position);
        else
            startNewSelection(position);
    } else if (m_selection.contains(position)) {
        m_drag = DragStroke;
        const QPointF clamped(qBound(m_selection.left(), position.x(), m_selection.right()),
            qBound(m_selection.top(), position.y(), m_selection.bottom()));
        m_live = Stroke{m_tool, m_color, m_width, {clamped, clamped}, QString(), 0};
        m_liveActive = true;
    } else {
        startNewSelection(position);
    }
    update();
}

void ScreenshotOverlay::mouseMoveEvent(QMouseEvent* event)
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
    if (m_drag == DragResize) {
        const QPointF clamped(qBound(0.0, position.x(), double(width())),
            qBound(0.0, position.y(), double(height())));
        qreal left = m_pressSelection.left();
        qreal top = m_pressSelection.top();
        qreal right = m_pressSelection.right();
        qreal bottom = m_pressSelection.bottom();
        if (m_handle == TopLeft || m_handle == Left || m_handle == BottomLeft)
            left = clamped.x();
        if (m_handle == TopLeft || m_handle == Top || m_handle == TopRight)
            top = clamped.y();
        if (m_handle == TopRight || m_handle == Right || m_handle == BottomRight)
            right = clamped.x();
        if (m_handle == BottomRight || m_handle == Bottom || m_handle == BottomLeft)
            bottom = clamped.y();
        m_selection = QRectF(QPointF(left, top), QPointF(right, bottom)).normalized();
        if (m_selection.width() < kMinimumSelection)
            m_selection.setWidth(kMinimumSelection);
        if (m_selection.height() < kMinimumSelection)
            m_selection.setHeight(kMinimumSelection);
        update();
        return;
    }
    if (m_drag == DragStroke) {
        const QPointF clamped(qBound(m_selection.left(), position.x(), m_selection.right()),
            qBound(m_selection.top(), position.y(), m_selection.bottom()));
        if (m_live.tool == Pen)
            m_live.points.append(clamped);
        else
            m_live.points[1] = clamped;
        update();
        return;
    }
    updateCursor(position);
}

void ScreenshotOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const QPointF position = event->position();
    if (m_drag == DragRubberBand) {
        // 松开后必须结束框选：鼠标跟踪开着，不重置的话后续移动会继续改选区。
        m_drag = DragNone;
        m_selection = QRectF(m_pressPos, position).normalized();
        if (m_selection.width() < kMinimumSelection || m_selection.height() < kMinimumSelection) {
            // 过小的拖拽视为误触：恢复原选区；原本没有选区时回到初始状态。
            m_selection = m_pressSelection;
            m_state = m_selection.isNull() ? Selecting : Annotating;
            if (m_state == Annotating)
                updateToolbarGeometry();
        } else {
            m_state = Annotating;
            setTool(MoveTool);
            updateToolbarGeometry();
        }
        updateCursor(position);
        update();
    } else if (m_drag == DragMove || m_drag == DragResize) {
        m_drag = DragNone;
        m_handle = NoHandle;
        updateCursor(position);
        update();
    } else if (m_drag == DragStroke) {
        m_drag = DragNone;
        m_liveActive = false;
        if (m_live.tool != Pen && m_live.points.first() == m_live.points.last())
            return; // 形状起止重合时丢弃，避免残留看不见的笔画。
        m_strokes.append(m_live);
        update();
    }
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
    commitTextEditor();
    const QRect crop = imageSelection().toAlignedRect().intersected(m_image.rect());
    if (crop.isEmpty()) {
        cancelCapture();
        return;
    }
    QImage composed(crop.size(), QImage::Format_ARGB32_Premultiplied);
    composed.setDevicePixelRatio(1.0);
    composed.fill(Qt::transparent);
    QPainter painter(&composed);
    // copy 会继承图像的 DPR；显式归一后按设备像素 1:1 画入，避免高 DPI 下缩成一角。
    QImage cropImage = m_image.copy(crop);
    cropImage.setDevicePixelRatio(1.0);
    painter.drawImage(QPoint(0, 0), cropImage);
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
    discardTextEditor();
    emit finished(QImage(), true);
    close();
}

void ScreenshotOverlay::closeEvent(QCloseEvent* event)
{
    // 非本类发起的关闭（如 Alt+F4）按取消处理。
    event->accept();
    if (!m_finished) {
        m_finished = true;
        discardTextEditor();
        emit finished(QImage(), true);
    }
}
