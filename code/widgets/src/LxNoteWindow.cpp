#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QResizeEvent>
#include <QTextStream>
#include <QMessageBox>
#include <QSplitter>
#include "LxNoteWindow.h"
#include "itemWidgets/inc/LxConfig.h"

LxNoteWindow::LxNoteWindow(QWidget *parent)
    : QWidget(parent)
{
    ui.setupUi(this);
    ui.uNoteDataInput->setAcceptRichText(true);  // 默认就是 true

    // 初始化便签列表
    initNoteListWidget(); 

    // 初始化布局
    // 将控件添加到左右可控布局中
    QSplitter* splitter = new QSplitter(Qt::Horizontal); // 水平分布
    splitter->addWidget(ui.uNoteEditWidget);
    splitter->addWidget(ui.uNoteManageWidget);

    splitter->setStretchFactor(0, 5); // 设置左侧控件占据剩余空间
    splitter->setStretchFactor(1, 2); // 设置左侧控件占据剩余空间

    ui.uCellWidgetLayout->addWidget(splitter);

}

LxNoteWindow::~LxNoteWindow()
{
}

void LxNoteWindow::on_uButClearNoteData_clicked()
{
    // 清空便签内容
    ui.uNoteDataInput->clear();
    // 清空标题输入框
    ui.uNoteTitleInput->clear(); 
}

void LxNoteWindow::on_uButSearch_clicked()
{
    QString searchText = ui.uInputSearch->text().trimmed();
    if (searchText.isEmpty()) {
        // 如果搜索文本为空，清空列表
        for (QListWidgetItem* item : m_noteListItems) {
            item->setHidden(false); // 显示所有项
        }
        return;
    }
    // 遍历列表项，隐藏不匹配的项
    for (QListWidgetItem* item : m_noteListItems) {
        if (item->text().contains(searchText, Qt::CaseInsensitive)) {
            item->setHidden(false); // 显示匹配项
        }
        else {
            item->setHidden(true); // 隐藏不匹配项
        }
    }
}

void LxNoteWindow::on_uInputSearch_returnPressed()
{
    on_uButSearch_clicked();
}

void LxNoteWindow::on_uNoteListWidget_itemDoubleClicked(QListWidgetItem* item)
{
    // 双击列表项时，加载对应的便签内容
    QString dirName = item->text();
    QString filePath = QApplication::applicationDirPath() + NoteFilePath + dirName + "/index.html"; // 便签文件路径
    QFile file(filePath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {

        // 设置html资源路径
        QTextDocument* doc = ui.uNoteDataInput->document();;
        QUrl baseUrl = QUrl::fromLocalFile(QFileInfo(QApplication::applicationDirPath() + NoteFilePath + dirName + "/").absolutePath() + "/");
        doc->setBaseUrl(baseUrl);

        QTextStream in(&file);
        QString content = in.readAll();
        // 设置编辑器内容
        //doc->setHtml(content);
        // 设置文档内容
        //ui.uNoteDataInput->setDocument(doc); 
        // 设置HTML内容
        ui.uNoteDataInput->setHtml(content); 
        // 触发resize事件以更新内容显示
        ui.uNoteDataInput->updateImageSize();
        // 设置标题输入框内容
        ui.uNoteTitleInput->setText(dirName); 
        file.close();
    }
    else {
        ui.uNoteDataInput->clear(); // 如果文件不存在，清空编辑器
    }
}

void LxNoteWindow::on_uButSelectAllNoteList_clicked()
{
    // 选中所有便签列表项
    for (QListWidgetItem* item : m_noteListItems) {
        item->setCheckState(Qt::Checked); // 设置为选中状态
    }
}

void LxNoteWindow::on_uButRemoveSelectedNotes_clicked()
{
    // 弹窗询问是否删除
    QMessageBox::StandardButton reply;
    reply = QMessageBox::question(this, "删除确认", "确定要删除选中的便签吗？", QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) {
        return; // 如果用户选择否，则不执行删除操作
    }


    // 删除选中的便签列表项
    for(int i = m_noteListItems.size() - 1; i > -1; i --)
    {
        if (m_noteListItems[i]->checkState() == Qt::Checked) 
        {
            removeSelectedNotes(m_noteListItems[i]); // 删除对应的便签文件和列表项
        }
    }
}

void LxNoteWindow::on_minWindowSizeChanged()
{
    // 当主窗口停止缩放时，调用更新显示的图片的大小
    ui.uNoteDataInput->updateImageSize(); // 更新图片大小
}


void LxNoteWindow::on_uButSaveNote_clicked()
{
    QString dirName = ui.uNoteTitleInput->text();
    if (dirName.isEmpty()) {
        // 默认文件名为当前时间戳
        dirName = QString::number(QDateTime::currentMSecsSinceEpoch());
    }
    QString filePath = QApplication::applicationDirPath() + NoteFilePath + dirName + "/index.html"; // 保存到应用程序目录


    saveDocumentWithImages(ui.uNoteDataInput, filePath); // 保存到 note.html 文件

    // 重新初始化便签列表
    initNoteListWidget(); 
}

void LxNoteWindow::removeSelectedNotes(QListWidgetItem* listItem)
{
    // 删除选中的便签列表项以及对应的便签文件
    QString notesDir = QApplication::applicationDirPath() + NoteFilePath + listItem->text();
    QDir dir(notesDir);
    if (dir.exists()) {
        dir.removeRecursively(); // 删除目录及其内容
    }


    // 从 QVector 中移除
    m_noteListItems.removeOne(listItem);
    // 从列表中移除对应的 QListWidgetItem
    delete listItem;

    // 重新初始化便签列表
    // initNoteListWidget();
    
}

void LxNoteWindow::initNoteListWidget()
{
    // 清空列表
    ui.uNoteListWidget->clear(); 
    // 清空 QVector
    m_noteListItems.clear(); 

    ui.uNoteListWidget->setStyleSheet(R"(
        QListWidget::item {
            font-size: 18px;
            font-weight: bold;
            font-family: "宋体";
            padding-left: 10px;
            border-bottom: 1px solid #dcdcdc; /* 浅灰色 */
            height: 32px; /* 可选：统一高度 */
            color: #4e5661;
        }
        QListWidget::item:selected {
            background-color: #e6f2ff; /* 可选：选中项背景 */
            font-size: 18px;
            font-family: "宋体";
            padding-left: 10px;
            font-weight: bold;
            border-bottom: 1px solid #dcdcdc; /* 浅灰色 */
            height: 32px; /* 可选：统一高度 */
            color: #4e5661;
        }
    )");
    ui.uNoteListWidget->setFocusPolicy(Qt::NoFocus);

    // 读取便签文件夹下所有的文件夹，作为列表名称
    QString notesDir = QApplication::applicationDirPath() + NoteFilePath;
    QDir dir(notesDir);

    // 设置过滤条件：仅列出目录
    dir.setFilter(QDir::Dirs | QDir::NoDotAndDotDot);

    // 获取目录名称列表
    QStringList folderList = dir.entryList();

    // 添加到 QListWidget
    QListWidgetItem* item = nullptr;
    QFont font1;
    font1.setFamilies({ QString::fromUtf8("\345\256\213\344\275\223") });
    font1.setPointSize(16);
    font1.setBold(true);
    for (const QString& folderName : folderList) {

        item = new QListWidgetItem(folderName);

        // 设置每个条目的高度
        item->setSizeHint(QSize(0, 30)); 
        item->setFont(font1);
        
        // 设置复选框状态
        item->setCheckState(Qt::Unchecked); 

        // 添加到列表项容器
        m_noteListItems.append(item); 

        ui.uNoteListWidget->addItem(item);
    }
}

void LxNoteWindow::saveDocumentWithImages(QTextEdit* textEdit, const QString& htmlFilePath)
{
    QTextDocument* doc = textEdit->document();

    // 提取原始 HTML
    QString html = doc->toHtml();

    // 图像资源目录
    QString baseDir = QFileInfo(htmlFilePath).absolutePath();
    QString imageDir = baseDir + "/images";

    QDir().mkpath(imageDir); // 创建目录

    // 遍历资源
    QMap<QString, QString> imageMap; // resource -> file path 映射
    QRegularExpression imgTagRegex(R"(<img[^>]*src=\"([^\"]+)\"[^>]*>)");
        QRegularExpressionMatchIterator it = imgTagRegex.globalMatch(html);

    int imageIndex = 0;
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        QString resourceName = match.captured(1);

        if (imageMap.contains(resourceName))
            continue; // 已处理

        QVariant resourceData = doc->resource(QTextDocument::ImageResource, QUrl(resourceName));
        if (resourceData.isValid()) {
            QImage image = resourceData.value<QImage>();
            QString imageFileName = QString("img_%1.png").arg(++imageIndex);
            QString imageFilePath = imageDir + "/" + imageFileName;

            // 保存图片
            image.save(imageFilePath);

            // 替换路径
            QString relativePath = "images/" + imageFileName;
            imageMap[resourceName] = relativePath;

            html.replace(resourceName, relativePath);
        }
    }

    // 保存 HTML
    QFile file(htmlFilePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&file);
        // out.setCodec("UTF-8");
        out << html;
        file.close();
    }
    else {
        QMessageBox::warning(textEdit, "保存失败", "无法保存便签内容到文件: " + htmlFilePath);
        return;
    }
    // 保存成功
    QMessageBox::information(textEdit, "保存成功", "便签内容已保存到: ");

}


