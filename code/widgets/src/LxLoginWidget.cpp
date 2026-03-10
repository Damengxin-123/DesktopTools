#include "LxLoginWidget.h"

LxLoginWidget::LxLoginWidget(QWidget *parent)
    : QWidget(parent)
{
    ui.setupUi(this);
}

LxLoginWidget::~LxLoginWidget()
{
}

void LxLoginWidget::on_uButQuitLogin_clicked()
{
    ui.uLoginTabWidget->setCurrentIndex(0);
}

void LxLoginWidget::on_uButLogin_clicked()
{
    ui.uLoginTabWidget->setCurrentIndex(1);
}
