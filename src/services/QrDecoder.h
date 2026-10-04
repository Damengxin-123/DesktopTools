#pragma once

#include <QImage>
#include <QString>

// 自包含的二维码识别实现：在灰度图中定位三个定位图案，重建（可能的）透视网格，
// 读取格式信息与数据码字并解码文本。面向完整性较高的二维码图片（截图、保存的
// 图片），不还原纠错码字，损坏模块过多时会明确报告失败。
namespace QrDecoder {

// 单次识别结果；text 非空表示成功，error 为可直接显示的中文原因。
struct Result {
    QString text;
    QString error;
    int version = 0; // 1–40；未识别到二维码时为 0。
};

// 识别图片中的第一个二维码；支持任意旋转、适度缩放、反色与轻微透视变形。
Result decode(const QImage& image);

}
