#include <QMimeData>
#include <QDateTime>
#include <QFileInfo>
#include "LxTextEdit.h"

void LxTextEdit::insertFromMimeData(const QMimeData* source)
{
    QTextCursor cursor = textCursor();
    int imageWidth = this->width() - 20; // 设置图片最大宽度为编辑器宽度减去边距
    // 粘贴截图/图片数据
    if (source->hasImage()) {
        QImage image = qvariant_cast<QImage>(source->imageData());

        // 生成唯一资源名
        QString imageName = QString("image_%1").arg(QDateTime::currentMSecsSinceEpoch()) + ".png";

        QString imagePath = getResourcePath();

        image.save(imagePath + imageName); // 保存图片到资源路径

        // 插入一个显示为缩放尺寸的图像格式（不影响原始资源）
        QTextImageFormat imageFormat;
        imageFormat.setName("images/" + imageName);

        // 设置缩放显示尺寸（例如最大宽度为 300px）

        if (image.width() > imageWidth) {
            double scaleFactor = static_cast<double>(imageWidth) / image.width();
            imageFormat.setWidth(image.width() * scaleFactor);
            imageFormat.setHeight(image.height() * scaleFactor);
        }

        cursor.insertImage(imageFormat);
    }

    // 粘贴图片文件
    else if (source->hasUrls()) {
        for (const QUrl& url : source->urls()) {
            QString localPath = url.toLocalFile();
            QImage image(localPath);
            if (!image.isNull()) {

                // 生成唯一资源名
                QString imageName = QString("image_%1").arg(QDateTime::currentMSecsSinceEpoch()) + ".png";

                QString imagePath = getResourcePath();

                image.save(imagePath + imageName); // 保存图片到资源路径

                // 插入一个显示为缩放尺寸的图像格式（不影响原始资源）
                QTextImageFormat imageFormat;
                imageFormat.setName("images/" + imageName);

                if (image.width() > imageWidth) {
                    double scaleFactor = static_cast<double>(imageWidth) / image.width();
                    imageFormat.setWidth(image.width() * scaleFactor);
                    imageFormat.setHeight(image.height() * scaleFactor);
                }

                cursor.insertImage(imageFormat);
            }
            else {
                cursor.insertText(localPath);
            }
        }
    }

    // 粘贴文本/HTML
    else {
        QTextEdit::insertFromMimeData(source);
    }
}

QString LxTextEdit::getResourcePath()
{
    // 获取提前保存的路径
    QString imagePath = document()->baseUrl().toString() + "images/";

    // 去掉 file:///
    imagePath.replace("file:///", ""); 

    return imagePath;
}

void LxTextEdit::updateImageSize()
{
    QTextDocument* doc = this->document();
    QTextCursor cursor(doc);

    cursor.movePosition(QTextCursor::Start);
    while (!cursor.atEnd()) {
        cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        QTextCharFormat format = cursor.charFormat();

        if (format.isImageFormat()) {
            QTextImageFormat imgFormat = format.toImageFormat();

            // 只更新宽度，高度自动按比例缩放
            QString imgName = imgFormat.name(); // image src

            imgName = doc->baseUrl().toString() + imgName;
            imgName.replace("file:///", "");
            QImage img(imgName); // 加载原图
            if (img.isNull())
            {
                continue;
            }

            int newWidth = this->viewport()->width() - 24; // 留出边距
            int newHeight = img.height() * newWidth / img.width();

            imgFormat.setWidth(newWidth);
            imgFormat.setHeight(newHeight);

            cursor.setCharFormat(imgFormat); // 更新格式
        }
    }
}

void LxTextEdit::resizeEvent(QResizeEvent* event)
{
    QTextEdit::resizeEvent(event);

}
