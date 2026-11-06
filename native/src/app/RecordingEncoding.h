#pragma once

#include <QMediaRecorder>
#include <QSize>
#include <algorithm>
#include <cmath>

// 录屏共用编码策略，供实际录制和合成帧验证复用。
namespace RecordingEncoding {
// 将选区等比缩小到指定档位的横屏或竖屏边界；零表示原始尺寸，不放大小选区。
inline QSize outputSize(const QSize& source, int resolution)
{
    if (source.width() < 2 || source.height() < 2)
        return {};
    QSize result = source;
    if (resolution > 0) {
        const int longEdge = resolution * 16 / 9;
        const QSize bounds = source.width() >= source.height()
            ? QSize(longEdge, resolution) : QSize(resolution, longEdge);
        if (source.width() > bounds.width() || source.height() > bounds.height())
            result.scale(bounds, Qt::KeepAspectRatio);
    }
    return QSize(std::max(2, result.width() & ~1), std::max(2, result.height() & ~1));
}

// 按输出像素数分配 H.264 平均码率；1080p 约 4 Mbps，音频为 128 kbps AAC。
inline void configureRecorder(QMediaRecorder& recorder, const QSize& size, qreal frameRate)
{
    const int bitRate = int(std::clamp(std::llround(double(size.width()) * size.height()
        * frameRate * 0.065), 500000LL, 16000000LL));
    recorder.setEncodingMode(QMediaRecorder::AverageBitRateEncoding);
    recorder.setVideoBitRate(bitRate);
    recorder.setAudioBitRate(128000);
    recorder.setVideoResolution(size);
    recorder.setVideoFrameRate(frameRate);
}
} // namespace RecordingEncoding
