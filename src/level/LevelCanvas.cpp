#include "LevelCanvas.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPathStroker>
#include <QUuid>
#include <QWheelEvent>
#include <cmath>
#include <algorithm>

LevelCanvas::LevelCanvas(QWidget* parent) : QWidget(parent)
{
    setObjectName("levelCanvas");
    setFocusPolicy(Qt::StrongFocus); setMouseTracking(true); setMinimumSize(280, 240);
    m_history.append(m_document);
}

QPointF LevelCanvas::screen(QPointF point) const { return point * m_scale + m_origin; }
QPointF LevelCanvas::world(QPointF point) const { return (point - m_origin) / m_scale; }

void LevelCanvas::load(const LevelDocument& document)
{
    m_document = document; m_draft = {}; m_selected = m_vertex = -1; m_dragging = m_panning = false;
    m_history = {m_document}; m_historyIndex = 0;
    fit(); emit changed(); emit selectionChanged();
}

void LevelCanvas::record()
{
    if (m_history[m_historyIndex].json() == m_document.json()) return;
    while (m_history.size() > m_historyIndex + 1) m_history.removeLast();
    m_history.append(m_document);
    if (m_history.size() > 100) m_history.removeFirst();
    m_historyIndex = int(m_history.size()) - 1;
    emit changed(); update();
}

void LevelCanvas::apply(const LevelDocument& document)
{
    m_document = document; record(); update();
}

void LevelCanvas::replaceSelected(const LevelObject& object)
{
    if (m_selected < 0 || m_selected >= m_document.objects.size()) return;
    m_document.objects[m_selected] = object; record(); emit selectionChanged();
}

void LevelCanvas::setDrawingStyle(const LevelObject& style)
{
    m_style = style;
    m_draft.fill = style.fill; m_draft.stroke = style.stroke;
    m_draft.width = style.width; m_draft.diameter = style.diameter;
    update();
}

void LevelCanvas::setTool(const QString& tool)
{
    if (!finish()) return;
    m_tool = tool; m_vertex = -1; m_dragging = false;
    // 预设只提供样式与默认绘制类型，几何完全来自用户逐点输入。
    if (tool.startsWith("preset:")) m_style = LevelDrawing::presetStyle(tool.mid(7));
    else if (tool != "select") {
        if (!m_style.preset.isEmpty()) m_style = LevelObject();
        m_style.kind = tool;
    }
    select(-1); setFocus();
    const QString hint = m_style.preset.isEmpty() ? QString() :
        m_style.preset + (m_style.kind == "polyline" ? QStringLiteral(" · 连续折线：") : QStringLiteral(" · 多边形："));
    emit message(tool == "select" ? QStringLiteral("拖动对象或红色节点；双击线段添加节点；Delete 删除选中节点或对象。")
        : hint + QStringLiteral("左键连续添加节点；Enter / 右键完成；Backspace 回退；Esc 取消；中键平移，滚轮缩放。"));
}

void LevelCanvas::select(int index)
{
    m_selected = index >= 0 && index < m_document.objects.size() ? index : -1;
    m_vertex = -1; emit selectionChanged(); update();
}

bool LevelCanvas::finish()
{
    if (!hasDraft()) return true;
    const int minimum = m_draft.kind == "polygon" ? 3 : m_draft.kind == "polyline" ? 2 : 1;
    if (m_draft.points.size() < minimum) {
        emit message(QStringLiteral("当前图形至少需要 %1 个节点；继续绘制或按 Esc 取消。").arg(minimum)); return false;
    }
    m_draft.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_draft.name = (m_draft.preset.isEmpty()
        ? (m_draft.kind == "polygon" ? QStringLiteral("面") : m_draft.kind == "polyline" ? QStringLiteral("线") : QStringLiteral("点集"))
        : m_draft.preset) + QString::number(m_document.objects.size() + 1);
    m_document.objects.append(m_draft); m_draft = {};
    m_selected = int(m_document.objects.size()) - 1; m_vertex = -1;
    record(); emit selectionChanged(); update(); return true;
}

void LevelCanvas::cancelDraft() { m_draft = {}; emit changed(); update(); }

void LevelCanvas::removeSelected()
{
    if (m_selected < 0) return;
    m_document.objects.removeAt(m_selected); select(-1); record();
}

void LevelCanvas::duplicateSelected()
{
    if (m_selected < 0) return;
    auto object = m_document.objects[m_selected];
    object.id = QUuid::createUuid().toString(QUuid::WithoutBraces); object.name += QStringLiteral(" 副本");
    object.points.translate(1, 1); m_document.objects.append(object);
    select(int(m_document.objects.size()) - 1); record();
}

void LevelCanvas::undo()
{
    if (hasDraft()) { m_draft.points.removeLast(); emit changed(); update(); return; }
    if (m_historyIndex <= 0) return;
    m_document = m_history[--m_historyIndex]; select(-1); emit changed(); update();
}

void LevelCanvas::redo()
{
    if (hasDraft() || m_historyIndex + 1 >= m_history.size()) return;
    m_document = m_history[++m_historyIndex]; select(-1); emit changed(); update();
}

void LevelCanvas::fit()
{
    const auto bounds = m_document.bounds();
    m_scale = std::clamp(std::min((width() - 60) / bounds.width(), (height() - 60) / bounds.height()), .00001, 800.);
    m_origin = QPointF(width()/2., height()/2.) - bounds.center() * m_scale; update();
}

int LevelCanvas::hit(QPointF point) const
{
    for (int i = int(m_document.objects.size()) - 1; i >= 0; --i) {
        const auto& object = m_document.objects[i];
        const auto path = object.path();
        QPainterPathStroker stroker; stroker.setWidth(std::max(object.width, 12 / m_scale));
        if ((object.kind != "polyline" && path.contains(point)) || stroker.createStroke(path).contains(point)) return i;
    }
    return -1;
}

void LevelCanvas::paintEvent(QPaintEvent*)
{
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing); painter.fillRect(rect(), QColor("#fafbf9"));
    if (m_document.grid) {
        double spacing = m_document.gridStep;
        while (spacing * m_scale < 8) spacing *= 10;
        const auto topLeft = world(QPointF(0, 0)), bottomRight = world(QPointF(width(), height()));
        const auto startX = qint64(std::floor(topLeft.x() / spacing));
        const auto endX = qint64(std::ceil(bottomRight.x() / spacing));
        const auto startY = qint64(std::floor(topLeft.y() / spacing));
        const auto endY = qint64(std::ceil(bottomRight.y() / spacing));
        // 每五格和十格分别加粗；缩小时整级隐藏细网格，避免巨量绘制。
        const auto gridPen = [](qint64 i) { return QPen(QColor(i % 10 == 0 ? "#b5c3c5" : i % 5 == 0 ? "#c9d3d4" : "#e6ebea"), i % 10 == 0 ? 1.5 : i % 5 == 0 ? 1.0 : .5); };
        for (qint64 x = startX; x <= endX; ++x) { painter.setPen(gridPen(x)); const double px = screen(QPointF(x*spacing, 0)).x(); painter.drawLine(QPointF(px,0),QPointF(px,height())); }
        for (qint64 y = startY; y <= endY; ++y) { painter.setPen(gridPen(y)); const double py = screen(QPointF(0,y*spacing)).y(); painter.drawLine(QPointF(0,py),QPointF(width(),py)); }
    }
    painter.save(); painter.translate(m_origin); painter.scale(m_scale, m_scale);
    for (const auto& object : m_document.objects) LevelDrawing::paintObject(painter, object);
    QPointF cursor = LevelDrawing::snapped(m_draft.points, world(m_mouse), m_document);
    if (hasDraft()) {
        auto preview = m_draft; preview.points.append(cursor);
        painter.setOpacity(.65); LevelDrawing::paintObject(painter, preview); painter.setOpacity(1);
    }
    painter.restore();
    if (m_document.crosshair && underMouse()) {
        painter.setPen(QPen(QColor("#5794ab"), 1, Qt::DashLine));
        painter.drawLine(QPointF(m_mouse.x(),0), QPointF(m_mouse.x(),height()));
        painter.drawLine(QPointF(0,m_mouse.y()), QPointF(width(),m_mouse.y()));
    }
    if (m_selected >= 0) {
        const auto& object = m_document.objects[m_selected];
        painter.setBrush(Qt::NoBrush); painter.setPen(QPen(QColor("#df3434"), 1, Qt::DashLine));
        painter.drawRect(QRectF(screen(object.points.boundingRect().topLeft()),screen(object.points.boundingRect().bottomRight())).adjusted(-5,-5,5,5));
        for (qsizetype i = 0; i < object.points.size(); ++i) {
            const auto point = screen(object.points[i]);
            if (!rect().adjusted(-8,-8,8,8).contains(point.toPoint())) continue;
            painter.setBrush(i == m_vertex ? QColor("#a60000") : QColor("#ef4040"));
            painter.setPen(QPen(Qt::white, 1)); painter.drawEllipse(point, 4.5, 4.5);
        }
    }
    if (hasDraft()) {
        painter.setPen(QPen(Qt::white, 1)); painter.setBrush(QColor("#ef4040"));
        for (const auto& point : m_draft.points) painter.drawEllipse(screen(point), 4, 4);
        const auto start = screen(m_draft.points.last()), end = screen(cursor);
        if (m_document.ruler) {
            const auto delta = end - start; const double pixels = std::hypot(delta.x(), delta.y());
            const QPointF normal = pixels > .01 ? QPointF(-delta.y(),delta.x()) / pixels : QPointF(0,-1);
            const auto a = start + normal*18, b = end + normal*18;
            painter.setPen(QPen(QColor("#33677c"),1)); painter.drawLine(a,b);
            painter.drawLine(a-normal*4,a+normal*4); painter.drawLine(b-normal*4,b+normal*4);
            QString label = LevelDrawing::length(QLineF(m_draft.points.last(),cursor).length(), m_document.unit);
            if (m_draft.points.size() > 1) {
                const auto v = m_draft.points[m_draft.points.size()-2] - m_draft.points.last();
                const auto w = cursor - m_draft.points.last();
                const double angle = std::abs(std::remainder((std::atan2(w.y(),w.x())-std::atan2(v.y(),v.x()))*180/3.141592653589793,360.));
                label += QString("  /  %1°").arg(angle,0,'f',2);
            }
            auto box = painter.fontMetrics().boundingRect(label).adjusted(-6,-4,6,4);
            box.moveCenter(((a+b)/2+normal*16).toPoint());
            painter.fillRect(box,QColor(255,255,255,235)); painter.drawText(box,Qt::AlignCenter,label);
        }
    }
}

void LevelCanvas::mousePressEvent(QMouseEvent* event)
{
    setFocus(); m_mouse = event->position();
    if (event->button() == Qt::MiddleButton) { m_panning = true; m_panStart = m_mouse; setCursor(Qt::ClosedHandCursor); return; }
    if (event->button() == Qt::RightButton) { finish(); return; }
    if (event->button() != Qt::LeftButton) return;
    const auto position = world(m_mouse);
    if (m_tool == "select") {
        m_vertex = -1;
        if (m_selected >= 0) {
            const auto& points = m_document.objects[m_selected].points;
            for (int i = 0; i < points.size(); ++i)
                if (QLineF(screen(points[i]), m_mouse).length() <= 9) { m_vertex = i; break; }
        }
        if (m_vertex < 0) select(hit(position));
        if (m_selected >= 0) { m_beforeDrag = m_document.objects[m_selected]; m_dragStart = position; m_dragging = true; }
        update(); return;
    }
    // 普通图形与预设共用同一草稿、吸附、预览及完成流程。
    if (!hasDraft()) {
        m_draft = m_style; m_draft.points.clear();
        if (!m_tool.startsWith("preset:")) { m_draft.preset.clear(); m_draft.kind = m_tool; }
    }
    const auto point = LevelDrawing::snapped(m_draft.points, position, m_document);
    if (m_draft.points.isEmpty() || QLineF(m_draft.points.last(), point).length() > .00001) m_draft.points.append(point);
    m_selected = -1; emit changed(); emit selectionChanged(); update();
}

void LevelCanvas::dragTo(QPointF position)
{
    if (!m_dragging || m_selected < 0) return;
    auto object = m_beforeDrag;
    const auto delta = position - m_dragStart;
    if (m_vertex >= 0) object.points[m_vertex] += delta;
    else object.points.translate(delta);
    m_document.objects[m_selected] = object; update();
}

void LevelCanvas::mouseMoveEvent(QMouseEvent* event)
{
    m_mouse = event->position();
    if (m_panning) { m_origin += m_mouse - m_panStart; m_panStart = m_mouse; }
    else dragTo(world(m_mouse));
    update();
}

void LevelCanvas::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton) { m_panning = false; unsetCursor(); }
    if (event->button() == Qt::LeftButton && m_dragging) {
        dragTo(world(event->position())); m_dragging = false; record(); emit selectionChanged();
    }
}

void LevelCanvas::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;
    if (m_tool != "select") { finish(); return; }
    m_dragging = false;
    if (m_selected < 0) return;
    auto object = m_document.objects[m_selected];
    if (object.kind == "points") return;
    const auto point = world(event->position());
    double best = 10 / m_scale; int insert = -1; QPointF nearest;
    const int count = int(object.points.size());
    for (int i = 0; i < count - (object.kind == "polygon" ? 0 : 1); ++i) {
        const auto a = object.points[i], b = object.points[(i+1)%count], v = b-a;
        const double squared = QPointF::dotProduct(v,v);
        if (squared < 1e-12) continue;
        const double t = std::clamp(QPointF::dotProduct(point-a,v)/squared,0.,1.);
        const auto projected = a+v*t;
        const double distance = QLineF(point,projected).length();
        if (t > .001 && t < .999 && distance < best) { best = distance; insert = i+1; nearest = projected; }
    }
    if (insert >= 0) { object.points.insert(insert,nearest); replaceSelected(object); m_vertex = insert; }
}

void LevelCanvas::wheelEvent(QWheelEvent* event)
{
    const auto anchor = world(event->position());
    m_scale = std::clamp(m_scale * std::pow(1.0015,event->angleDelta().y()),.00001,800.);
    m_origin = event->position() - anchor*m_scale; update(); event->accept();
}

void LevelCanvas::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        if (m_dragging) { m_document.objects[m_selected] = m_beforeDrag; m_dragging = false; }
        cancelDraft(); return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) { finish(); return; }
    if (event->key() == Qt::Key_Backspace && hasDraft()) { undo(); return; }
    if (event->key() == Qt::Key_Delete && m_selected >= 0) {
        if (m_vertex < 0) removeSelected();
        else {
            auto object = m_document.objects[m_selected];
            const int minimum = object.kind == "polygon" ? 3 : object.kind == "polyline" ? 2 : 1;
            if (object.points.size() <= minimum) { emit message(QStringLiteral("不能少于最小节点数；请使用“删除对象”删除整个图形。")); return; }
            object.points.removeAt(m_vertex); m_vertex = -1; replaceSelected(object);
        }
        return;
    }
    QWidget::keyPressEvent(event);
}
