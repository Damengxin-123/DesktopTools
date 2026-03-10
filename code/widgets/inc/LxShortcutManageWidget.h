#pragma once

#include <QWidget>
#include "ui_LxShortcutManageWidget.h"

class QListWidgetItem;
/*
 快捷方式功能界面
*/
class LxShortcutManageWidget : public QWidget
{
    Q_OBJECT

public:
    LxShortcutManageWidget(QWidget *parent = nullptr);
    ~LxShortcutManageWidget();
private slots:
    //  选择目录
    void on_uButOpenDir_clicked();
    // 选择文件
    void on_uButOpenFile_clicked();
    // 选择快捷方式类型
    void on_uComboxShortcutType_currentIndexChanged(int index);
    // 添加快捷方式
    void on_uButAddShortcut_clicked();
    // 列表项双击事件
    void on_uShortcutsListWidget_itemDoubleClicked(QListWidgetItem* item);
    // 列表选择事件
    void on_uButSelectAllNoteList_clicked();
    // 全选按钮
    void on_uButSelectAllShortcutList_clicked();
    // 删除按钮
    void on_uButRemoveSelectedShortcuts_clicked();
    // 搜索功能
    void on_uButSearch_clicked();
private:
    // 加载全部快捷方式 用于初始化及刷新时
    void loadShortcutsFromConfig();
    
    // 保存快捷方式到配置文件
    void saveShortcutToConfig(const QString& title, const QString& path, int type);

    Ui::LxShortcutManageWidget ui;

    // 当前快捷方式类型
    int m_nCurrentShortcutType = 0; // 0:目录, 1:网页链接, 2:文件

    // 当前选中的item
    QListWidgetItem* m_pCurrentItem = nullptr;
};
