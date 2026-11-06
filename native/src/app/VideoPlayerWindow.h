#pragma once

#include <QWidget>

class QAudioOutput;
class QComboBox;
class QLabel;
class QMediaPlayer;
class QPushButton;
class QSlider;
class QTimer;
class QVideoWidget;

// 资源浏览器的视频窗口：QMediaPlayer 播放，控制条提供播放/暂停、
// 进度拖动、倍速、音量与全屏，并可在同目录视频间切换。
// 窗口关闭时自毁（WA_DeleteOnClose），调用方创建后只需 show。
class VideoPlayerWindow final : public QWidget
{
    Q_OBJECT
public:
    // 播放一组视频文件；index 为初始项，autoplay 决定加载后是否立即播放。
    void setFiles(const QStringList& paths, int index, bool autoplay = true);

    // 便捷打开：创建窗口、加载并显示；路径列表不能为空。
    static VideoPlayerWindow* open(const QStringList& paths, int index, QWidget* parent);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    explicit VideoPlayerWindow(QWidget* parent);

    void showIndex(int index, bool autoplay);
    void togglePlayback();
    void toggleFullscreen();
    // 全屏时按播放状态自动隐藏/显示控制条，并把指针变为隐藏光标。
    void updateFullscreenChrome();
    void stepPosition(qint64 milliseconds);
    void stepVolume(int delta);
    QString positionText() const;

    QMediaPlayer* m_player;
    QAudioOutput* m_audio;
    QVideoWidget* m_video;
    QWidget* m_controls;
    QPushButton* m_playButton;
    QPushButton* m_previous;
    QPushButton* m_next;
    QPushButton* m_muteButton;
    QPushButton* m_fullscreenButton;
    QSlider* m_seek;
    QSlider* m_volume;
    QComboBox* m_speed;
    QLabel* m_timeLabel;
    QStringList m_paths;
    int m_index = 0;
    qint64 m_duration = 0;
    // 拖动进度条期间不回写界面，避免与播放进度互相覆盖。
    bool m_scrubbing = false;
    // 全屏下延迟隐藏控制条。
    QTimer* m_hideTimer;
    // 单击暂停与双击全屏的区分：双击递增代际使未触发的单击失效。
    int m_clickGeneration = 0;
};
