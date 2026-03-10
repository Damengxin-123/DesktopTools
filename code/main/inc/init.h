#pragma once

#include <qapplication.h>
#include <QFile>

// 加载qt样式表qss文件
void loadQss(const QString& qssFilePath) {

    QFile file(qssFilePath);
    if (file.open(QFile::ReadOnly)) {
        QString styleSheet = QLatin1String(file.readAll());
        qApp->setStyleSheet(styleSheet);
        file.close();
    }
    else {
        qDebug() << "Failed to open QSS file:" << qssFilePath;
    }
}