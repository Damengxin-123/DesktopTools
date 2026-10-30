#pragma once

#include <QImage>
#include <QWidget>

class QLabel;
class QPushButton;

// 资源浏览器的图像窗口：深色画布上提供滚轮缩放、拖动平移、90° 旋转，
// 并可在同一目录的图片间切换。窗口关闭时自毁（WA_DeleteOnClose），
// 调用方创建后只需 show，不需要管理生命周期。
class ImageBrowserWindow final : public QWidget
{
    Q_OBJECT
public:
    // 显示一组图片文件；index 为初始显示项，越界时取第一张。
    void setFiles(const QStringList& paths, int index);
    // 显示一张内存图片（软件内截图等），不提供同目录切换。
    void setStandaloneImage(const QImage& image, const QString& title);

    // 便捷打开：创建并加载首张图片，失败时弹窗说明并返回 nullptr。
    static ImageBrowserWindow* open(const QStringList& paths, int index, QWidget* parent);
    // 便捷打开：显示一张内存图片（软件内截图等）；图片为空时返回 nullptr。
    static ImageBrowserWindow* openImage(const QImage& image, const QString& title, QWidget* parent);

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    // 画布：负责图像绘制、滚轮缩放与拖动平移。
    class Canvas;
    explicit ImageBrowserWindow(QWidget* parent);

    void showIndex(int index);
    bool loadCurrent();
    void updateHeader();
    void rotate(int degrees);
    void stepZoom(int direction);

    // 同目录可切换的图片列表；为空表示独立的内存图片。
    QStringList m_paths;
    int m_index = 0;
    // 当前已解码图像；为空表示加载失败。
    QImage m_image;
    // 内存图片的显示标题。
    QString m_title;
    Canvas* m_canvas;
    QLabel* m_nameLabel;
    QLabel* m_sizeLabel;
    QLabel* m_zoomLabel;
    QPushButton* m_previous;
    QPushButton* m_next;
    QPushButton* m_copyButton;
};
