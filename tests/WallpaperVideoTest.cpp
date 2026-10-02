#include "app/WallpaperWindow.h"

#include <QApplication>
#include <QFile>
#include <QImage>
#include <QMediaPlayer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QVideoFrame>
#include <QVideoSink>
#include <QVideoWidget>

// 使用真实 H.264 文件验证视频解码、循环、资源切换和失败撤窗。
class WallpaperVideoTest final : public QObject
{
    Q_OBJECT
private slots:
    // 中文、空格和 URL 特殊字符路径也应持续解码并循环播放。
    void h264FramesLoopAndSwitch();
    // 损坏文件必须报告错误并撤回窗口，不能只留下黑屏。
    void invalidVideoWithdrawsWindow();
};

void WallpaperVideoTest::h264FramesLoopAndSwitch()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString video = directory.filePath(QStringLiteral("海风 #测试.mp4"));
    QVERIFY(QFile::copy(QStringLiteral(TEST_VIDEO_FILE), video));
    WallpaperWindow window;
    window.resize(320, 180);
    QSignalSpy errors(&window, &WallpaperWindow::mediaFailed);
    window.setSource(video, QStringLiteral("video"));
    QMediaPlayer* player = window.findChild<QMediaPlayer*>();
    QVERIFY(player);
    QVERIFY(!player->audioOutput());
    QCOMPARE(player->loops(), QMediaPlayer::Infinite);
    QCOMPARE(window.videoView()->aspectRatioMode(), Qt::KeepAspectRatioByExpanding);
    int frames = 0;
    bool looped = false;
    qint64 previousTime = -1;
    connect(window.videoView()->videoSink(), &QVideoSink::videoFrameChanged, &window,
        [&](const QVideoFrame& frame) {
            if (!frame.isValid())
                return;
            ++frames;
            looped |= previousTime >= 0 && frame.startTime() < previousTime;
            previousTime = frame.startTime();
        });
    QTRY_VERIFY_WITH_TIMEOUT(window.isMediaReady(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(frames > 10 && looped, 10000);
    QCOMPARE(errors.count(), 0);
    const QImage frame = window.videoView()->videoSink()->videoFrame().toImage();
    QCOMPARE(frame.size(), QSize(160, 90));
    // 彩色测试图案必须含明亮像素，避免只检查播放时钟却接受全黑画面。
    int bright = 0;
    for (int y = 0; y < frame.height(); y += 5)
        for (int x = 0; x < frame.width(); x += 5)
            bright += qGray(frame.pixel(x, y)) > 40;
    QVERIFY(bright > 100);
    const QString picture = directory.filePath(QStringLiteral("静图.png"));
    QImage still(160, 90, QImage::Format_RGB32);
    still.fill(Qt::cyan);
    QVERIFY(still.save(picture));
    window.setSource(picture, QStringLiteral("image"));
    QVERIFY(!window.findChild<QMediaPlayer*>());
    QVERIFY(!window.videoView());
    window.setSource(video, QStringLiteral("video"));
    QTRY_VERIFY_WITH_TIMEOUT(window.isMediaReady(), 15000);
    QCOMPARE(errors.count(), 0);
}

void WallpaperVideoTest::invalidVideoWithdrawsWindow()
{
    QTemporaryDir directory;
    QFile file(directory.filePath(QStringLiteral("损坏.mp4")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("this is not a video");
    file.close();
    WallpaperWindow window;
    window.resize(160, 90);
    window.show();
    QSignalSpy errors(&window, &WallpaperWindow::mediaFailed);
    window.setSource(file.fileName(), QStringLiteral("video"));
    QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, 15000);
    QVERIFY(errors.first().at(1).toString().contains(QStringLiteral("已恢复系统桌面")));
    QVERIFY(!window.isVisible());
    QVERIFY(!window.isMediaReady());
    QVERIFY(!window.isAlive());
    // 同一个窗口仍可在用户更换资源后正常播放。
    window.setSource(QStringLiteral(TEST_VIDEO_FILE), QStringLiteral("video"));
    QTRY_VERIFY_WITH_TIMEOUT(window.isMediaReady(), 15000);
    // 不等待旧错误和首帧，连续切换后只允许最终有效资源更新窗口状态。
    for (int attempt = 0; attempt < 3; ++attempt) {
        window.setSource(file.fileName(), QStringLiteral("video"));
        window.setSource(QStringLiteral(TEST_VIDEO_FILE), QStringLiteral("video"));
    }
    QTRY_VERIFY_WITH_TIMEOUT(window.isMediaReady(), 15000);
    QTest::qWait(300);
    QCOMPARE(errors.count(), 1);
}

// 在 QApplication 之前注册图片协议，保证图片与视频来回切换可验证。
int main(int argc, char** argv)
{
    WallpaperWindow::registerMediaScheme();
    QApplication app(argc, argv);
    WallpaperVideoTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "WallpaperVideoTest.moc"
