#include "PaintWindow.h"
#include "PaintCanvas.h"

#include <QActionGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QImageReader>
#include <QImageWriter>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QToolBar>

PaintWindow::PaintWindow(const QString& dataRoot, QWidget* parent)
    : QMainWindow(parent, Qt::Window), m_canvas(new PaintCanvas(this)),
      m_preferences(QDir(dataRoot).filePath(QStringLiteral("paint.ini")))
{
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName(QStringLiteral("paint-window"));
    resize(1180, 820);
    setMinimumSize(800, 540);
    auto* scroll = new QScrollArea(this);
    scroll->setAlignment(Qt::AlignCenter);
    scroll->setWidget(m_canvas);
    scroll->setStyleSheet(QStringLiteral("QScrollArea { background: #dce3eb; border: 0; }"));
    setCentralWidget(scroll);
    auto* files = addToolBar(QStringLiteral("文件与编辑"));
    files->setMovable(false);
    // 统一创建带中文说明和可选标准快捷键的动作。
    const auto action = [this, files](const QString& name, const QKeySequence& key, auto callback) {
        auto* item = files->addAction(name);
        if (!key.isEmpty()) item->setShortcut(key);
        connect(item, &QAction::triggered, this, callback);
        return item;
    };
    action(QStringLiteral("新建"), QKeySequence::New, [this] { newDocument(); });
    action(QStringLiteral("打开图片"), QKeySequence::Open, [this] { openDocument(); });
    action(QStringLiteral("保存"), QKeySequence::Save, [this] { saveDocument(); });
    action(QStringLiteral("另存为"), QKeySequence::SaveAs, [this] { saveDocument(true); });
    files->addSeparator();
    m_undo = action(QStringLiteral("撤销"), QKeySequence::Undo, [this] { m_canvas->undo(); });
    m_redo = action(QStringLiteral("重做"), QKeySequence::Redo, [this] { m_canvas->redo(); });
    action(QStringLiteral("复制图片"), QKeySequence::Copy, [this] {
        m_canvas->resetInput();
        QGuiApplication::clipboard()->setImage(m_canvas->image());
    });
    action(QStringLiteral("清空"), {}, [this] {
        m_canvas->resetInput();
        if (QMessageBox::question(this, QStringLiteral("清空画布"), QStringLiteral("清空当前画布？此操作可以撤销。"),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
            m_canvas->clear();
    });
    files->addSeparator();
    auto* zoom = new QComboBox(files);
    zoom->setAccessibleName(QStringLiteral("画布缩放"));
    for (const int percent : {25, 50, 75, 100, 150, 200, 300, 400})
        zoom->addItem(QString::number(percent) + "%", percent / 100.0);
    zoom->setCurrentIndex(3);
    files->addWidget(zoom);
    connect(zoom, &QComboBox::currentIndexChanged, this, [this, zoom] { m_canvas->setZoom(zoom->currentData().toDouble()); });

    addToolBarBreak();
    auto* tools = addToolBar(QStringLiteral("绘图工具"));
    tools->setMovable(false);
    auto* group = new QActionGroup(this);
    // 名称与枚举一一对应，工具切换不使用容易误触的字母快捷键。
    const QStringList names{QStringLiteral("画笔"), QStringLiteral("橡皮"), QStringLiteral("填充"), QStringLiteral("取色"),
        QStringLiteral("直线"), QStringLiteral("矩形"), QStringLiteral("椭圆"), QStringLiteral("文字")};
    for (int i = 0; i < names.size(); ++i) {
        auto* item = tools->addAction(names[i]);
        item->setCheckable(true);
        group->addAction(item);
        item->setChecked(i == 0);
        connect(item, &QAction::triggered, this, [this, i] {
            m_canvas->setTool(static_cast<PaintCanvas::Tool>(i));
            if (i == static_cast<int>(PaintCanvas::Tool::Text)) {
                bool accepted = false;
                const QString text = QInputDialog::getText(this, QStringLiteral("文字工具"), QStringLiteral("输入文字，随后在画布上点击放置："),
                                                         QLineEdit::Normal, {}, &accepted);
                m_canvas->setText(accepted ? text : QString(), 28);
            }
            m_canvas->setFocus();
        });
    }
    tools->addSeparator();
    m_color = tools->addAction(QStringLiteral("颜色"));
    // 用图标显示当前实际前景色，取色器与颜色对话框共用更新。
    const auto showColor = [this](const QColor& color) {
        QPixmap swatch(20, 20);
        swatch.fill(color);
        m_color->setIcon(QIcon(swatch));
        m_color->setToolTip(QStringLiteral("当前颜色：%1").arg(color.name()));
        m_color->setData(color);
    };
    showColor(QColor("#263445"));
    connect(m_canvas, &PaintCanvas::colorPicked, this, showColor);
    connect(m_color, &QAction::triggered, this, [this, showColor] {
        m_canvas->resetInput();
        const QColor color = QColorDialog::getColor(m_color->data().value<QColor>(), this, QStringLiteral("画笔颜色"));
        if (color.isValid()) { m_canvas->setColor(color); showColor(color); }
    });
    tools->addWidget(new QLabel(QStringLiteral("  粗细（像素） "), tools));
    auto* width = new QSpinBox(tools);
    width->setRange(1, 200);
    width->setValue(5);
    width->setAccessibleName(QStringLiteral("画笔粗细（像素）"));
    tools->addWidget(width);
    connect(width, &QSpinBox::valueChanged, m_canvas, &PaintCanvas::setBrushWidth);
    auto* filled = new QCheckBox(QStringLiteral("填充形状"), tools);
    tools->addWidget(filled);
    connect(filled, &QCheckBox::toggled, m_canvas, &PaintCanvas::setShapeFilled);

    addToolBarBreak();
    auto* mode = addToolBar(QStringLiteral("数位板控制"));
    mode->setMovable(false);
    m_tablet = new QCheckBox(QStringLiteral("数位板模式"), mode);
    m_tablet->setObjectName(QStringLiteral("paint-tablet-mode"));
    m_tablet->setToolTip(QStringLiteral("左键或笔尖接触只定位，额外按住控制键才落笔；松开立即断笔。"));
    mode->addWidget(m_tablet);
    mode->addWidget(new QLabel(QStringLiteral("    按住落笔："), mode));
    m_key = new QComboBox(mode);
    m_key->setAccessibleName(QStringLiteral("数位板落笔按键"));
    m_key->addItem(QStringLiteral("空格 Space"), Qt::Key_Space);
    m_key->addItem(QStringLiteral("Shift"), Qt::Key_Shift);
    for (const int key : {Qt::Key_F, Qt::Key_D, Qt::Key_A}) m_key->addItem(QString(QChar(key)), key);
    mode->addWidget(m_key);
    m_hint = new QLabel(mode);
    mode->addWidget(m_hint);
    QSettings preferences(m_preferences, QSettings::IniFormat);
    m_tablet->setChecked(preferences.value(QStringLiteral("tabletMode"), false).toBool());
    m_key->setCurrentIndex(qMax(0, m_key->findData(preferences.value(QStringLiteral("drawingKey"), int(Qt::Key_Space)).toInt())));
    m_canvas->setTabletMode(m_tablet->isChecked());
    m_canvas->setDrawingKey(m_key->currentData().toInt());
    connect(m_tablet, &QCheckBox::toggled, this, [this](bool enabled) {
        m_canvas->setTabletMode(enabled); savePreferences(); refresh(); m_canvas->setFocus();
    });
    connect(m_key, &QComboBox::currentIndexChanged, this, [this] {
        m_canvas->setDrawingKey(m_key->currentData().toInt()); savePreferences(); refresh(); m_canvas->setFocus();
    });
    connect(m_canvas, &PaintCanvas::changed, this, &PaintWindow::refresh);
    connect(m_canvas, &PaintCanvas::drawingPermissionChanged, this, [this](bool allowed) { m_allowed = allowed; refresh(); });
    refresh();
    m_canvas->setFocus();
}

void PaintWindow::savePreferences()
{
    QSettings settings(m_preferences, QSettings::IniFormat);
    settings.setValue(QStringLiteral("tabletMode"), m_tablet->isChecked());
    settings.setValue(QStringLiteral("drawingKey"), m_key->currentData());
}
void PaintWindow::refresh()
{
    setWindowTitle(QStringLiteral("%1%2 — 画板").arg(m_canvas->isModified() ? QStringLiteral("* ") : QString(),
                     m_path.isEmpty() ? QStringLiteral("未命名") : QFileInfo(m_path).fileName()));
    m_undo->setEnabled(m_canvas->canUndo());
    m_redo->setEnabled(m_canvas->canRedo());
    m_key->setEnabled(m_tablet->isChecked());
    m_hint->setText(!m_tablet->isChecked() ? QStringLiteral("    普通模式 · 按住左键或笔尖绘制")
        : m_allowed ? QStringLiteral("    ● 允许落笔 · 松键即停")
        : QStringLiteral("    ○ 仅定位 · 左键 / 笔尖接触 + 按住 %1 才绘制").arg(m_key->currentText()));
    statusBar()->showMessage(QStringLiteral("%1 × %2 像素    |    缩放 %3%    |    Ctrl+Z 撤销 · Ctrl+Y 重做 · Ctrl+S 保存")
        .arg(m_canvas->image().width()).arg(m_canvas->image().height()).arg(qRound(m_canvas->zoom() * 100)));
}
bool PaintWindow::confirmDiscard()
{
    m_canvas->resetInput();
    if (!m_canvas->isModified()) return true;
    const auto answer = QMessageBox::warning(this, QStringLiteral("保存画作"), QStringLiteral("当前画作尚未保存，是否保存后继续？"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save) return saveDocument();
    return answer == QMessageBox::Discard;
}
void PaintWindow::newDocument()
{
    m_canvas->resetInput();
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("新建画布"));
    auto* form = new QFormLayout(&dialog);
    auto* width = new QSpinBox(&dialog);
    auto* height = new QSpinBox(&dialog);
    width->setRange(32, 4096); height->setRange(32, 4096);
    width->setValue(1280); height->setValue(800);
    form->addRow(QStringLiteral("宽度（像素）"), width);
    form->addRow(QStringLiteral("高度（像素）"), height);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || !confirmDiscard()) return;
    QImage blank(width->value(), height->value(), QImage::Format_ARGB32);
    if (blank.isNull()) { QMessageBox::warning(this, QStringLiteral("新建失败"), QStringLiteral("内存不足，请减小画布尺寸。")); return; }
    blank.fill(Qt::white);
    m_path.clear();
    m_canvas->setImage(blank);
}
void PaintWindow::openDocument()
{
    m_canvas->resetInput();
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("打开图片"), m_path,
        QStringLiteral("图像 (*.png *.jpg *.jpeg *.bmp *.webp);;所有文件 (*)"));
    if (path.isEmpty()) return;
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (!size.isValid() || qint64(size.width()) * size.height() > 16777216) {
        QMessageBox::warning(this, QStringLiteral("无法打开"), QStringLiteral("图像无效或超过 1677 万像素的画布上限。")); return;
    }
    const QImage image = reader.read();
    if (image.isNull()) { QMessageBox::warning(this, QStringLiteral("无法打开"), reader.errorString()); return; }
    if (!confirmDiscard()) return;
    m_path = path;
    m_canvas->setImage(image);
}
bool PaintWindow::saveDocument(bool saveAs)
{
    m_canvas->resetInput();
    QString path = m_path;
    QString selected = QStringLiteral("PNG 图片 (*.png)");
    if (saveAs || path.isEmpty()) {
        path = QFileDialog::getSaveFileName(this, QStringLiteral("保存画作"), path.isEmpty() ? QStringLiteral("画作.png") : path,
            QStringLiteral("PNG 图片 (*.png);;JPEG 图片 (*.jpg *.jpeg);;BMP 图片 (*.bmp)"), &selected);
        if (path.isEmpty()) return false;
        if (QFileInfo(path).suffix().isEmpty()) {
            path += selected.startsWith("JPEG") ? ".jpg" : selected.startsWith("BMP") ? ".bmp" : ".png";
            // 补全扩展名后重新确认覆盖，避免文件对话框仅检查了无后缀路径。
            if (QFileInfo::exists(path) && QMessageBox::question(this, QStringLiteral("替换图片"),
                    QStringLiteral("文件已存在，是否替换？\n%1").arg(path), QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No) != QMessageBox::Yes) return false;
        }
    }
    const QByteArray format = QFileInfo(path).suffix().toLower().toLatin1();
    if (!QImageWriter::supportedImageFormats().contains(format)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), QStringLiteral("不支持此文件扩展名，请另存为 PNG、JPEG 或 BMP。")); return false;
    }
    QImage image = m_canvas->image();
    if (format == "jpg" || format == "jpeg" || format == "bmp") {
        QImage opaque(image.size(), QImage::Format_RGB32);
        opaque.fill(Qt::white);
        QPainter painter(&opaque);
        painter.drawImage(0, 0, image);
        painter.end();
        image = opaque;
    }
    QSaveFile file(path);
    QImageWriter writer(&file, format);
    writer.setQuality(95);
    if (!file.open(QIODevice::WriteOnly) || !writer.write(image) || !file.commit()) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), QStringLiteral("无法保存画作：%1\n%2").arg(file.errorString(), writer.errorString()));
        return false;
    }
    m_path = path;
    m_canvas->markSaved();
    return true;
}
void PaintWindow::closeEvent(QCloseEvent* event)
{
    if (confirmDiscard()) event->accept();
    else event->ignore();
}
