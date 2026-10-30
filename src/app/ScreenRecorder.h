#pragma once

#include <QElapsedTimer>
#include <QAudioBuffer>
#include <QObject>
#include <QQueue>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QVariantMap>
#include <QVideoFrame>

class QScreen;
class QScreenCapture;
class QAudioBufferInput;
class AudioCapturer;
class QMediaCaptureSession;
class QMediaRecorder;
class QTimer;
class QVideoFrameInput;
class QVideoSink;

// 区域录屏管线：QScreenCapture（Windows 上走 Windows Graphics Capture，硬件级抓屏，
// 帧率跟随显示器刷新率）抓取整屏，把每一帧裁剪到选区后经 QVideoFrameInput 交给
// QMediaRecorder 编码为 MP4（H.264，优先使用系统硬件编码器）。
//
// 裁剪在帧级完成：抓屏会话只接一个自定义 QVideoSink，编码会话只接 QVideoFrameInput，
// 两个会话各司其职。暂停即停止送帧，恢复后时间戳从暂停前的进度继续，因此输出视频
// 不包含暂停期间的画面与时长；编码器来不及消费的帧进入有界队列，队满时丢弃新帧。
// 录制中控制条等悬浮窗口只在选区外显示，选区内画面保持纯净。
class ScreenRecorder final : public QObject
{
    Q_OBJECT
public:
    explicit ScreenRecorder(QObject* parent = nullptr);
    // 开始录制：selection 为选区在所在屏幕内的逻辑坐标（相对屏幕左上角），
    // overlayLogicalSize 为选区遮罩的逻辑尺寸，用于把选区映射到帧的设备像素。
    // outputPath 为最终 MP4 路径，目录必须已存在。recordMicrophone / recordSystemSound
    // 决定是否采集音频；个别来源失败时继续录制并经 warning 返回说明。
    // 失败返回 false 并填充 error。
    bool start(QScreen* screen, const QRectF& selection, const QSizeF& overlayLogicalSize,
        const QString& outputPath, bool recordMicrophone, bool recordSystemSound,
        QString* error = nullptr, QString* warning = nullptr);
    // 暂停送帧；恢复后时间戳无缝继续。
    void pause();
    void resume();
    bool isPaused() const { return m_paused; }
    // 结束录制并落盘；结果经 finished 或 failed 通知。
    void stop();
    // 有效录制时长（毫秒，不含暂停）。
    qint64 recordedMilliseconds() const;
    // 编码器提示帧率：显示器刷新率夹取到 [24, 60]；实际帧率跟随抓屏。
    static qreal frameRateHint(QScreen* screen);
signals:
    // 第一帧送入编码器时发出。
    void started();
    // 有效录制时长更新（约每 200 毫秒一次；暂停期间数值冻结）。
    void tick(qint64 recordedMs);
    // 落盘完成：path、width、height、durationMs、bytes 与首帧缩略图。
    void finished(const QVariantMap& result);
    // 管线或编码出错；此后不再发出其他信号。
    void failed(const QString& error);
private:
    // 抓屏新帧：裁剪、盖时间戳并尝试送入编码器；编码器忙时进队列。
    void onSourceFrame(const QVideoFrame& frame);
    // 队列中的帧继续送入编码器。
    void sendQueued();
    // 一段混音后的音频采样：盖时间戳并送入编码器，忙时进队列。
    void onAudioChunk(const QByteArray& chunk);
    // 队列中的音频继续送入编码器。
    void sendQueuedAudio();
    // 把一帧按选区裁剪并盖时间戳；选区随首帧的设备尺寸一次性换算。
    QVideoFrame croppedFrame(const QVideoFrame& frame);
    // 结束编码器并等待落盘（stop 与出错时共用）。
    void finishEncoding(bool success, const QString& error);

    QScreenCapture* m_capture = nullptr;
    QMediaCaptureSession* m_captureSession = nullptr;
    QVideoSink* m_sink = nullptr;
    QVideoFrameInput* m_frameInput = nullptr;
    QMediaCaptureSession* m_encodeSession = nullptr;
    QMediaRecorder* m_recorder = nullptr;
    QTimer* m_tickTimer = nullptr;
    // 音频（可选）：混音采集器与编码输入。
    AudioCapturer* m_audioCapturer = nullptr;
    QAudioBufferInput* m_audioInput = nullptr;

    // 选区（屏幕逻辑坐标）与遮罩逻辑尺寸，用于换算帧内裁剪矩形。
    QRectF m_selection;
    QSizeF m_overlayLogicalSize;
    // 首帧换算出的设备像素裁剪矩形；帧尺寸变化时按比例重算。
    QRect m_crop;
    QSize m_frameSize;
    // 输出文件路径与录制开始时间。
    QString m_outputPath;
    QElapsedTimer m_clock;
    // 暂停前累计的有效时长（微秒）与是否暂停。
    qint64 m_recordedUs = 0;
    bool m_paused = false;
    // 帧间隔（微秒），由编码器提示帧率得出。
    qint64 m_frameIntervalUs = 33333;
    // 待送入编码器的帧队列；按字节有界，溢出丢新帧。
    QQueue<QVideoFrame> m_pending;
    qint64 m_pendingBytes = 0;
    // 待送入编码器的音频队列与音频时间线（微秒，不含暂停）。
    QQueue<QAudioBuffer> m_pendingAudio;
    qint64 m_pendingAudioBytes = 0;
    qint64 m_audioUs = 0;
    // 首帧缩略图（已裁剪）。
    QImage m_thumbnail;
    // 状态：首帧前 / 录制中 / 停止中 / 已结束。
    bool m_started = false;
    bool m_stopping = false;
    bool m_failed = false;
};
