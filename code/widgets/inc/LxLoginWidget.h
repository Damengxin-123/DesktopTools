#pragma once

#include <QWidget>
#include "ui_LxLoginWidget.h"

class LxLoginWidget : public QWidget
{
    Q_OBJECT

public:
    LxLoginWidget(QWidget *parent = nullptr);
    ~LxLoginWidget();
private slots:
    // 登录按钮点击事件
    void on_uButLogin_clicked();
    // 退出
    void on_uButQuitLogin_clicked();

private:
    Ui::LxLoginWidget ui;
};
