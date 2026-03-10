#pragma once

#include <QWidget>
#include <QVector>
#include "ui_LxNoteWindow.h"
class LxLineEdit;
class QListWidgetItem;


class LxNoteWindow : public QWidget
{
    Q_OBJECT

public:
    LxNoteWindow(QWidget *parent = nullptr);
    ~LxNoteWindow();

public slots:   
    // 大小停止更新事件接收
    void on_minWindowSizeChanged();
private slots:
    // 保存
    void on_uButSaveNote_clicked();
    // 清空
    void on_uButClearNoteData_clicked();
    // 搜索
    void on_uButSearch_clicked();
    // 搜索框确认
    void on_uInputSearch_returnPressed();
    // 列表项双击
    void on_uNoteListWidget_itemDoubleClicked(QListWidgetItem* item);
    // 选中所有便签列表项
    void on_uButSelectAllNoteList_clicked();
    // 删除选中的便签列表项
    void on_uButRemoveSelectedNotes_clicked();
private:


    Ui::LxNoteWindow ui;

    // 删除便签列表项以及便签文件
    void removeSelectedNotes(QListWidgetItem* listItem);

    // 初始化便签列表
    void initNoteListWidget();

    // 保存当前编辑的内容
    void saveDocumentWithImages(QTextEdit* textEdit, const QString& htmlFilePath);

    // 便签列表项列表
    QVector<QListWidgetItem*> m_noteListItems;
};