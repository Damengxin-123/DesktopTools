#pragma once
#include <QColor>
#include <QJsonObject>
#include <QPainterPath>
#include <QPolygonF>
#include <QVariantList>

class QPainter;

// 一个可独立编辑的矢量对象，所有几何尺寸均使用米。
struct LevelObject {
    QString id; // 永久唯一标识。
    QString name; // 用户定义的对象名称。
    QString kind = "polygon"; // 几何类型：点集、折线或多边形。
    QString preset; // 预设语义：绿化、道路、家具、水域、建筑。
    QPolygonF points; // 不限制节点数量的有序坐标。
    QColor fill = QColor("#a9d4bb"); // 面内部或点的颜色。
    QColor stroke = QColor("#355b50"); // 轮廓颜色。
    double width = 0.20; // 线宽，单位米。
    double diameter = 0.80; // 点直径，单位米。
    // 转换为持久化 JSON。
    QJsonObject json() const;
    // 返回可用于填充和命中测试的路径。
    QPainterPath path() const;
};

// 项目及绘图辅助设置；视口位置不影响正式几何数据。
struct LevelDocument {
    QString title = QStringLiteral("未命名关卡"); // 项目名称。
    QList<LevelObject> objects; // 按绘制顺序保存的对象。
    bool grid = true; // 显示分级网格。
    bool angles = false; // 吸附常用夹角。
    bool quantize = false; // 强制夹角为最小单位的倍数。
    bool ruler = true; // 显示当前线段标尺。
    bool crosshair = false; // 显示整个视口的十字对齐线。
    double angleStep = 10.00; // 最小夹角单位，单位度。
    double gridStep = 1.00; // 基本网格间距，单位米。
    QString unit = "m"; // 标尺显示单位，内部仍使用米。
    // 编码全部对象与设置。
    QJsonObject json() const;
    // 严格验证后解析，失败时不改变当前文档。
    static bool parse(const QJsonObject& value, LevelDocument& result, QString& error);
    // 读取工程文件。
    static bool read(const QString& path, LevelDocument& result, QString& error);
    // 原子写入工程文件，避免不完整内容覆盖已有项目。
    bool write(const QString& path, QString& error) const;
    // 列出数据目录内的所有有效项目。
    static QVariantList catalog(const QString& dataRoot);
    // 返回用于导出的完整图形边界。
    QRectF bounds() const;
};

// 统一几何、预设和导出逻辑，避免编辑视口与导出结果不一致。
namespace LevelDrawing {
// 将鼠标点吸附到前一个顶点两侧的夹角，第一段以水平轴为基准。
QPointF snapped(const QPolygonF& points, QPointF target, const LevelDocument& document);
// 创建仅含绘制类型、颜色、粗细及纹样语义的预设样式，不预置节点。
LevelObject presetStyle(const QString& key);
// 为示例和测试创建默认形状；编辑器使用预设样式逐点绘制。
LevelObject preset(const QString& key, QPointF center);
// 绘制单个对象及其程序化纹样。
void paintObject(QPainter& painter, const LevelObject& object);
// 导出带可选图例和预设语义的 PNG 或 SVG。
bool exportFile(const LevelDocument& document, const QString& path, bool svg,
                bool presetNames, bool legend, int pixelWidth, QString& error);
// 显示保留两位小数的长度。
QString length(double meters, const QString& unit);
}
