#pragma once

#include <QImage>
#include <QWidget>
#include <QVector>

// 原生位图画布：鼠标和数位笔共用落笔门控与有界撤销历史。
class PaintCanvas final : public QWidget
{
    Q_OBJECT
public:
    // 可用绘图工具，橡皮擦恢复白色背景。
    enum class Tool { Brush, Eraser, Fill, Picker, Line, Rectangle, Ellipse, Text };
    // 创建默认白色画布并监听键盘释放及窗口失焦。
    explicit PaintCanvas(QWidget* parent = nullptr);
    // 读取当前作品，不包含定位光标与形状预览。
    const QImage& image() const { return m_image; }
    // 替换整个文档并清空历史，用于新建及打开图片。
    void setImage(const QImage& image);
    // 设置工具并结束当前笔画。
    void setTool(Tool tool);
    // 设置前景颜色。
    void setColor(const QColor& color);
    // 设置画笔在原图中的像素宽度。
    void setBrushWidth(int width);
    // 设置形状是否使用前景色填充。
    void setShapeFilled(bool filled);
    // 设置文字工具下一次落下的文字及像素字号。
    void setText(const QString& text, int pixels);
    // 启用需要额外按键才能落笔的数位板模式。
    void setTabletMode(bool enabled);
    // 设置落笔控制键，只接受空格、Shift 或指定字母。
    void setDrawingKey(int key);
    // 设置画布显示比例，保持原图像素不变。
    void setZoom(double zoom);
    // 返回当前显示比例。
    double zoom() const { return m_zoom; }
    // 返回作品与最近保存版本是否不同。
    bool isModified() const;
    // 记录成功保存后的版本。
    void markSaved();
    // 判断是否可以撤销或重做。
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    // 撤销或重做一次已提交笔画。
    void undo();
    void redo();
    // 清空画布，可撤销。
    void clear();
    // 停止输入并清除所有按住状态，避免失焦后继续画线。
    void resetInput();
signals:
    // 内容、历史或显示比例变化，供窗口刷新状态。
    void changed();
    // 取色器取得新的前景颜色。
    void colorPicked(const QColor& color);
    // 更新额外按键是否按住的可视反馈。
    void drawingPermissionChanged(bool allowed);
protected:
    // 绘制作品、临时形状和定位圆环。
    void paintEvent(QPaintEvent* event) override;
    // 鼠标及数位笔输入统一转换到画布坐标。
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void tabletEvent(QTabletEvent* event) override;
    // 离开画布时隐藏定位光标。
    void leaveEvent(QEvent* event) override;
    // 全局过滤本窗口的控制键与失焦事件。
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    // 更新指针并根据左键和额外按键状态开始、延续或结束笔画。
    void pointer(const QPointF& position, bool down);
    // 从当前坐标创建独立笔画，禁止连接定位期间的轨迹。
    void beginStroke();
    // 把当前笔画提交为一条撤销记录。
    void finishStroke();
    // 绘制圆头连续线段。
    void drawSegment(const QPointF& from, const QPointF& to);
    // 绘制直线、矩形或椭圆，可用于预览和正式提交。
    void drawShape(QPainter& painter) const;
    // 使用扫描线进行连续区域填色，避免递归栈溢出。
    void floodFill(const QPoint& point);
    // 存储旧版本并按总内存和数量限制裁剪历史。
    void remember(const QImage& before);
    // 当前作品、保存版本和单笔画前的快照。
    QImage m_image, m_saved, m_before;
    // 撤销和重做栈，合计不超过历史预算。
    QVector<QImage> m_undo, m_redo;
    // 当前绘图工具。
    Tool m_tool = Tool::Brush;
    // 前景色与原图像素笔宽。
    QColor m_color = QColor("#263445");
    int m_width = 5;
    // 文本内容与原图像素字号。
    QString m_text;
    int m_textPixels = 28;
    // 显示缩放比例与形状填充状态。
    double m_zoom = 1.0;
    bool m_filled = false;
    // 数位板模式、落笔键及其当前按住状态。
    bool m_tabletMode = false;
    int m_drawingKey = Qt::Key_Space;
    bool m_keyHeld = false;
    // 指针接触、笔画活动、光标可见状态。
    bool m_down = false, m_stroking = false, m_inside = false;
    // 最新位置、当前笔画起点与上一绘制点，单位为原图像素。
    QPointF m_position, m_start, m_last;
};
