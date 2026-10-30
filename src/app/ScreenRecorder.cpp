#include "ScreenRecorder.h"

#include "AudioCapturer.h"

#include <QAudioBuffer>
#include <QAudioBufferInput>
#include <QDateTime>
#include <QImage>
#include <QMediaCaptureSession>
#include <QMediaFormat>
#include <QMediaRecorder>
#include <QScreen>
#include <QScreenCapture>
#include <QTimer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoFrameInput>
#include <QVideoSink>
#include <algorithm>

namespace {
// 待编码帧队列按字节封顶（约 192 MB，1080p 约 23 帧）：编码器启动或过载时
// 缓冲若干秒画面保护不丢帧，超过后丢弃新帧避免内存膨胀。
constexpr qint64 MaximumPendingBytes = 192LL * 1024 * 1024;
// 待编码音频队列字节上限（约 20 秒的 48kHz 双声道 float）。
constexpr qint64 MaximumPendingAudioBytes = 8LL * 1024 * 1024;
// 帧率提示边界：低于 24 画面卡顿，高于 60 时软件编码难以跟上。
constexpr qreal MinimumFrameRate = 24.0;
constexpr qreal MaximumFrameRate = 60.0;
constexpr qreal DefaultFrameRate = 30.0;

qint64 microsOf(qint64 nanoseconds) { return nanoseconds / 1000; }

// 队列字节量的估算值：本管线中的帧都来自 QImage（4 字节每像素）。
qint64 frameBytesEstimate(const QVideoFrame& frame)
{
    const QSize size = frame.size();
    return qint64(size.width()) * qMax(1, size.height()) * 4;
}
} // namespace

ScreenRecorder::ScreenRecorder(QObject* parent)
    : QObject(parent)
{
    m_tickTimer = new QTimer(this);
    m_tickTimer->setInterval(200);
    connect(m_tickTimer, &QTimer::timeout, this, [this]() { emit tick(recordedMilliseconds()); });
}

qreal ScreenRecorder::frameRateHint(QScreen* screen)
{
    const qreal refresh = screen ? screen->refreshRate() : 0.0;
    if (refresh <= 0)
        return DefaultFrameRate;
    return std::clamp(refresh, MinimumFrameRate, MaximumFrameRate);
}

bool ScreenRecorder::start(QScreen* screen, const QRectF& selection, const QSizeF& overlayLogicalSize,
    const QString& outputPath, bool recordMicrophone, bool recordSystemSound,
    QString* error, QString* warning)
{
    const auto bail = [error](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    if (m_started || m_stopping)
        return bail(QStringLiteral("录制已经进行中。"));
    if (!screen || selection.isEmpty() || overlayLogicalSize.isEmpty()
        || overlayLogicalSize.width() <= 0 || overlayLogicalSize.height() <= 0)
        return bail(QStringLiteral("录屏选区无效。"));
    if (outputPath.isEmpty())
        return bail(QStringLiteral("缺少输出文件路径。"));

    m_selection = selection;
    m_overlayLogicalSize = overlayLogicalSize;
    m_outputPath = outputPath;
    m_crop = QRect();
    m_frameSize = QSize();
    m_recordedUs = 0;
    m_paused = false;
    m_pending.clear();
    m_pendingBytes = 0;
    m_pendingAudio.clear();
    m_pendingAudioBytes = 0;
    m_audioUs = 0;
    m_thumbnail = QImage();
    m_started = false;
    m_stopping = false;
    m_failed = false;
    m_frameIntervalUs = qint64(1000000.0 / frameRateHint(screen));

    // 抓屏会话：整屏帧只进入自定义 sink，由本类裁剪后转发。
    m_sink = new QVideoSink(this);
    connect(m_sink, &QVideoSink::videoFrameChanged, this, &ScreenRecorder::onSourceFrame);
    m_capture = new QScreenCapture(this);
    // 显式绑定选区所在屏幕，避免抓屏器使用默认主屏幕。
    m_capture->setScreen(screen);
    m_captureSession = new QMediaCaptureSession(this);
    m_captureSession->setScreenCapture(m_capture);
    m_captureSession->setVideoSink(m_sink);

    // 编码会话：裁剪后的帧经 QVideoFrameInput 交给 QMediaRecorder。
    m_frameInput = new QVideoFrameInput(this);
    connect(m_frameInput, &QVideoFrameInput::readyToSendVideoFrame,
        this, &ScreenRecorder::sendQueued);
    m_recorder = new QMediaRecorder(this);
    m_encodeSession = new QMediaCaptureSession(this);
    m_encodeSession->setVideoFrameInput(m_frameInput);
    m_encodeSession->setRecorder(m_recorder);

    // 音频（可选）：混音后的采样经 QAudioBufferInput 进入同一编码器（AAC）。
    if (recordMicrophone || recordSystemSound) {
        m_audioCapturer = new AudioCapturer(this);
        QString audioWarning;
        const QString audioError = m_audioCapturer->start(recordMicrophone, recordSystemSound, &audioWarning);
        if (!audioError.isEmpty()) {
            m_audioCapturer->deleteLater();
            m_audioCapturer = nullptr;
            return bail(audioError);
        }
        if (warning)
            *warning = audioWarning;
        m_audioInput = new QAudioBufferInput(this);
        connect(m_audioInput, &QAudioBufferInput::readyToSendAudioBuffer,
            this, &ScreenRecorder::sendQueuedAudio);
        connect(m_audioCapturer, &AudioCapturer::chunkReady, this, &ScreenRecorder::onAudioChunk);
        m_encodeSession->setAudioBufferInput(m_audioInput);
    }

    QMediaFormat format(QMediaFormat::MPEG4);
    format.setVideoCodec(QMediaFormat::VideoCodec::H264);
    if (recordMicrophone || recordSystemSound)
        format.setAudioCodec(QMediaFormat::AudioCodec::AAC);
    m_recorder->setMediaFormat(format);
    // 画质优先：恒定质量模式 + 最高质量档，分辨率与实际帧率由输入帧决定。
    m_recorder->setQuality(QMediaRecorder::Quality::VeryHighQuality);
    m_recorder->setEncodingMode(QMediaRecorder::EncodingMode::ConstantQualityEncoding);
    m_recorder->setVideoFrameRate(frameRateHint(screen));
    m_recorder->setOutputLocation(QUrl::fromLocalFile(outputPath));

    connect(m_recorder, &QMediaRecorder::errorOccurred, this,
        [this](QMediaRecorder::Error, const QString& errorString) {
            if (m_stopping || m_failed)
                return;
            m_failed = true;
            finishEncoding(false, errorString);
        });
    connect(m_recorder, &QMediaRecorder::recorderStateChanged, this, [this]() {
        if (m_recorder->recorderState() != QMediaRecorder::StoppedState || m_failed)
            return;
        // 正常结束：编码器已写完 MP4 尾部，汇总结果。
        QVariantMap result;
        result.insert(QStringLiteral("path"), m_outputPath);
        result.insert(QStringLiteral("width"), m_crop.width());
        result.insert(QStringLiteral("height"), m_crop.height());
        result.insert(QStringLiteral("durationMs"),
            qMax<qint64>(m_recorder->duration(), recordedMilliseconds()));
        result.insert(QStringLiteral("thumbnail"), m_thumbnail);
        emit finished(result);
    });
    connect(m_capture, &QScreenCapture::errorOccurred, this,
        [this](QScreenCapture::Error, const QString& errorString) {
            if (m_stopping || m_failed)
                return;
            m_failed = true;
            finishEncoding(false, errorString);
        });

    m_clock.start();
    m_capture->setActive(true);
    m_recorder->record();
    if (m_recorder->error() != QMediaRecorder::NoError) {
        m_failed = true;
        finishEncoding(false, m_recorder->errorString());
        return false;
    }
    if (m_audioInput && m_audioCapturer) {
        // 立即补发一小块静音：MP4 头要等音频流初始化才能写出；若来源长时间
        // 无声（如系统完全静音时环回不产包），视频帧会被持续拒绝。
        QByteArray silence(1024 * m_audioCapturer->format().channelCount()
                * int(sizeof(float)), 0);
        onAudioChunk(silence);
    }
    m_tickTimer->start();
    return true;
}

qint64 ScreenRecorder::recordedMilliseconds() const
{
    // 暂停时冻结在累计值；录制中加上当前段经过的时间。
    const qint64 current = m_paused ? 0 : microsOf(m_clock.nsecsElapsed());
    return (m_recordedUs + current) / 1000;
}

void ScreenRecorder::pause()
{
    if (m_paused || m_stopping || m_failed || !m_started)
        return;
    m_recordedUs += microsOf(m_clock.nsecsElapsed());
    m_paused = true;
    if (m_audioCapturer)
        m_audioCapturer->pause();
}

void ScreenRecorder::resume()
{
    if (!m_paused || m_stopping || m_failed)
        return;
    m_paused = false;
    m_clock.restart();
    if (m_audioCapturer)
        m_audioCapturer->resume();
}

void ScreenRecorder::stop()
{
    if (m_stopping || m_failed)
        return;
    m_stopping = true;
    finishEncoding(true, {});
}

void ScreenRecorder::finishEncoding(bool success, const QString& error)
{
    m_tickTimer->stop();
    if (m_capture)
        m_capture->setActive(false);
    if (m_audioCapturer)
        m_audioCapturer->stop();
    m_pending.clear();
    m_pendingBytes = 0;
    m_pendingAudio.clear();
    m_pendingAudioBytes = 0;
    if (m_recorder && m_recorder->recorderState() != QMediaRecorder::StoppedState)
        m_recorder->stop();
    // stop() 是异步的：正常结束时由 recorderStateChanged 汇总结果；出错时立即报告，
    // 半成品文件由持有方删除。成功但状态已停（比如录了 0 帧）也直接汇总。
    if (!success) {
        emit failed(error.isEmpty() ? QStringLiteral("录制失败。") : error);
        return;
    }
    if (!m_recorder || m_recorder->recorderState() == QMediaRecorder::StoppedState) {
        QVariantMap result;
        result.insert(QStringLiteral("path"), m_outputPath);
        result.insert(QStringLiteral("width"), m_crop.width());
        result.insert(QStringLiteral("height"), m_crop.height());
        result.insert(QStringLiteral("durationMs"), recordedMilliseconds());
        result.insert(QStringLiteral("thumbnail"), m_thumbnail);
        emit finished(result);
    }
}

void ScreenRecorder::onSourceFrame(const QVideoFrame& frame)
{
    if (!frame.isValid())
        return;
    if (m_failed)
        return;
    if (!m_started) {
        m_started = true;
        emit started();
    }
    if (m_paused || m_stopping)
        return; // 暂停期间与停止后的帧不进入视频。
    const QVideoFrame cropped = croppedFrame(frame);
    if (!cropped.isValid())
        return;
    if (m_pending.isEmpty() && m_frameInput->sendVideoFrame(cropped))
        return;
    if (m_pendingBytes + frameBytesEstimate(cropped) > MaximumPendingBytes)
        return; // 编码器过载：丢新帧避免内存膨胀。
    m_pendingBytes += frameBytesEstimate(cropped);
    m_pending.enqueue(cropped);
}

void ScreenRecorder::sendQueued()
{
    while (!m_pending.isEmpty()) {
        if (!m_frameInput->sendVideoFrame(m_pending.head()))
            return;
        m_pendingBytes -= frameBytesEstimate(m_pending.dequeue());
    }
}

void ScreenRecorder::onAudioChunk(const QByteArray& chunk)
{
    if (!m_audioInput || m_failed || m_stopping || m_paused || chunk.isEmpty())
        return;
    const QAudioFormat& format = m_audioCapturer->format();
    QAudioBuffer buffer(chunk, format, m_audioUs);
    if (!buffer.isValid())
        return;
    // 音频时间线按采样数推进，与视频共用“不含暂停”的口径。
    m_audioUs += qint64(1000000.0 * buffer.frameCount() / qMax(1, format.sampleRate()));
    if (m_pendingAudio.isEmpty() && m_audioInput->sendAudioBuffer(buffer))
        return;
    if (m_pendingAudioBytes + chunk.size() > MaximumPendingAudioBytes)
        return; // 编码器过载：丢新音频避免内存膨胀。
    m_pendingAudioBytes += chunk.size();
    m_pendingAudio.enqueue(buffer);
}

void ScreenRecorder::sendQueuedAudio()
{
    while (!m_pendingAudio.isEmpty()) {
        if (!m_audioInput->sendAudioBuffer(m_pendingAudio.head()))
            return;
        m_pendingAudioBytes -= m_pendingAudio.dequeue().byteCount();
    }
}

QVideoFrame ScreenRecorder::croppedFrame(const QVideoFrame& frame)
{
    const QSize size = frame.size();
    if (size.isEmpty())
        return {};
    if (size != m_frameSize) {
        // 按首帧（或分辨率变化后的帧）重新换算：逻辑选区 → 设备像素。
        m_frameSize = size;
        const qreal scaleX = double(size.width()) / qMax<qreal>(1.0, m_overlayLogicalSize.width());
        const qreal scaleY = double(size.height()) / qMax<qreal>(1.0, m_overlayLogicalSize.height());
        QRect crop(int(std::lround(m_selection.left() * scaleX)),
            int(std::lround(m_selection.top() * scaleY)),
            int(std::lround(m_selection.width() * scaleX)),
            int(std::lround(m_selection.height() * scaleY)));
        // H.264 的 4:2:0 采样要求偶数坐标与尺寸，先偶对齐再夹取到帧内。
        crop.setX(crop.x() - (crop.x() % 2));
        crop.setY(crop.y() - (crop.y() % 2));
        crop.setWidth(crop.width() - (crop.width() % 2));
        crop.setHeight(crop.height() - (crop.height() % 2));
        crop.setWidth(std::min(crop.width(), size.width() - crop.x()));
        crop.setHeight(std::min(crop.height(), size.height() - crop.y()));
        m_crop = crop;
        if (m_crop.width() < 2 || m_crop.height() < 2)
            return {};
    }
    // 落到 CPU 侧裁剪：toImage 覆盖任意后端帧格式，保证 QVideoFrameInput 可送。
    QImage image = frame.toImage();
    if (image.isNull())
        return {};
    image.setDevicePixelRatio(1.0);
    if (m_thumbnail.isNull()) {
        // 首帧作为列表缩略图，与截图历史的 320px 宽度一致。
        m_thumbnail = image.copy(m_crop).scaledToWidth(320, Qt::SmoothTransformation);
    }
    const QImage cropped = image.copy(m_crop);
    QVideoFrame result(cropped);
    // 时间戳排除暂停时段：暂停即停止送帧，恢复后从累计进度继续。
    const qint64 startUs = microsOf(m_clock.nsecsElapsed()) + m_recordedUs;
    result.setStartTime(startUs);
    result.setEndTime(startUs + qMax<qint64>(1000, m_frameIntervalUs));
    return result;
}
