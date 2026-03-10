#include <QFileDialog>
#include <QDesktopServices>
#include "LxConfig.h"
#include "LxShortcutManageWidget.h"

LxShortcutManageWidget::LxShortcutManageWidget(QWidget* parent)
    : QWidget(parent)
{
    ui.setupUi(this);

    // --- 针对整个 QListWidget 的全局设置 ---
    // ui.uShortcutsListWidget->setViewMode(QListView::IconMode);   // 图标模式
    ui.uShortcutsListWidget->setIconSize(QSize(20, 20));         // 图标大小
    ui.uShortcutsListWidget->setSpacing(0);                     // 项目之间的间距
    ui.uShortcutsListWidget->setResizeMode(QListView::Adjust);   // 自动调整布局
    ui.uShortcutsListWidget->setStyleSheet(R"(
    QListWidget {
        font-size: 20px;
        color: #4e5661;
        font-family: 'Arial';          /* 设置字体 */
        background-color: transparent; /* 设置背景色 */
    }

    QListWidget::item {
        padding: 5px 5px;             /* 设置 item 内部的内边距 */
        font-weight: bold;             /* 设置字体加粗 */
    }
)");

    // 初始化时加载快捷方式配置
    loadShortcutsFromConfig(); 

    // 搜索框回车事件
    connect(ui.uInputSearch, &QLineEdit::returnPressed, this, &LxShortcutManageWidget::on_uButSearch_clicked);
}

LxShortcutManageWidget::~LxShortcutManageWidget()
{
}

void LxShortcutManageWidget::on_uComboxShortcutType_currentIndexChanged(int index)
{
    // 判断是否更改了快捷方式类型
    if (m_nCurrentShortcutType != index)
    {
        // 清空输入框
        //ui.uShortcutTitleInput->clear();

        // 更新当前快捷方式类型
        m_nCurrentShortcutType = index;
    }
}

void LxShortcutManageWidget::on_uButAddShortcut_clicked()
{
    // 如果输入框为空，提示用户输入标题
    if (ui.uShortcutTitleInput->text().isEmpty())
    {
        return;
    }

    QString title = ui.uShortcutTitleInput->text();
    QString path = ui.uShortcutPathInput->text();
    int type = m_nCurrentShortcutType;

    // 保存到配置文件
    saveShortcutToConfig(title, path, type);
}

void LxShortcutManageWidget::on_uShortcutsListWidget_itemDoubleClicked(QListWidgetItem* item)
{
    // 获取存储的路径和类型
    QString path = item->data(Qt::UserRole).toString();
    int type = item->data(Qt::UserRole + 1).toInt();
    // 根据类型打开对应的路径
    switch (type)
    {
    case 0: // 目录
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        break;
    case 1: // 网页链接
        QDesktopServices::openUrl(QUrl(path));
        break;
    case 2: // 文件
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        break;
    default:
        qDebug() << "未知的快捷方式类型:" << type;
        break;
    }
}

void LxShortcutManageWidget::on_uButSelectAllNoteList_clicked()
{
    // 判断当前选择了哪个item
    //for (int i = 0; i < ui.uShortcutsListWidget->count(); i++)
    //{
    //    QListWidgetItem* item = ui.uShortcutsListWidget->item(i);
    //    item->setSelected(true); // 设置为选中状态


    //}
    // 获取当前选中的item
    QListWidgetItem* selectedItem = ui.uShortcutsListWidget->currentItem();
    // 设置勾选框为选中状态
    if (selectedItem)
    {
        selectedItem->setCheckState(Qt::CheckState(!selectedItem->checkState())); // 设置为选中状态
    }
}

void LxShortcutManageWidget::on_uButSelectAllShortcutList_clicked()
{
    // 选中所有快捷方式列表项
    for (int i = 0; i < ui.uShortcutsListWidget->count(); i++)
    {
        QListWidgetItem* item = ui.uShortcutsListWidget->item(i);
        item->setCheckState(Qt::CheckState::Checked); // 设置为选中状态
    }
}

void LxShortcutManageWidget::on_uButRemoveSelectedShortcuts_clicked()
{
    // 判断当前选择的item
    int count = ui.uShortcutsListWidget->count() - 1;
    for (int i = count; i >= 0; i --)
    {
        QListWidgetItem* item = ui.uShortcutsListWidget->item(i);

        if (item->checkState())
        {
            // 删除选中的快捷方式
            QString title = item->text();
            QString path = item->data(Qt::UserRole).toString();
            int type = item->data(Qt::UserRole + 1).toInt();
            // 从配置文件中删除对应的快捷方式
            QString configFilePath = QApplication::applicationDirPath() + ShortcutFilePath + "/shortcuts_config.txt";
            QFile configFile(configFilePath);
            if (configFile.open(QIODevice::ReadOnly | QIODevice::Text))
            {
                QStringList lines;
                QTextStream in(&configFile);
                while (!in.atEnd())
                {
                    QString line = in.readLine();
                    if (!line.contains(title) || !line.contains(path) || !line.contains(QString::number(type)))
                    {
                        lines.append(line); // 保留未删除的行
                    }
                }
                configFile.close();
                // 重新写入文件
                if (configFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
                {
                    QTextStream out(&configFile);
                    for (const QString& line : lines)
                    {
                        out << line << "\n";
                    }
                    configFile.close();
                }
            }
            
            // 从列表中移除
            delete ui.uShortcutsListWidget->takeItem(i);
        }
    }
}

void LxShortcutManageWidget::on_uButSearch_clicked()
{
    // 获取搜索关键字
    QString searchText = ui.uInputSearch->text().trimmed();
    // 清空当前列表
    ui.uShortcutsListWidget->clear();
    // 重新加载配置文件中的快捷方式
    loadShortcutsFromConfig();
    // 遍历列表项，隐藏不匹配的项
    for (int i = 0; i < ui.uShortcutsListWidget->count(); ++i)
    {
        QListWidgetItem* item = ui.uShortcutsListWidget->item(i);
        if (item->text().contains(searchText, Qt::CaseInsensitive))
        {
            item->setSelected(true); // 显示匹配的项
        }
        else
        {
            item->setSelected(false); // 隐藏不匹配的项
        }
    }
}

void LxShortcutManageWidget::loadShortcutsFromConfig()
{
    // 读取配置文件中的快捷方式信息
    QString configFilePath = QApplication::applicationDirPath() + ShortcutFilePath + "/shortcuts_config.txt";
    QFile configFile(configFilePath);

    // 如果文件打开失败，提示错误
    if (!configFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        qDebug() << "无法打开配置文件:" << configFilePath;
        return;
    }

    // 清空现有的快捷方式列表
    ui.uShortcutsListWidget->clear();

    // 读取文件内容
    QTextStream in(&configFile);
    while (!in.atEnd())
    {
        QString line = in.readLine();

        // 跳过空行
        if (line.isEmpty())
        {
            continue;
        }

        QStringList parts = line.split(",");

        // 确保格式正确
        if (parts.size() < 3)
        {
            continue;
        }

        QString title = parts[0].trimmed();
        QString path = parts[1].trimmed();
        int type = parts[2].trimmed().toInt();

        // 设置图标路径
        QString iconPath;
        switch (type)
        {
            case 0: // 目录
                iconPath = ":/images/dir.png"; // 确保路径格式正确
                break;
            case 1: // 网页链接
                iconPath = ":/images/web.png"; // 确保路径格式正确
                break;
            case 2: // 文件
                iconPath = ":/images/file.png"; // 确保路径格式正确
                break;
            default:
                continue; // 如果类型不合法，跳过
        }


        // 创建列表项并添加到列表中
        QListWidgetItem* item = new QListWidgetItem(QIcon(iconPath), title);

        // 设置存储数据
        item->setData(Qt::UserRole, path);      // 存储路径
        item->setData(Qt::UserRole + 1, type);  // 存储类型
        item->setCheckState(Qt::Unchecked);     // 设置复选框状态
        // 设置文字居中
        item->setTextAlignment(Qt::AlignVCenter);

        // 设置 item 的大小（整个格子的宽高）
        item->setSizeHint(QSize(100, 40));

        // 添加到列表
        ui.uShortcutsListWidget->addItem(item);
    }
}

void LxShortcutManageWidget::saveShortcutToConfig(const QString& title, const QString& path, int type)
{
    // 在程序目录中的指定文件夹中的配置文件中
    QString configFilePath = QApplication::applicationDirPath() + ShortcutFilePath + "/shortcuts_config.txt";

    // 打开配置文件
    QFile configFile(configFilePath);
    if (!configFile.open(QIODevice::Append | QIODevice::Text))
    {
        // 如果文件打开失败，提示错误
        qDebug() << "无法打开配置文件:" << configFilePath;
        return;
    }

    // 写入快捷方式信息到配置文件
    QTextStream out(&configFile);
    out << title << "," << path << "," << type << "\n";
    configFile.close();

    // 刷新
    loadShortcutsFromConfig();
}

void LxShortcutManageWidget::on_uButOpenDir_clicked()
{
    // 选择目录对话框
    QString dirName = QFileDialog::getExistingDirectory(
        this,
        "选择想要快捷打开的目录",
        "",
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
    );

    // 如果用户选择了目录，则将目录名设置到输入框中
    if (!dirName.isEmpty())
    {
        // 设置输入框内容为选择的目录
        // 选择文件夹名称作为标题
        if (ui.uShortcutTitleInput->text().isEmpty())
        {
            // 获取目录的最后一部分作为标题 
            QString title = dirName.section('/', -1);
            ui.uShortcutTitleInput->setText(title);
        }

        ui.uShortcutPathInput->setText(dirName);

        // 设置快捷方式类型为目录
        ui.uComboxShortcutType->setCurrentIndex(0);
        m_nCurrentShortcutType = 0;
    }
    else
    {
        // 如果没有选择目录，则清空输入框
        ui.uShortcutTitleInput->clear();
    }
}

void LxShortcutManageWidget::on_uButOpenFile_clicked()
{
    // 选择文件对话框
    QString fileName = QFileDialog::getOpenFileName(
        this,
        "选择想要快捷打开的文件",
        "",
        "所有文件 (*.*)"
    );

    // 如果用户选择了文件，则将文件名设置到输入框中
    if (!fileName.isEmpty())
    {
        // 设置输入框内容为选择的文件名
        // 选择文件名称作为标题
        if (ui.uShortcutTitleInput->text().isEmpty())
        {
            // 获取文件的最后一部分作为标题
            QString title = fileName.section('/', -1);
            ui.uShortcutTitleInput->setText(title);
        }

        ui.uShortcutPathInput->setText(fileName);

        // 设置快捷方式类型为文件
        ui.uComboxShortcutType->setCurrentIndex(2);
        m_nCurrentShortcutType = 2;
    }
    else
    {
        // 如果没有选择文件，则清空输入框
        ui.uShortcutTitleInput->clear();
    }
}
