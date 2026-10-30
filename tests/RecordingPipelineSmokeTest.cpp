#include <QAudioBuffer>
#include <QAudioBufferInput>
#include <QAudioFormat>
#include <QElapsedTimer>
#include <QImage>
#include <QMediaCaptureSession>
#include <QMediaFormat>
#include <QMediaRecorder>
#include <QTemporaryDir>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoFrameInput>
#include <QtTest>
#include <cmath>

// 录屏编码半边管线的冒烟测试：合成帧经 QVideoFrameInput、合成 PCM 经
// QAudioBufferInput 送入 QMediaRecorder，按 ScreenRecorder 相同的 MP4/H.264/AAC
// 恒定高质量设置编码落盘。验证运行环境中存在可用的 H.264 与 AAC 编码器、
// 时间戳被正确接受以及 MP4 正常收尾。
// WGC 抓屏与 WASAPI 环回半边需要真实桌面与音频设备，不在 offscreen 测试范围内。
class RecordingPipelineSmokeTest final : public QObject
{
    Q_OBJECT

private slots:
    // 合成 2 秒 30fps 视频帧与 0.6 秒音频，编码出的 MP4 存在、非空且时长正确。
    void encodesSyntheticFrames();
};

void RecordingPipelineSmokeTest::encodesSyntheticFrames()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString output = dir.filePath(QStringLiteral("out.mp4"));

    QVideoFrameInput input;
    QAudioBufferInput audioInput;
    QMediaCaptureSession session;
    session.setVideoFrameInput(&input);
    session.setAudioBufferInput(&audioInput);
    QMediaRecorder recorder;
    session.setRecorder(&recorder);
    QMediaFormat format(QMediaFormat::MPEG4);
    format.setVideoCodec(QMediaFormat::VideoCodec::H264);
    format.setAudioCodec(QMediaFormat::AudioCodec::AAC);
    recorder.setMediaFormat(format);
    recorder.setQuality(QMediaRecorder::Quality::VeryHighQuality);
    recorder.setEncodingMode(QMediaRecorder::EncodingMode::ConstantQualityEncoding);
    recorder.setVideoFrameRate(30);
    recorder.setOutputLocation(QUrl::fromLocalFile(output));

    QString failure;
    connect(&recorder, &QMediaRecorder::errorOccurred,
        [&failure](QMediaRecorder::Error, const QString& message) { failure = message; });
    recorder.record();
    if (!failure.isEmpty())
        QFAIL(qPrintable(failure));

    // 视频 60 帧（2 秒）与音频 30 段（约 0.64 秒）交错发送：后端在首帧写 MP4 头时
    // 需要音频流就绪，真实管线中两路也是并行到达的。
    QElapsedTimer clock;
    clock.start();
    QAudioFormat audioFormat;
    audioFormat.setSampleRate(48000);
    audioFormat.setChannelCount(2);
    audioFormat.setSampleFormat(QAudioFormat::Float);
    constexpr int framesPerBuffer = 1024;
    int sentAudio = 0;
    for (int index = 0; index < 60; ++index) {
        QImage image(320, 240, QImage::Format_ARGB32);
        image.fill(QColor::fromHsv(index * 6 % 360, 200, 200));
        QVideoFrame frame(image);
        frame.setStartTime(index * 33333);
        frame.setEndTime((index + 1) * 33333);
        while (!input.sendVideoFrame(frame)) {
            if (clock.elapsed() > 30000)
                QFAIL(QStringLiteral("sendVideoFrame 未被接受：第 %1 帧").arg(index).toUtf8());
            QTest::qWait(5);
        }
        if (index == 0 || index == 59)
            qInfo() << "frame" << index << "accepted at" << clock.elapsed() << "ms";
        // 每两帧送一段音频。
        while (sentAudio < 30 && sentAudio * 2 <= index) {
            QByteArray data(framesPerBuffer * 2 * int(sizeof(float)), Qt::Uninitialized);
            auto* samples = reinterpret_cast<float*>(data.data());
            for (int sample = 0; sample < framesPerBuffer; ++sample) {
                const float value = 0.2f
                    * float(std::sin(2.0 * M_PI * 440.0
                        * (qint64(sentAudio) * framesPerBuffer + sample) / 48000.0));
                samples[sample * 2] = value;
                samples[sample * 2 + 1] = value;
            }
            const qint64 startUs = qint64(sentAudio) * framesPerBuffer * 1000000 / 48000;
            QAudioBuffer buffer(data, audioFormat, startUs);
            QVERIFY(buffer.isValid());
            while (!audioInput.sendAudioBuffer(buffer)) {
                if (clock.elapsed() > 30000)
                    QFAIL(QStringLiteral("sendAudioBuffer 未被接受：第 %1 段").arg(sentAudio).toUtf8());
                QTest::qWait(5);
            }
            ++sentAudio;
        }
    }
    qInfo() << "all frames and audio sent at" << clock.elapsed() << "ms";
    recorder.stop();
    QTRY_COMPARE_WITH_TIMEOUT(recorder.recorderState(), QMediaRecorder::StoppedState, 30000);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QTRY_VERIFY(QFileInfo::exists(output) && QFileInfo(output).size() > 0);
    // 60 帧 33333 微秒 ≈ 2 秒，编码器报告的时长不应明显缩水。
    QVERIFY2(recorder.duration() >= 1500,
        qPrintable(QStringLiteral("duration=%1ms").arg(recorder.duration())));
}

QTEST_MAIN(RecordingPipelineSmokeTest)
#include "RecordingPipelineSmokeTest.moc"
