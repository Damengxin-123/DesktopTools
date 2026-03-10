#pragma once

#include <QWidget>
#include "ui_LxDownloadWidget.h"

class LxDownloadWidget : public QWidget
{
    Q_OBJECT

public:
    LxDownloadWidget(QWidget *parent = nullptr);
    ~LxDownloadWidget();

private:
    Ui::LxDownloadWidget ui;
};
