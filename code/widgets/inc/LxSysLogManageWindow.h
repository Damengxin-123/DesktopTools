#pragma once

#include <QWidget>
#include "ui_LxSysLogManageWindow.h"

class LxSysLogManageWindow : public QWidget
{
    Q_OBJECT

public:
    LxSysLogManageWindow(QWidget *parent = nullptr);
    ~LxSysLogManageWindow();

private:
    Ui::LxSysLogManageWindow ui;
};
