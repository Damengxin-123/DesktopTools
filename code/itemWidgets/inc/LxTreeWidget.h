#pragma once

#include <QWidget>
#include "ui_LxTreeWidget.h"

class LxTreeModel;

class LxTreeWidget : public QWidget
{
    Q_OBJECT

public:
    LxTreeWidget(QWidget *parent = nullptr);
    ~LxTreeWidget();
public slots:
    void onTreeContextMenu(const QPoint& pos);
private:
    Ui::LxTreeWidget ui;

         
    LxTreeModel* m_pTreeViewModel{ nullptr };
};
