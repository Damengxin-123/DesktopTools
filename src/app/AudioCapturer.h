#pragma once

#include <QAudioFormat>
#include <QByteArray>
#include <QObject>
#include <QVector>
#include <atomic>
#include <thread>

class QAudioSource;
class QIODevice;
class QTimer;

// 录屏音频采集：按需采集麦克风（QAudioSource）与系统声音（WASAPI 环回捕获，
// 即扬声器正在播放的混合流），统一转换为主时钟格式（float32），
// 勾选两个来源时逐样本相加混音，经 chunkReady 交给录屏管线编码为 AAC。
//
// 主时钟格式由系统声音的混音格式决定（保证环回无需重采样），麦克风以同一格式
// 请求采集（WASAPI 共享模式会自动重采样）；仅录制麦克风时使用 48kHz 双声道。
// 暂停时丢弃到达的数据；恢复后先丢弃设备中滞留的旧数据再继续，
// 保证暂停期间的声音不混入视频。个别来源启动失败时继续其余来源，
// 全部失败时通过 start() 返回错误。
class AudioCapturer final : public QObject
{
    Q_OBJECT
public:
    explicit AudioCapturer(QObject* parent = nullptr);
    ~AudioCapturer() override;

    // 启动请求的来源；返回整体失败的说明，个别来源失败的原因经 warning 返回。
    QString start(bool microphone, bool system, QString* warning = nullptr);
    void stop();
    void pause();
    void resume();
    // 是否至少有一个来源在采集。
    bool isActive() const { return m_microphoneActive || m_systemActive; }
    // 主时钟格式（混合输出的 QAudioFormat）。
    const QAudioFormat& format() const { return m_format; }
signals:
    // 一段主时钟格式的 float32 采样（单源直出或双源混音），仅录制中产生。
    void chunkReady(const QByteArray& chunk);
private:
    // 麦克风定时拉取（主线程）。
    void appendMicrophone();
    // 系统声音线程投递的一段采样（经队列信号回到主线程）。
    void appendSystem(const QByteArray& chunk);
    // 从待混音队列中取出可混合的部分发出。
    void emitMixed();
    // 任意来源格式 → 主时钟格式（int/float 转 float、声道映射、线性重采样）。
    QByteArray convertToMaster(const QByteArray& data, const QAudioFormat& sourceFormat) const;

    // 主时钟格式与各来源的激活状态。
    QAudioFormat m_format;
    bool m_microphoneActive = false;
    bool m_systemActive = false;
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_stopped{false};

    // 麦克风：主线程定时拉取。
    QAudioSource* m_microphone = nullptr;
    QIODevice* m_microphoneDevice = nullptr;
    QTimer* m_microphoneTimer = nullptr;
    QAudioFormat m_microphoneFormat;

    // 系统声音：WASAPI 环回捕获线程。
    std::thread m_systemThread;

    // 待混音队列（主时钟格式 float32，主线程专用）。
    QVector<float> m_microphoneBuffer;
    QVector<float> m_systemBuffer;
};
