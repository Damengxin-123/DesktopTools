#include "InterfaceLanguage.h"
#include "ImageBrowserWindow.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QTransform>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace
{
// 缩放边界与单步倍率。
constexpr qreal kMinimumZoom = 0.02;
constexpr qreal kMaximumZoom = 40.0;
constexpr qreal kZoomStep = 1.25;
}

// 图像画布：维持“视口中心 + 偏移 → 旋转 → 缩放”的绘制变换，
// 滚轮缩放以光标为锚点，左键拖动平移，双击在适应窗口与 1:1 间切换。
class ImageBrowserWindow::Canvas final : public QWidget
{
    Q_OBJECT
public:
    explicit Canvas(QWidget* parent)
        : QWidget(parent)
    {
        setMouseTracking(true);
        setCursor(Qt::OpenHandCursor);
    }

    void setImage(const QImage& image)
    {
        m_image = image;
        resetView();
    }

    bool hasImage() const { return !m_image.isNull(); }
    QSize imageSize() const { return m_image.size(); }
    qreal factor() const { return m_factor; }
    int rotation() const { return m_rotation; }

    void resetView()
    {
        m_rotation = 0;
        fitView();
    }

    void fitView()
    {
        const QSizeF rotated = rotatedSize();
        if (rotated.isEmpty()) {
            m_factor = 1.0;
            m_offset = QPointF();
        } else {
            // 视口四周各留 16 px 边距；小图放大到铺满，与常见看图工具一致。
            m_factor = qMax(kMinimumZoom,
                qMin((width() - 32.0) / rotated.width(), (height() - 32.0) / rotated.height()));
            m_offset = QPointF(0, 0);
        }
        m_fit = true;
        m_panning = false;
        update();
        emit zoomChanged();
    }

    void setFactor(qreal factor) { applyZoom(factor / m_factor, rect().center()); }

    void setRotation(int degrees)
    {
        m_rotation = ((degrees % 360) + 360) % 360;
        if (m_fit)
            fitView();
        else
            update();
        emit zoomChanged();
    }

    void panBy(const QPointF& delta)
    {
        if (m_image.isNull())
            return;
        m_offset += delta;
        m_fit = false;
        update();
    }

signals:
    // 缩放比例或适应状态变化；窗口据此刷新百分比与按钮文字。
    void zoomChanged();

protected:
    void paintEvent(QPaintEvent* event) override
    {
        Q_UNUSED(event)
        QPainter painter(this);
        painter.fillRect(rect(), QColor(0x1e, 0x20, 0x26));
        if (m_image.isNull())
            return;
        painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
        painter.save();
        painter.translate(width() / 2.0 + m_offset.x(), height() / 2.0 + m_offset.y());
        painter.rotate(m_rotation);
        painter.scale(m_factor, m_factor);
        painter.drawImage(QRectF(-m_image.width() / 2.0, -m_image.height() / 2.0,
            m_image.width(), m_image.height()), m_image);
        painter.restore();
    }

    void wheelEvent(QWheelEvent* event) override
    {
        // 每 15° 滚轮一格按指数缩放，保证高速滚动时手感一致。
        const qreal delta = event->angleDelta().y();
        if (delta == 0)
            return;
        applyZoom(qPow(1.0015, delta), event->position());
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton || m_image.isNull())
            return;
        m_panning = true;
        m_panStart = event->position();
        setCursor(Qt::ClosedHandCursor);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!m_panning)
            return;
        const QPointF delta = event->position() - m_panStart;
        m_panStart = event->position();
        panBy(delta);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton)
            return;
        m_panning = false;
        setCursor(Qt::OpenHandCursor);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        Q_UNUSED(event)
        if (m_image.isNull())
            return;
        if (m_fit)
            applyZoom(1.0 / m_factor, rect().center());
        else
            fitView();
    }

    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        // 适应模式下跟随窗口重排；自由缩放时保持当前视野。
        if (m_fit)
            fitView();
        else
            update();
    }

private:
    // 以 anchor（视口坐标）为锚点按倍率缩放：锚点下的图像点保持不动。
    void applyZoom(qreal ratio, const QPointF& anchor)
    {
        if (m_image.isNull())
            return;
        const qreal next = qBound(kMinimumZoom, m_factor * ratio, kMaximumZoom);
        if (qFuzzyCompare(next, m_factor))
            return;
        const QTransform base = viewportTransform();
        const QPointF imagePoint = base.inverted().map(anchor);
        const QPointF previousOffset = m_offset;
        m_factor = next;
        m_fit = false;
        m_offset = anchor - base.map(imagePoint) + previousOffset;
        update();
        emit zoomChanged();
    }

    // 完整绘制变换：视口中心 + 偏移 → 旋转 → 缩放 → 图像中心对齐。
    QTransform viewportTransform() const
    {
        QTransform transform;
        transform.translate(width() / 2.0 + m_offset.x(), height() / 2.0 + m_offset.y());
        transform.rotate(m_rotation);
        transform.scale(m_factor, m_factor);
        transform.translate(-m_image.width() / 2.0, -m_image.height() / 2.0);
        return transform;
    }

    QSizeF rotatedSize() const
    {
        const QSizeF size(m_image.width(), m_image.height());
        return m_rotation % 180 == 0 ? size : size.transposed();
    }

    QImage m_image;
    qreal m_factor = 1.0;
    int m_rotation = 0;
    QPointF m_offset;
    bool m_fit = true;
    bool m_panning = false;
    QPointF m_panStart;
};

ImageBrowserWindow::ImageBrowserWindow(QWidget* parent)
    : QWidget(parent)
{
    // 不依赖父窗口：主窗口最小化时保持独立显示；打开后始终置顶。
    setWindowFlag(Qt::Window);
    setWindowFlag(Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_DeleteOnClose);
    InterfaceLanguage::bind(this, "windowTitle", QStringLiteral("图像浏览器"));
    resize(1080, 720);
    setMinimumSize(560, 420);
    setStyleSheet(QStringLiteral(
        "#imageToolbar{background:#26282e;border-top:1px solid #14151a;}"
        "#imageToolbar QLabel{color:#c8ccd4;font-size:12px;background:transparent;}"
        "#imageToolbar QPushButton{background:transparent;border:1px solid #3a3d46;border-radius:6px;"
        "color:#d5d8df;padding:6px 12px;font-size:12px;}"
        "#imageToolbar QPushButton:hover{background:#33363e;border-color:#4a4e59;}"
        "#imageToolbar QPushButton:disabled{color:#5d626d;border-color:#2c2f36;}"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_canvas = new Canvas(this);
    layout->addWidget(m_canvas, 1);

    auto* toolbar = new QFrame(this);
    toolbar->setObjectName(QStringLiteral("imageToolbar"));
    auto* bar = new QHBoxLayout(toolbar);
    bar->setContentsMargins(10, 8, 10, 8);
    bar->setSpacing(8);
    m_previous = new QPushButton(InterfaceLanguage::text(QStringLiteral("◀ 上一张")), toolbar);
    InterfaceLanguage::bind(m_previous, "text", QStringLiteral("◀ 上一张"));
    m_next = new QPushButton(InterfaceLanguage::text(QStringLiteral("下一张 ▶")), toolbar);
    InterfaceLanguage::bind(m_next, "text", QStringLiteral("下一张 ▶"));
    auto* zoomOut = new QPushButton(QStringLiteral("－"), toolbar);
    InterfaceLanguage::bind(zoomOut, "toolTip", QStringLiteral("缩小（-）"));
    auto* zoomIn = new QPushButton(QStringLiteral("＋"), toolbar);
    InterfaceLanguage::bind(zoomIn, "toolTip", QStringLiteral("放大（+/=）"));
    m_zoomLabel = new QLabel(QStringLiteral("100%"), toolbar);
    m_zoomLabel->setMinimumWidth(46);
    m_zoomLabel->setAlignment(Qt::AlignCenter);
    auto* fitButton = new QPushButton(InterfaceLanguage::text(QStringLiteral("适应窗口")), toolbar);
    InterfaceLanguage::bind(fitButton, "text", QStringLiteral("适应窗口"));
    InterfaceLanguage::bind(fitButton, "toolTip", QStringLiteral("适应窗口（0/F）"));
    auto* actualButton = new QPushButton(QStringLiteral("1:1"), toolbar);
    InterfaceLanguage::bind(actualButton, "toolTip", QStringLiteral("原始大小（1）"));
    auto* rotateLeft = new QPushButton(InterfaceLanguage::text(QStringLiteral("↺ 左旋")), toolbar);
    InterfaceLanguage::bind(rotateLeft, "text", QStringLiteral("↺ 左旋"));
    auto* rotateRight = new QPushButton(InterfaceLanguage::text(QStringLiteral("↪ 右旋")), toolbar);
    InterfaceLanguage::bind(rotateRight, "text", QStringLiteral("↪ 右旋"));
    m_sizeLabel = new QLabel(toolbar);
    m_nameLabel = new QLabel(toolbar);
    m_nameLabel->setMaximumWidth(280);
    m_copyButton = new QPushButton(InterfaceLanguage::text(QStringLiteral("复制图片")), toolbar);
    InterfaceLanguage::bind(m_copyButton, "text", QStringLiteral("复制图片"));
    bar->addWidget(m_previous);
    bar->addWidget(m_next);
    bar->addSpacing(8);
    bar->addWidget(zoomOut);
    bar->addWidget(m_zoomLabel);
    bar->addWidget(zoomIn);
    bar->addWidget(fitButton);
    bar->addWidget(actualButton);
    bar->addSpacing(8);
    bar->addWidget(rotateLeft);
    bar->addWidget(rotateRight);
    bar->addStretch(1);
    bar->addWidget(m_sizeLabel);
    bar->addWidget(m_nameLabel);
    bar->addWidget(m_copyButton);
    layout->addWidget(toolbar);

    connect(m_previous, &QPushButton::clicked, this, [this]() { showIndex(m_index - 1); });
    connect(m_next, &QPushButton::clicked, this, [this]() { showIndex(m_index + 1); });
    connect(zoomOut, &QPushButton::clicked, this, [this]() { stepZoom(-1); });
    connect(zoomIn, &QPushButton::clicked, this, [this]() { stepZoom(1); });
    connect(fitButton, &QPushButton::clicked, m_canvas, &Canvas::fitView);
    connect(actualButton, &QPushButton::clicked, this, [this]() { m_canvas->setFactor(1.0); });
    connect(rotateLeft, &QPushButton::clicked, this, [this]() { rotate(-90); });
    connect(rotateRight, &QPushButton::clicked, this, [this]() { rotate(90); });
    connect(m_copyButton, &QPushButton::clicked, this, [this]() {
        if (!m_image.isNull())
            QApplication::clipboard()->setImage(m_image);
    });
    connect(m_canvas, &Canvas::zoomChanged, this, [this]() {
        m_zoomLabel->setText(QStringLiteral("%1%").arg(qRound(m_canvas->factor() * 100)));
    });
    updateHeader();
}

void ImageBrowserWindow::setFiles(const QStringList& paths, int index)
{
    m_paths = paths;
    showIndex(index);
}

void ImageBrowserWindow::setStandaloneImage(const QImage& image, const QString& title)
{
    m_title = title;
    m_image = image;
    m_canvas->setImage(image);
    updateHeader();
}

ImageBrowserWindow* ImageBrowserWindow::open(const QStringList& paths, int index, QWidget* parent)
{
    if (paths.isEmpty())
        return nullptr;
    // 独立顶级窗口：与主窗口解耦，parent 仅用于打开失败时的提示弹窗。
    Q_UNUSED(parent)
    auto* window = new ImageBrowserWindow(nullptr);
    window->setFiles(paths, index);
    if (!window->m_canvas->hasImage()) {
        delete window;
        return nullptr;
    }
    window->show();
    window->raise();
    window->activateWindow();
    return window;
}

ImageBrowserWindow* ImageBrowserWindow::openImage(const QImage& image, const QString& title, QWidget* parent)
{
    if (image.isNull())
        return nullptr;
    // 独立顶级窗口：与主窗口解耦。
    Q_UNUSED(parent)
    auto* window = new ImageBrowserWindow(nullptr);
    window->setStandaloneImage(image, title);
    window->show();
    window->raise();
    window->activateWindow();
    return window;
}

void ImageBrowserWindow::showIndex(int index)
{
    if (m_paths.isEmpty())
        return;
    m_index = qBound(0, index, m_paths.size() - 1);
    loadCurrent();
    m_canvas->setImage(m_image);
    updateHeader();
}

bool ImageBrowserWindow::loadCurrent()
{
    if (m_paths.isEmpty())
        return !m_image.isNull();
    QImageReader reader(m_paths.at(m_index));
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
    m_image = reader.read();
    if (m_image.isNull())
        QMessageBox::warning(this, InterfaceLanguage::text(QStringLiteral("图像浏览器")),
            InterfaceLanguage::text(QStringLiteral("无法读取图片：%1\n%2")).arg(m_paths.at(m_index), reader.errorString()));
    return !m_image.isNull();
}

void ImageBrowserWindow::updateHeader()
{
    const bool browse = !m_paths.isEmpty();
    const QString name = browse ? QFileInfo(m_paths.at(m_index)).fileName() : m_title;
    InterfaceLanguage::bind(this, "windowTitle", QStringLiteral("图像浏览器 — %1").arg(name));
    m_nameLabel->setText(name);
    m_nameLabel->setToolTip(browse ? m_paths.at(m_index) : QString());
    if (m_image.isNull()) {
        InterfaceLanguage::bind(m_sizeLabel, "text", QStringLiteral("无法加载"));
    } else {
        m_sizeLabel->setText(QStringLiteral("%1 × %2 px").arg(m_image.width()).arg(m_image.height()));
    }
    m_previous->setEnabled(browse && m_index > 0);
    m_next->setEnabled(browse && m_index < m_paths.size() - 1);
    m_copyButton->setEnabled(!m_image.isNull());
    if (browse)
        m_sizeLabel->setText(m_sizeLabel->text()
            + QStringLiteral("　%1 / %2").arg(m_index + 1).arg(m_paths.size()));
    m_zoomLabel->setText(QStringLiteral("%1%").arg(qRound(m_canvas->factor() * 100)));
}

void ImageBrowserWindow::rotate(int degrees)
{
    if (m_canvas->hasImage())
        m_canvas->setRotation(m_canvas->rotation() + degrees);
}

void ImageBrowserWindow::stepZoom(int direction)
{
    if (m_canvas->hasImage())
        m_canvas->setFactor(m_canvas->factor() * (direction >= 0 ? kZoomStep : 1.0 / kZoomStep));
}

void ImageBrowserWindow::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        close();
        return;
    case Qt::Key_Left:
        if (m_previous->isEnabled())
            showIndex(m_index - 1);
        return;
    case Qt::Key_Right:
        if (m_next->isEnabled())
            showIndex(m_index + 1);
        return;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        stepZoom(1);
        return;
    case Qt::Key_Minus:
        stepZoom(-1);
        return;
    case Qt::Key_0:
    case Qt::Key_F:
        m_canvas->fitView();
        return;
    case Qt::Key_1:
        m_canvas->setFactor(1.0);
        return;
    case Qt::Key_R:
        rotate(event->modifiers() & Qt::ShiftModifier ? -90 : 90);
        return;
    case Qt::Key_C:
        if (event->modifiers() & Qt::ControlModifier) {
            if (!m_image.isNull())
                QApplication::clipboard()->setImage(m_image);
        }
        return;
    default:
        QWidget::keyPressEvent(event);
    }
}

#include "ImageBrowserWindow.moc"
