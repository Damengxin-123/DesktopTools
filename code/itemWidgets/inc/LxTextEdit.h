#pragma once

#include <QTextEdit>

class LxTextEdit : public QTextEdit
{
    Q_OBJECT

public:
    using QTextEdit::QTextEdit;

    // 手动更新显示的图片的大小，避免缩放窗口时太卡
    void updateImageSize();

    // 更新大小
    void resizeEvent(QResizeEvent* event) override;

protected:
    // 重写插入数据方法，处理图片和文本的粘贴
    void insertFromMimeData(const QMimeData* source) override;

    // 获取当前加载资源文件的路径
    QString getResourcePath();
    
};
