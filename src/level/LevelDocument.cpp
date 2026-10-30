#include "LevelDocument.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <QXmlStreamWriter>
#include <cmath>
#include <algorithm>

namespace {
// 格式化长度及 SVG 数值为两位小数。
QString number(double value) { return QString::number(value, 'f', 2); }
// 拒绝非有限或超出合理绘图范围的数值。
bool finite(double value, double low, double high) { return std::isfinite(value) && value >= low && value <= high; }
// 收集图例中实际使用的预设。
QList<LevelObject> legendItems(const LevelDocument& document)
{
    QList<LevelObject> result;
    QSet<QString> seen;
    for (const auto& object : document.objects) {
        if (!object.preset.isEmpty() && !seen.contains(object.preset)) {
            seen.insert(object.preset);
            result.append(object);
        }
    }
    return result;
}
// 生成绿化纹样，尺寸固定为米，画布与导出共享同一贴图。
QImage vegetationTile()
{
    QImage image(80, 80, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor("#3e7952"), 2));
    painter.setBrush(QColor(255, 255, 255, 55));
    painter.drawEllipse(QRectF(12, 10, 24, 32));
    painter.drawLine(QPointF(24, 38), QPointF(24, 20));
    painter.drawEllipse(QRectF(48, 48, 20, 22));
    return image;
}
}

QJsonObject LevelObject::json() const
{
    QJsonArray vertices;
    for (const auto& point : points) vertices.append(QJsonArray{point.x(), point.y()});
    return {{"id", id}, {"name", name}, {"kind", kind}, {"preset", preset}, {"points", vertices},
            {"fill", fill.name(QColor::HexArgb)}, {"stroke", stroke.name(QColor::HexArgb)},
            {"width", width}, {"diameter", diameter}};
}

QPainterPath LevelObject::path() const
{
    QPainterPath result;
    if (kind == "points") {
        for (const auto& point : points) result.addEllipse(point, diameter / 2, diameter / 2);
    } else if (!points.isEmpty()) {
        result.moveTo(points.first());
        for (qsizetype i = 1; i < points.size(); ++i) result.lineTo(points[i]);
        if (kind == "polygon") result.closeSubpath();
    }
    return result;
}

QJsonObject LevelDocument::json() const
{
    QJsonArray entries;
    for (const auto& object : objects) entries.append(object.json());
    return {{"format", "DesktopTool.LevelLayout"}, {"version", 1}, {"title", title}, {"objects", entries},
            {"settings", QJsonObject{{"grid", grid}, {"angles", angles}, {"quantize", quantize},
             {"ruler", ruler}, {"crosshair", crosshair}, {"angleStep", angleStep}, {"gridStep", gridStep}, {"unit", unit}}}};
}

bool LevelDocument::parse(const QJsonObject& value, LevelDocument& result, QString& error)
{
    error = QStringLiteral("关卡项目格式不正确、数据损坏或版本不受支持。");
    if (value["format"] != "DesktopTool.LevelLayout" || value["version"].toInt() != 1
        || !value["objects"].isArray() || !value["settings"].isObject()) return false;
    LevelDocument candidate;
    candidate.title = value["title"].toString().trimmed();
    if (candidate.title.isEmpty()) return false;
    QSet<QString> ids;
    for (const auto& entry : value["objects"].toArray()) {
        if (!entry.isObject()) return false;
        const auto data = entry.toObject();
        LevelObject object;
        object.id = data["id"].toString();
        object.name = data["name"].toString();
        object.kind = data["kind"].toString();
        object.preset = data["preset"].toString();
        object.fill = QColor(data["fill"].toString());
        object.stroke = QColor(data["stroke"].toString());
        object.width = data["width"].toDouble(-1);
        object.diameter = data["diameter"].toDouble(-1);
        if (object.id.isEmpty() || ids.contains(object.id) || !object.fill.isValid() || !object.stroke.isValid()
            || !finite(object.width, 0.01, 1000) || !finite(object.diameter, 0.01, 1000)
            || !QStringList{"points", "polyline", "polygon"}.contains(object.kind) || !data["points"].isArray()) return false;
        ids.insert(object.id);
        for (const auto& vertex : data["points"].toArray()) {
            const auto xy = vertex.toArray();
            if (xy.size() != 2 || !xy[0].isDouble() || !xy[1].isDouble()
                || !finite(xy[0].toDouble(), -1e7, 1e7) || !finite(xy[1].toDouble(), -1e7, 1e7)) return false;
            object.points.append(QPointF(xy[0].toDouble(), xy[1].toDouble()));
        }
        const int minimum = object.kind == "polygon" ? 3 : object.kind == "polyline" ? 2 : 1;
        if (object.points.size() < minimum) return false;
        candidate.objects.append(object);
    }
    const auto settings = value["settings"].toObject();
    candidate.grid = settings["grid"].toBool(true);
    candidate.angles = settings["angles"].toBool();
    candidate.quantize = settings["quantize"].toBool();
    candidate.ruler = settings["ruler"].toBool(true);
    candidate.crosshair = settings["crosshair"].toBool();
    candidate.angleStep = settings["angleStep"].toDouble(10);
    candidate.gridStep = settings["gridStep"].toDouble(1);
    candidate.unit = settings["unit"].toString("m");
    if (!finite(candidate.angleStep, 0.01, 180) || !finite(candidate.gridStep, 0.01, 10000)
        || !QStringList{"m", "cm", "km"}.contains(candidate.unit)) return false;
    result = candidate;
    error.clear();
    return true;
}

bool LevelDocument::read(const QString& path, LevelDocument& result, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); return false; }
    QJsonParseError parseError;
    const auto data = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !data.isObject()) {
        error = QStringLiteral("无法解析关卡项目：") + parseError.errorString(); return false;
    }
    return parse(data.object(), result, error);
}

bool LevelDocument::write(const QString& path, QString& error) const
{
    LevelDocument checked;
    if (!parse(json(), checked, error)) return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) { error = file.errorString(); return false; }
    const auto data = QJsonDocument(json()).toJson();
    if (file.write(data) != data.size() || !file.commit()) { error = file.errorString(); return false; }
    return true;
}

QVariantList LevelDocument::catalog(const QString& dataRoot)
{
    QVariantList items;
    const QDir directory(QDir(dataRoot).filePath("level-layouts"));
    for (const auto& file : directory.entryInfoList({"*.level.json"}, QDir::Files, QDir::Time)) {
        LevelDocument document;
        QString error;
        if (read(file.absoluteFilePath(), document, error))
            items.append(QVariantMap{{"id", file.fileName()}, {"title", document.title},
                         {"objects", document.objects.size()}, {"updated", file.lastModified().toString(Qt::ISODate)}});
    }
    return items;
}

QRectF LevelDocument::bounds() const
{
    QRectF result;
    bool first = true;
    for (const auto& object : objects) {
        const double margin = std::max(object.width, object.diameter) / 2;
        const auto rect = object.points.boundingRect().adjusted(-margin, -margin, margin, margin);
        result = first ? rect : result.united(rect);
        first = false;
    }
    return first ? QRectF(0, 0, 50, 30) : result.adjusted(-2, -2, 2, 2);
}

QPointF LevelDrawing::snapped(const QPolygonF& points, QPointF target, const LevelDocument& document)
{
    if (points.isEmpty() || (!document.angles && !document.quantize)) return target;
    const QPointF origin = points.last();
    const QPointF delta = target - origin;
    const double radius = std::hypot(delta.x(), delta.y());
    constexpr double radians = 3.14159265358979323846 / 180;
    // 基准为从当前顶点指向上一顶点的反向射线，因此夹角确实位于点两侧。
    const auto previous = points.size() > 1 ? points[points.size() - 2] - origin : QPointF(1, 0);
    const double base = std::atan2(previous.y(), previous.x());
    const double relative = std::remainder((std::atan2(delta.y(), delta.x()) - base) / radians, 360.0);
    double angle = std::abs(relative);
    if (document.quantize) {
        const double maxMultiple = std::floor(180.0 / document.angleStep);
        angle = std::min(maxMultiple, std::round(angle / document.angleStep)) * document.angleStep;
    } else {
        double closest = angle;
        double distance = 4.0;
        for (double common : {0., 30., 45., 60., 90., 120., 135., 150., 180.}) {
            if (std::abs(angle - common) <= distance) { distance = std::abs(angle - common); closest = common; }
        }
        angle = closest;
    }
    const double direction = base + std::copysign(angle, relative) * radians;
    return origin + QPointF(std::cos(direction), std::sin(direction)) * radius;
}

LevelObject LevelDrawing::presetStyle(const QString& key)
{
    LevelObject object;
    object.preset = key;
    if (key == QStringLiteral("道路")) {
        object.kind = "polyline"; object.stroke = QColor("#a8acb1"); object.width = 4.00;
    }
    if (key == QStringLiteral("家具")) { object.fill = QColor("#c9a477"); object.stroke = QColor("#70533a"); object.width = 0.10; }
    if (key == QStringLiteral("水域")) { object.fill = QColor("#93d4e9"); object.stroke = QColor("#4798b3"); }
    if (key == QStringLiteral("建筑")) { object.fill = QColor("#dedbd0"); object.stroke = QColor("#86847b"); }
    return object;
}

LevelObject LevelDrawing::preset(const QString& key, QPointF center)
{
    LevelObject object = presetStyle(key);
    object.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    object.name = key;
    if (object.kind == "polyline") {
        object.points = QPolygonF{center + QPointF(-6, 0), center + QPointF(6, 0)};
        return object;
    }
    const QSizeF size = key == QStringLiteral("家具") ? QSizeF(2.00, 0.80) : QSizeF(8.00, 5.00);
    const double x = size.width() / 2, y = size.height() / 2;
    object.points = QPolygonF{center + QPointF(-x,-y), center + QPointF(x,-y), center + QPointF(x,y), center + QPointF(-x,y)};
    return object;
}

void LevelDrawing::paintObject(QPainter& painter, const LevelObject& object)
{
    painter.save();
    const auto path = object.path();
    painter.setPen(QPen(object.stroke, object.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(object.kind == "polyline" ? QBrush(Qt::NoBrush) : QBrush(object.fill));
    painter.drawPath(path);
    if (object.preset == QStringLiteral("绿化") && object.kind == "polygon") {
        static const QImage tile = vegetationTile();
        QBrush pattern(tile);
        pattern.setTransform(QTransform::fromScale(0.025, 0.025));
        painter.fillPath(path, pattern);
    } else if (object.preset == QStringLiteral("道路") && object.kind == "polyline") {
        QPen center(QColor("#fff4c7"), std::min(0.12, object.width / 5), Qt::DashLine, Qt::RoundCap);
        center.setDashPattern({8, 6});
        painter.setPen(center); painter.drawPath(path);
    } else if (object.preset == QStringLiteral("家具") && object.kind == "polygon") {
        painter.setClipPath(path);
        painter.setPen(QPen(QColor("#8c6948"), 0.04));
        const auto box = path.boundingRect();
        // 家具木板最多绘制可见的合理纹样密度，不影响原始节点数量。
        const double spacing = std::max(0.25, box.height() / 1000);
        for (double y = box.top() + spacing; y < box.bottom(); y += spacing)
            painter.drawLine(QPointF(box.left(), y), QPointF(box.right(), y));
    }
    painter.restore();
}

QString LevelDrawing::length(double meters, const QString& unit)
{
    return number(unit == "cm" ? meters * 100 : unit == "km" ? meters / 1000 : meters)
        + (unit == "cm" ? QStringLiteral(" 厘米") : unit == "km" ? QStringLiteral(" 千米") : QStringLiteral(" 米"));
}

bool LevelDrawing::exportFile(const LevelDocument& document, const QString& path, bool svg,
                             bool presetNames, bool legend, int pixelWidth, QString& error)
{
    const auto items = legend ? legendItems(document) : QList<LevelObject>{};
    const auto bounds = document.bounds();
    const double scale = std::clamp(pixelWidth, 256, 8192) / bounds.width();
    const int width = std::clamp(pixelWidth, 256, 8192);
    const double drawingHeight = std::max(1., std::ceil(bounds.height() * scale));
    const int legendHeight = items.isEmpty() ? 0 : 40 + int(items.size()) * 28;
    if (!svg && (drawingHeight + legendHeight > 16384 || width * (drawingHeight + legendHeight) > 64000000)) {
        error = QStringLiteral("图片尺寸过大，请减小导出宽度或改用 SVG。"); return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) { error = file.errorString(); return false; }
    if (!svg) {
        QImage image(width, int(drawingHeight) + legendHeight, QImage::Format_ARGB32_Premultiplied);
        if (image.isNull()) { error = QStringLiteral("无法分配图片内存。"); return false; }
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(scale, scale); painter.translate(-bounds.topLeft());
        for (const auto& object : document.objects) paintObject(painter, object);
        painter.resetTransform();
        QFont font; font.setPixelSize(16); painter.setFont(font);
        int y = int(drawingHeight) + 28;
        if (!items.isEmpty()) { painter.setPen(Qt::black); painter.drawText(16, y, QStringLiteral("图例")); }
        for (const auto& object : items) {
            y += 28;
            painter.fillRect(QRect(16, y - 16, 20, 16), object.kind == "polyline" ? object.stroke : object.fill);
            painter.setPen(Qt::black); painter.drawText(48, y - 2, object.preset);
        }
        painter.end();
        if (!image.save(&file, "PNG")) { error = QStringLiteral("PNG 编码失败。"); return false; }
    } else {
        QXmlStreamWriter xml(&file);
        xml.setAutoFormatting(true); xml.writeStartDocument();
        xml.writeStartElement("svg"); xml.writeDefaultNamespace("http://www.w3.org/2000/svg");
        xml.writeAttribute("width", QString::number(width));
        xml.writeAttribute("height", number(drawingHeight + legendHeight));
        xml.writeAttribute("viewBox", QString("0 0 %1 %2").arg(width).arg(number(drawingHeight + legendHeight)));
        xml.writeTextElement("title", document.title);
        xml.writeTextElement("metadata", QString::fromUtf8(QJsonDocument(document.json()).toJson(QJsonDocument::Compact)));
        xml.writeStartElement("defs");
        xml.writeStartElement("pattern"); xml.writeAttribute("id", "vegetation");
        xml.writeAttribute("patternUnits", "userSpaceOnUse"); xml.writeAttribute("width", "2"); xml.writeAttribute("height", "2");
        xml.writeEmptyElement("path"); xml.writeAttribute("d", "M .6 .95 C -.1 .3 .9 -.1 .9 .55 C .9 .9 .6 .95 .6 .95 M .6 .95 L .6 .5 M 1.45 1.75 C 1 1.1 1.8 1 1.7 1.5 Z");
        xml.writeAttribute("fill", "#ffffff"); xml.writeAttribute("fill-opacity", ".22");
        xml.writeAttribute("stroke", "#3e7952"); xml.writeAttribute("stroke-width", ".05");
        xml.writeEndElement();
        xml.writeStartElement("pattern"); xml.writeAttribute("id", "wood"); xml.writeAttribute("patternUnits", "userSpaceOnUse");
        xml.writeAttribute("width", "1"); xml.writeAttribute("height", ".25");
        xml.writeEmptyElement("path"); xml.writeAttribute("d", "M 0 0 H 1");
        xml.writeAttribute("stroke", "#8c6948"); xml.writeAttribute("stroke-width", ".04");
        xml.writeEndElement(); xml.writeEndElement();
        xml.writeEmptyElement("rect"); xml.writeAttribute("width", "100%"); xml.writeAttribute("height", "100%"); xml.writeAttribute("fill", "white");
        xml.writeStartElement("g");
        xml.writeAttribute("transform", QString("scale(%1) translate(%2 %3)").arg(scale, 0, 'g', 12).arg(-bounds.x(), 0, 'g', 12).arg(-bounds.y(), 0, 'g', 12));
        for (const auto& object : document.objects) {
            xml.writeStartElement("g"); xml.writeAttribute("id", "object-" + object.id);
            xml.writeAttribute("data-name", presetNames && !object.preset.isEmpty() ? object.preset : object.name);
            xml.writeAttribute("data-preset", object.preset); xml.writeAttribute("data-unit", "m");
            xml.writeTextElement("title", presetNames && !object.preset.isEmpty() ? object.preset : object.name);
            QString d;
            if (object.kind != "points") {
                for (qsizetype i = 0; i < object.points.size(); ++i)
                    d += QString("%1 %2 %3 ").arg(i == 0 ? "M" : "L", number(object.points[i].x()), number(object.points[i].y()));
                if (object.kind == "polygon") d += "Z";
            }
            // 将共享颜色、透明度及轮廓写入每个 SVG 几何元素。
            const auto style = [&]() {
                xml.writeAttribute("fill", object.kind == "polyline" ? "none" : object.fill.name());
                xml.writeAttribute("fill-opacity", number(object.fill.alphaF()));
                xml.writeAttribute("stroke", object.stroke.name()); xml.writeAttribute("stroke-opacity", number(object.stroke.alphaF()));
                xml.writeAttribute("stroke-width", number(object.width)); xml.writeAttribute("stroke-linejoin", "round"); xml.writeAttribute("stroke-linecap", "round");
            };
            if (object.kind == "points") {
                for (const auto& point : object.points) {
                    xml.writeEmptyElement("circle"); xml.writeAttribute("cx", number(point.x())); xml.writeAttribute("cy", number(point.y()));
                    xml.writeAttribute("r", QString::number(object.diameter / 2, 'f', 3)); style();
                }
            } else {
                xml.writeEmptyElement("path"); xml.writeAttribute("d", d); style();
                if (object.kind == "polygon" && (object.preset == QStringLiteral("绿化") || object.preset == QStringLiteral("家具"))) {
                    xml.writeEmptyElement("path"); xml.writeAttribute("d", d);
                    xml.writeAttribute("fill", object.preset == QStringLiteral("绿化") ? "url(#vegetation)" : "url(#wood)");
                }
                if (object.kind == "polyline" && object.preset == QStringLiteral("道路")) {
                    xml.writeEmptyElement("path"); xml.writeAttribute("d", d); xml.writeAttribute("fill", "none");
                    xml.writeAttribute("stroke", "#fff4c7"); xml.writeAttribute("stroke-width", QString::number(std::min(.12, object.width / 5), 'g', 8));
                    xml.writeAttribute("stroke-dasharray", QString("%1 %2").arg(std::min(.12, object.width / 5) * 8).arg(std::min(.12, object.width / 5) * 6));
                }
            }
            xml.writeEndElement();
        }
        xml.writeEndElement();
        double y = drawingHeight + 28;
        for (const auto& object : items) {
            xml.writeEmptyElement("rect"); xml.writeAttribute("x", "16"); xml.writeAttribute("y", number(y - 16));
            xml.writeAttribute("width", "20"); xml.writeAttribute("height", "16");
            xml.writeAttribute("fill", (object.kind == "polyline" ? object.stroke : object.fill).name());
            xml.writeStartElement("text"); xml.writeAttribute("x", "48"); xml.writeAttribute("y", number(y));
            xml.writeAttribute("font-size", "16"); xml.writeCharacters(object.preset); xml.writeEndElement(); y += 28;
        }
        xml.writeEndElement(); xml.writeEndDocument();
        if (xml.hasError()) { error = QStringLiteral("SVG 写入失败。"); return false; }
    }
    if (!file.commit()) { error = file.errorString(); return false; }
    return true;
}
