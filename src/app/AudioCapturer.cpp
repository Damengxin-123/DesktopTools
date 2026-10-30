#include "AudioCapturer.h"

#include <QAudioDevice>
#include <QAudioSource>
#include <QElapsedTimer>
#include <QIODevice>
#include <QMediaDevices>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <cmath>

#ifdef Q_OS_WIN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#endif

namespace {
// 麦克风拉取与发送的节拍。
constexpr int ChunkMilliseconds = 20;
// 主时钟格式的默认采样率（仅麦克风时使用；带系统声音时以系统混音格式为准）。
constexpr int DefaultSampleRate = 48000;
constexpr int DefaultChannels = 2;
// 单个来源的待混音上限（交错采样数，约 2 秒），防止另一来源停滞时内存无限增长。
constexpr int MaximumSourceSamples = DefaultSampleRate * 2 * DefaultChannels;
// 两个 float 逐样本相加并夹取到 [-1, 1]。
inline float mixSample(float a, float b)
{
    return std::clamp(a + b, -1.0f, 1.0f);
}
// 追加一段 float32 交错采样，超过上限时丢弃最旧数据。
void appendToBuffer(QVector<float>& buffer, const QByteArray& chunk)
{
    const int count = chunk.size() / int(sizeof(float));
    const float* samples = reinterpret_cast<const float*>(chunk.constData());
    for (int index = 0; index < count; ++index)
        buffer.append(samples[index]);
    if (buffer.size() > MaximumSourceSamples)
        buffer.remove(0, buffer.size() - MaximumSourceSamples);
}
} // namespace

AudioCapturer::AudioCapturer(QObject* parent)
    : QObject(parent)
{
}

AudioCapturer::~AudioCapturer()
{
    stop();
}

QString AudioCapturer::start(bool microphone, bool system, QString* warning)
{
    m_stopped = false;
    m_paused = false;
    m_format.setSampleRate(DefaultSampleRate);
    m_format.setChannelCount(DefaultChannels);
    m_format.setSampleFormat(QAudioFormat::Float);
    QStringList failures;

#ifdef Q_OS_WIN
    // 先启动系统声音环回：主时钟格式的采样率跟随系统混音格式，避免对环回流重采样。
    if (system) {
        m_stopped = false;
        m_systemThread = std::thread([this]() {
            // COM 与 WASAPI 接口都必须在使用它们的线程上初始化与释放。
            const bool comInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
            IMMDeviceEnumerator* enumerator = nullptr;
            IMMDevice* endpoint = nullptr;
            IAudioClient* client = nullptr;
            IAudioCaptureClient* capture = nullptr;
            WAVEFORMATEX* mixFormat = nullptr;
            do {
                if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)))
                    || !enumerator
                    || FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &endpoint))
                    || !endpoint
                    || FAILED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                        reinterpret_cast<void**>(&client)))
                    || !client
                    || FAILED(client->GetMixFormat(&mixFormat)) || !mixFormat
                    || FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                        2000000, 0, mixFormat, nullptr))
                    || FAILED(client->GetService(__uuidof(IAudioCaptureClient),
                        reinterpret_cast<void**>(&capture)))
                    || !capture
                    || FAILED(client->Start()))
                    break;
                // 主时钟采样率以系统混音格式为准；此标志即系统声音可用的开关。
                m_format.setSampleRate(mixFormat->nSamplesPerSec);
                m_systemActive = true;
                while (!m_stopped) {
                    Sleep(10);
                    UINT32 packetBytes = 0;
                    while (SUCCEEDED(capture->GetNextPacketSize(&packetBytes)) && packetBytes > 0) {
                        BYTE* data = nullptr;
                        UINT32 frames = 0;
                        DWORD flags = 0;
                        if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                            break;
                        if (!m_paused && frames > 0) {
                            // 混音格式解析为 float32、取前两声道（单声道时复制）；
                            // SILENT 包同样转发（零采样），保证音频时间线持续推进。
                            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                            const WORD bytesPerSample = mixFormat->wBitsPerSample / 8;
                            const WORD sourceChannels = qMax<WORD>(1, mixFormat->nChannels);
                            const bool isFloat = mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT
                                || (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE && bytesPerSample == 4);
                            QVector<float> samples;
                            samples.reserve(qint64(frames) * DefaultChannels);
                            for (UINT32 frame = 0; frame < frames; ++frame) {
                                const BYTE* frameData = data + qint64(frame) * sourceChannels * bytesPerSample;
                                float values[DefaultChannels] = {0.0f, 0.0f};
                                if (!silent) {
                                    for (WORD channel = 0; channel < qMin<WORD>(sourceChannels, DefaultChannels); ++channel) {
                                        const BYTE* cursor = frameData + qint64(channel) * bytesPerSample;
                                        float value = 0.0f;
                                        if (isFloat)
                                            value = *reinterpret_cast<const float*>(cursor);
                                        else if (bytesPerSample == 2)
                                            value = *reinterpret_cast<const short*>(cursor) / 32768.0f;
                                        else if (bytesPerSample == 3)
                                            value = ((cursor[0] << 8) | (cursor[1] << 16) | (cursor[2] << 24))
                                                / 2147483648.0f;
                                        else if (bytesPerSample == 4)
                                            value = *reinterpret_cast<const int*>(cursor) / 2147483648.0f;
                                        values[channel] = std::clamp(value, -1.0f, 1.0f);
                                    }
                                }
                                samples.append(values[0]);
                                samples.append(sourceChannels == 1 ? values[0] : values[1]);
                            }
                            emit chunkReady(QByteArray(
                                reinterpret_cast<const char*>(samples.constData()),
                                int(samples.size()) * int(sizeof(float))));
                        }
                        capture->ReleaseBuffer(frames);
                    }
                }
            } while (false);
            m_systemActive = false;
            if (client && capture) {
                client->Stop();
                capture->Release();
            }
            if (client)
                client->Release();
            if (mixFormat)
                CoTaskMemFree(mixFormat);
            if (endpoint)
                endpoint->Release();
            if (enumerator)
                enumerator->Release();
            if (comInitialized)
                CoUninitialize();
        });
        // 线程内初始化失败时短暂等待后确认；成功后主时钟格式即已确定。
        QElapsedTimer probe;
        probe.start();
        while (!m_systemActive && probe.elapsed() < 500 && !m_stopped)
            QThread::msleep(5);
        if (!m_systemActive) {
            m_stopped = true;
            if (m_systemThread.joinable())
                m_systemThread.join();
            failures.append(QStringLiteral("系统声音（无法访问扬声器输出）"));
        }
    }
#else
    if (system)
        failures.append(QStringLiteral("系统声音（当前平台不支持）"));
#endif

    if (microphone) {
        const QAudioDevice device = QMediaDevices::defaultAudioInput();
        if (device.isNull()) {
            failures.append(QStringLiteral("麦克风（未找到输入设备）"));
        } else {
            m_microphone = new QAudioSource(device, m_format, this);
            m_microphoneDevice = m_microphone->start();
            if (!m_microphoneDevice || m_microphone->error() != QAudio::NoError) {
                failures.append(QStringLiteral("麦克风（无法打开采集设备，错误码 %1）")
                    .arg(int(m_microphone->error())));
                m_microphone->deleteLater();
                m_microphone = nullptr;
                m_microphoneDevice = nullptr;
            } else {
                m_microphoneFormat = m_microphone->format();
                m_microphoneActive = true;
                m_microphoneTimer = new QTimer(this);
                m_microphoneTimer->setInterval(ChunkMilliseconds);
                connect(m_microphoneTimer, &QTimer::timeout, this, &AudioCapturer::appendMicrophone);
                m_microphoneTimer->start();
            }
        }
    }

    if (warning) {
        *warning = failures.isEmpty()
            ? QString()
            : QStringLiteral("以下音频来源无法使用，本次录制将跳过它们：%1。")
                  .arg(failures.join(QStringLiteral("、")));
    }
    if (!isActive()) {
        stop();
        return QStringLiteral("无法启动任何音频来源，请检查系统音频设备或调整录屏音频选项。");
    }
    return {};
}

void AudioCapturer::stop()
{
    m_stopped = true;
#ifdef Q_OS_WIN
    if (m_systemThread.joinable())
        m_systemThread.join();
#endif
    m_systemActive = false;
    if (m_microphoneTimer) {
        m_microphoneTimer->stop();
        m_microphoneTimer->deleteLater();
        m_microphoneTimer = nullptr;
    }
    if (m_microphone) {
        m_microphone->stop();
        m_microphone->deleteLater();
        m_microphone = nullptr;
    }
    m_microphoneDevice = nullptr;
    m_microphoneActive = false;
    m_microphoneBuffer.clear();
    m_systemBuffer.clear();
}

void AudioCapturer::pause()
{
    m_paused = true;
}

void AudioCapturer::resume()
{
    if (!m_paused.exchange(false))
        return;
    // 丢弃暂停期间滞留在设备中的数据：麦克风重启采集，环回侧旧包随暂停标志丢弃。
    if (m_microphone) {
        m_microphone->stop();
        m_microphoneDevice = m_microphone->start();
        if (m_microphoneDevice)
            m_microphoneFormat = m_microphone->format();
    }
    m_microphoneBuffer.clear();
    m_systemBuffer.clear();
}

void AudioCapturer::appendMicrophone()
{
    if (!m_microphoneDevice)
        return;
    const QByteArray data = m_microphoneDevice->readAll();
    if (data.isEmpty() || m_paused)
        return;
    appendToBuffer(m_microphoneBuffer, convertToMaster(data, m_microphoneFormat));
    emitMixed();
}

void AudioCapturer::appendSystem(const QByteArray& chunk)
{
    if (m_paused || chunk.isEmpty())
        return;
    appendToBuffer(m_systemBuffer, chunk);
    emitMixed();
}

void AudioCapturer::emitMixed()
{
    if (m_paused)
        return;
    QVector<float> mixed;
    if (m_microphoneActive && m_systemActive) {
        const int common = qMin(m_microphoneBuffer.size(), m_systemBuffer.size());
        if (common <= 0)
            return;
        mixed.resize(common);
        for (int index = 0; index < common; ++index)
            mixed[index] = mixSample(m_microphoneBuffer.at(index), m_systemBuffer.at(index));
        m_microphoneBuffer.remove(0, common);
        m_systemBuffer.remove(0, common);
    } else if (m_microphoneActive) {
        mixed.swap(m_microphoneBuffer);
        m_microphoneBuffer.clear();
    } else if (m_systemActive) {
        mixed.swap(m_systemBuffer);
        m_systemBuffer.clear();
    } else {
        return;
    }
    if (mixed.isEmpty())
        return;
    emit chunkReady(QByteArray(
        reinterpret_cast<const char*>(mixed.constData()), mixed.size() * int(sizeof(float))));
}

QByteArray AudioCapturer::convertToMaster(const QByteArray& data, const QAudioFormat& sourceFormat) const
{
    const int sourceChannels = qMax(1, sourceFormat.channelCount());
    const int sourceRate = qMax(1, sourceFormat.sampleRate());
    const int sourceBytesPerSample = sourceFormat.bytesPerSample();
    const int frameCount = sourceBytesPerSample > 0 ? data.size() / (sourceBytesPerSample * sourceChannels) : 0;
    if (frameCount <= 0)
        return {};
    const int targetChannels = qMax(1, m_format.channelCount());
    const int targetRate = qMax(1, m_format.sampleRate());
    const int targetFrames = int(qint64(frameCount) * targetRate / sourceRate);
    QVector<float> out(qint64(targetFrames) * targetChannels);
    const char* base = data.constData();
    for (int frame = 0; frame < targetFrames; ++frame) {
        // 线性重采样定位到源帧；声道多于目标时映射前两路，单声道时复制。
        const double position = double(frame) * sourceRate / targetRate;
        const int index0 = qBound(0, int(position), frameCount - 1);
        const int index1 = qMin(index0 + 1, frameCount - 1);
        const float fraction = float(position - index0);
        for (int channel = 0; channel < targetChannels; ++channel) {
            const int sourceChannel = qMin(channel, sourceChannels - 1);
            auto decode = [&](int frameIndex) -> float {
                const char* cursor = base
                    + (qint64(frameIndex) * sourceChannels + sourceChannel) * sourceBytesPerSample;
                switch (sourceFormat.sampleFormat()) {
                case QAudioFormat::Float:
                    return *reinterpret_cast<const float*>(cursor);
                case QAudioFormat::Int16:
                    return *reinterpret_cast<const short*>(cursor) / 32768.0f;
                case QAudioFormat::Int32:
                    return *reinterpret_cast<const int*>(cursor) / 2147483648.0f;
                case QAudioFormat::UInt8:
                    return (*reinterpret_cast<const unsigned char*>(cursor) - 128) / 128.0f;
                default:
                    return 0.0f;
                }
            };
            out[qint64(frame) * targetChannels + channel] =
                std::clamp(decode(index0) * (1.0f - fraction) + decode(index1) * fraction, -1.0f, 1.0f);
        }
    }
    return QByteArray(reinterpret_cast<const char*>(out.constData()),
        int(out.size()) * int(sizeof(float)));
}
