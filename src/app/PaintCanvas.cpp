#include "PaintCanvas.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTabletEvent>
#include <algorithm>

PaintCanvas::PaintCanvas(QWidget* parent) : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_TabletTracking);
    setCursor(Qt::CrossCursor);
    qApp->installEventFilter(this);
    QImage blank(1280, 800, QImage::Format_ARGB32);
    blank.fill(Qt::white);
    setImage(blank);
}

void PaintCanvas::setImage(const QImage& image)
{
    resetInput();
    if (image.isNull()) return;
    m_image = image.convertToFormat(QImage::Format_ARGB32);
    m_saved = m_image;
    m_undo.clear();
    m_redo.clear();
    setZoom(m_zoom);
}

void PaintCanvas::setTool(Tool tool) { resetInput(); m_tool = tool; update(); }
void PaintCanvas::setColor(const QColor& color) { resetInput(); m_color = color; }
void PaintCanvas::setBrushWidth(int width) { resetInput(); m_width = qBound(1, width, 200); update(); }
void PaintCanvas::setShapeFilled(bool filled) { resetInput(); m_filled = filled; }
void PaintCanvas::setText(const QString& text, int pixels) { resetInput(); m_text = text; m_textPixels = pixels; }
void PaintCanvas::setTabletMode(bool enabled) { resetInput(); m_tabletMode = enabled; emit changed(); }
void PaintCanvas::setDrawingKey(int key) { resetInput(); m_drawingKey = key; }
void PaintCanvas::setZoom(double zoom)
{
    resetInput();
    m_zoom = qBound(0.1, zoom, 4.0);
    setFixedSize(qRound(m_image.width() * m_zoom), qRound(m_image.height() * m_zoom));
    update();
    emit changed();
}
bool PaintCanvas::isModified() const { return m_image != m_saved; }
void PaintCanvas::markSaved() { finishStroke(); m_saved = m_image; emit changed(); }

void PaintCanvas::remember(const QImage& before)
{
    if (before == m_image) return;
    m_redo.clear();
    m_undo.append(before);
    const qsizetype budget = 128 * 1024 * 1024;
    qsizetype used = 0;
    for (const auto& entry : m_undo) used += entry.sizeInBytes();
    while (m_undo.size() > 1 && (used > budget || m_undo.size() > 40)) {
        used -= m_undo.first().sizeInBytes();
        m_undo.removeFirst();
    }
    emit changed();
}

void PaintCanvas::undo()
{
    resetInput();
    if (m_undo.isEmpty()) return;
    m_redo.append(m_image);
    m_image = m_undo.takeLast();
    update();
    emit changed();
}
void PaintCanvas::redo()
{
    resetInput();
    if (m_redo.isEmpty()) return;
    m_undo.append(m_image);
    m_image = m_redo.takeLast();
    update();
    emit changed();
}
void PaintCanvas::clear()
{
    resetInput();
    const QImage before = m_image;
    m_image.fill(Qt::white);
    remember(before);
    update();
}
void PaintCanvas::resetInput()
{
    finishStroke();
    m_down = false;
    m_keyHeld = false;
    emit drawingPermissionChanged(false);
}

void PaintCanvas::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.scale(m_zoom, m_zoom);
    painter.fillRect(m_image.rect(), Qt::white);
    painter.drawImage(0, 0, m_image);
    painter.setRenderHint(QPainter::Antialiasing);
    if (m_stroking) drawShape(painter);
    if (m_inside) {
        const double radius = qMax(3.0 / m_zoom, m_width / 2.0);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(Qt::white, 3.0 / m_zoom));
        painter.drawEllipse(m_position, radius, radius);
        painter.setPen(QPen(m_tabletMode && !m_keyHeld ? QColor("#d95935") : QColor("#263445"), 1.0 / m_zoom));
        painter.drawEllipse(m_position, radius, radius);
    }
}
void PaintCanvas::drawSegment(const QPointF& from, const QPointF& to)
{
    QPainter painter(&m_image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(m_tool == Tool::Eraser ? QColor(Qt::white) : m_color, m_width,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (from == to) painter.drawPoint(to);
    else painter.drawLine(from, to);
}
void PaintCanvas::drawShape(QPainter& painter) const
{
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(m_color, m_width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(m_filled ? QBrush(m_color) : QBrush(Qt::NoBrush));
    const QRectF bounds = QRectF(m_start, m_last).normalized();
    if (m_tool == Tool::Line) painter.drawLine(m_start, m_last);
    else if (m_tool == Tool::Rectangle) painter.drawRect(bounds);
    else if (m_tool == Tool::Ellipse) painter.drawEllipse(bounds);
}
void PaintCanvas::floodFill(const QPoint& point)
{
    const QRgb target = m_image.pixel(point);
    const QRgb replacement = m_color.rgba();
    if (target == replacement) return;
    QVector<QPoint> pending{point};
    while (!pending.isEmpty()) {
        const QPoint seed = pending.takeLast();
        auto* row = reinterpret_cast<QRgb*>(m_image.scanLine(seed.y()));
        if (row[seed.x()] != target) continue;
        int left = seed.x(), right = seed.x();
        while (left > 0 && row[left - 1] == target) --left;
        while (right + 1 < m_image.width() && row[right + 1] == target) ++right;
        std::fill(row + left, row + right + 1, replacement);
        for (const int y : {seed.y() - 1, seed.y() + 1}) {
            if (y < 0 || y >= m_image.height()) continue;
            const auto* adjacent = reinterpret_cast<const QRgb*>(m_image.constScanLine(y));
            bool span = false;
            for (int x = left; x <= right; ++x) {
                const bool match = adjacent[x] == target;
                if (match && !span) pending.append(QPoint(x, y));
                span = match;
            }
        }
    }
}
void PaintCanvas::beginStroke()
{
    m_stroking = true;
    m_start = m_last = m_position;
    m_before = m_image;
    if (m_tool == Tool::Brush || m_tool == Tool::Eraser) drawSegment(m_position, m_position);
    else if (m_tool == Tool::Fill) floodFill(QPoint(int(m_position.x()), int(m_position.y())));
    else if (m_tool == Tool::Picker) {
        m_color = m_image.pixelColor(int(m_position.x()), int(m_position.y()));
        emit colorPicked(m_color);
    } else if (m_tool == Tool::Text && !m_text.isEmpty()) {
        QPainter painter(&m_image);
        QFont font = painter.font();
        font.setPixelSize(m_textPixels);
        painter.setFont(font);
        painter.setPen(m_color);
        painter.drawText(m_position, m_text);
    }
}
void PaintCanvas::finishStroke()
{
    if (!m_stroking) return;
    {
        QPainter painter(&m_image);
        drawShape(painter);
    }
    m_stroking = false;
    remember(m_before);
    m_before = QImage();
    update();
}
void PaintCanvas::pointer(const QPointF& position, bool down)
{
    m_position = position / m_zoom;
    m_inside = m_position.x() >= 0 && m_position.y() >= 0
        && m_position.x() < m_image.width() && m_position.y() < m_image.height();
    m_down = down;
    if (!m_down || !m_inside || (m_tabletMode && !m_keyHeld)) finishStroke();
    else if (!m_stroking) beginStroke();
    else {
        if (m_tool == Tool::Brush || m_tool == Tool::Eraser) drawSegment(m_last, m_position);
        m_last = m_position;
    }
    update();
}
void PaintCanvas::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;
    setFocus(Qt::MouseFocusReason);
    pointer(event->position(), true);
    event->accept();
}
void PaintCanvas::mouseMoveEvent(QMouseEvent* event)
{
    pointer(event->position(), event->buttons().testFlag(Qt::LeftButton));
    event->accept();
}
void PaintCanvas::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;
    if (m_stroking) pointer(event->position(), true);
    pointer(event->position(), false);
    event->accept();
}
void PaintCanvas::tabletEvent(QTabletEvent* event)
{
    if (event->type() == QEvent::TabletPress) setFocus(Qt::MouseFocusReason);
    const bool down = event->buttons().testFlag(Qt::LeftButton) && event->type() != QEvent::TabletRelease;
    if (event->type() == QEvent::TabletRelease && m_stroking) pointer(event->position(), true);
    pointer(event->position(), down);
    // 接受原生笔事件，阻止 Qt 再合成一套鼠标笔画。
    event->accept();
}
void PaintCanvas::leaveEvent(QEvent*) { finishStroke(); m_inside = false; update(); }
bool PaintCanvas::eventFilter(QObject* watched, QEvent* event)
{
    if ((watched == this && event->type() == QEvent::FocusOut)
        || (watched == window() && (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide))
        || event->type() == QEvent::ApplicationDeactivate) resetInput();
    if (m_tabletMode && hasFocus() && (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease
                                      || event->type() == QEvent::ShortcutOverride)) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == m_drawingKey) {
            if (event->type() == QEvent::ShortcutOverride) { event->accept(); return true; }
            if (key->isAutoRepeat()) return true;
            m_keyHeld = event->type() == QEvent::KeyPress;
            emit drawingPermissionChanged(m_keyHeld);
            if (!m_keyHeld) finishStroke();
            else if (m_down && m_inside && !m_stroking) beginStroke();
            update();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
