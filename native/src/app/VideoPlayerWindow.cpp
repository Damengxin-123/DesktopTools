#include "InterfaceLanguage.h"
#include "VideoPlayerWindow.h"

#include <QAudioOutput>
#include <QComboBox>
#include <QEvent>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoWidget>

namespace
{
// 进度显示为 h:mm:ss 或 m:ss。
QString formatTime(qint64 milliseconds)
{
    const qint64 total = qMax(0LL, milliseconds / 1000);
    const qint64 hours = total / 3600;
    const qint64 minutes = (total % 3600) / 60;
    const qint64 seconds = total % 60;
    if (hours > 0)
        return QStringLiteral("%1:%2:%3").arg(hours).arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
}

// 全屏下无操作自动隐藏控制条的间隔。
constexpr int kControlsHideMilliseconds = 2500;
// 单击与双击的区分窗口。
constexpr int kClickMilliseconds = 260;
}

VideoPlayerWindow::VideoPlayerWindow(QWidget* parent)
    : QWidget(parent)
{
    // 不依赖父窗口：主窗口最小化时保持独立显示；打开后始终置顶。
    setWindowFlag(Qt::Window);
    setWindowFlag(Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_DeleteOnClose);
    InterfaceLanguage::bind(this, "windowTitle", QStringLiteral("视频播放器"));
    resize(1000, 640);
    setMinimumSize(640, 440);
    setStyleSheet(QStringLiteral(
        "#videoControls{background:#26282e;border-top:1px solid #14151a;}"
        "#videoControls QLabel{color:#c8ccd4;font-size:12px;background:transparent;}"
        "#videoControls QPushButton{background:transparent;border:1px solid #3a3d46;border-radius:6px;"
        "color:#d5d8df;padding:6px 11px;font-size:12px;}"
        "#videoControls QPushButton:hover{background:#33363e;border-color:#4a4e59;}"
        "#videoControls QSlider::groove:horizontal{height:4px;background:#454954;border-radius:2px;}"
        "#videoControls QSlider::handle:horizontal{width:13px;height:13px;margin:-5px 0;border-radius:7px;background:#e8eaf0;}"
        "#videoControls QSlider::sub-page:horizontal{background:#5b7fe8;border-radius:2px;}"
        "#videoControls QComboBox{background:transparent;border:1px solid #3a3d46;border-radius:6px;"
        "color:#d5d8df;padding:4px 8px;font-size:12px;}"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_video = new QVideoWidget(this);
    m_video->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(m_video, 1);

    m_player = new QMediaPlayer(this);
    m_audio = new QAudioOutput(this);
    m_player->setAudioOutput(m_audio);
    m_player->setVideoOutput(m_video);

    m_controls = new QFrame(this);
    m_controls->setObjectName(QStringLiteral("videoControls"));
    auto* bar = new QHBoxLayout(m_controls);
    bar->setContentsMargins(10, 8, 10, 8);
    bar->setSpacing(9);
    m_playButton = new QPushButton(InterfaceLanguage::text(QStringLiteral("▶ 播放")), m_controls);
    InterfaceLanguage::bind(m_playButton, "text", QStringLiteral("▶ 播放"));
    m_previous = new QPushButton(InterfaceLanguage::text(QStringLiteral("◀ 上一部")), m_controls);
    InterfaceLanguage::bind(m_previous, "text", QStringLiteral("◀ 上一部"));
    m_next = new QPushButton(InterfaceLanguage::text(QStringLiteral("下一部 ▶")), m_controls);
    InterfaceLanguage::bind(m_next, "text", QStringLiteral("下一部 ▶"));
    m_timeLabel = new QLabel(QStringLiteral("00:00 / 00:00"), m_controls);
    m_seek = new QSlider(Qt::Horizontal, m_controls);
    m_seek->setRange(0, 0);
    m_seek->setEnabled(false);
    auto* speedLabel = new QLabel(InterfaceLanguage::text(QStringLiteral("倍速")), m_controls);
    InterfaceLanguage::bind(speedLabel, "text", QStringLiteral("倍速"));
    m_speed = new QComboBox(m_controls);
    const QList<qreal> rates = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0};
    for (const qreal rate : rates)
        m_speed->addItem(QStringLiteral("%1×").arg(rate), rate);
    m_speed->setCurrentIndex(3);
    m_muteButton = new QPushButton(QStringLiteral("🔊"), m_controls);
    InterfaceLanguage::bind(m_muteButton, "toolTip", QStringLiteral("静音（M）"));
    m_volume = new QSlider(Qt::Horizontal, m_controls);
    m_volume->setRange(0, 100);
    m_volume->setValue(100);
    m_volume->setFixedWidth(110);
    InterfaceLanguage::bind(m_volume, "toolTip", QStringLiteral("音量（↑/↓）"));
    m_fullscreenButton = new QPushButton(InterfaceLanguage::text(QStringLiteral("⛶ 全屏")), m_controls);
    InterfaceLanguage::bind(m_fullscreenButton, "text", QStringLiteral("⛶ 全屏"));
    bar->addWidget(m_playButton);
    bar->addWidget(m_previous);
    bar->addWidget(m_next);
    bar->addWidget(m_timeLabel);
    bar->addWidget(m_seek, 1);
    bar->addWidget(speedLabel);
    bar->addWidget(m_speed);
    bar->addWidget(m_muteButton);
    bar->addWidget(m_volume);
    bar->addWidget(m_fullscreenButton);
    layout->addWidget(m_controls);

    m_hideTimer = new QTimer(this);
    m_hideTimer->setSingleShot(true);
    m_video->installEventFilter(this);

    connect(m_playButton, &QPushButton::clicked, this, &VideoPlayerWindow::togglePlayback);
    connect(m_previous, &QPushButton::clicked, this, [this]() { showIndex(m_index - 1, true); });
    connect(m_next, &QPushButton::clicked, this, [this]() { showIndex(m_index + 1, true); });
    connect(m_fullscreenButton, &QPushButton::clicked, this, &VideoPlayerWindow::toggleFullscreen);

    connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
        InterfaceLanguage::bind(m_playButton, "text", state == QMediaPlayer::PlayingState ? QStringLiteral("⏸ 暂停")
                                                                  : QStringLiteral("▶ 播放"));
        updateFullscreenChrome();
    });
    connect(m_player, &QMediaPlayer::durationChanged, this, [this](qint64 duration) {
        m_duration = duration;
        m_seek->setRange(0, static_cast<int>(qBound(qint64(0), duration, qint64(0x7fffffff))));
        m_seek->setEnabled(duration > 0);
        updateFullscreenChrome();
    });
    connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
        if (!m_scrubbing)
            m_seek->setValue(static_cast<int>(position));
        m_timeLabel->setText(positionText());
    });
    connect(m_seek, &QSlider::sliderPressed, this, [this]() { m_scrubbing = true; });
    connect(m_seek, &QSlider::sliderMoved, this, [this](int value) {
        m_player->setPosition(value);
        m_timeLabel->setText(positionText());
    });
    connect(m_seek, &QSlider::sliderReleased, this, [this]() {
        m_scrubbing = false;
        m_player->setPosition(m_seek->value());
    });
    connect(m_speed, &QComboBox::activated, this, [this](int index) {
        m_player->setPlaybackRate(m_speed->itemData(index).toDouble());
    });
    connect(m_muteButton, &QPushButton::clicked, this, [this]() {
        m_audio->setMuted(!m_audio->isMuted());
        m_muteButton->setText(m_audio->isMuted() ? QStringLiteral("🔇") : QStringLiteral("🔊"));
    });
    connect(m_volume, &QSlider::valueChanged, this, [this](int value) {
        m_audio->setVolume(value / 100.0);
        if (value > 0 && m_audio->isMuted()) {
            m_audio->setMuted(false);
            m_muteButton->setText(QStringLiteral("🔊"));
        }
    });
    connect(m_player, &QMediaPlayer::errorOccurred, this,
        [this](QMediaPlayer::Error error, const QString& errorString) {
            Q_UNUSED(error)
            QMessageBox::warning(this, InterfaceLanguage::text(QStringLiteral("视频播放器")),
                InterfaceLanguage::text(QStringLiteral("无法播放该视频：%1")).arg(errorString));
        });
    connect(m_hideTimer, &QTimer::timeout, this, [this]() {
        if (isFullScreen() && m_player->playbackState() == QMediaPlayer::PlayingState) {
            m_controls->hide();
            m_video->setCursor(Qt::BlankCursor);
            setCursor(Qt::BlankCursor);
        }
    });
    updateFullscreenChrome();
}

void VideoPlayerWindow::setFiles(const QStringList& paths, int index, bool autoplay)
{
    m_paths = paths;
    showIndex(index, autoplay);
}

VideoPlayerWindow* VideoPlayerWindow::open(const QStringList& paths, int index, QWidget* parent)
{
    if (paths.isEmpty())
        return nullptr;
    // 独立顶级窗口：与主窗口解耦，parent 仅保留给调用方的错误提示。
    Q_UNUSED(parent)
    auto* window = new VideoPlayerWindow(nullptr);
    window->show();
    window->setFiles(paths, index, true);
    window->raise();
    window->activateWindow();
    return window;
}

void VideoPlayerWindow::showIndex(int index, bool autoplay)
{
    if (m_paths.isEmpty())
        return;
    m_index = qBound(0, index, m_paths.size() - 1);
    const QString path = m_paths.at(m_index);
    const QString name = QFileInfo(path).fileName();
    InterfaceLanguage::bind(this, "windowTitle", QStringLiteral("视频播放器 — %1%2").arg(name,
        m_paths.size() > 1 ? QStringLiteral("（%1/%2）").arg(m_index + 1).arg(m_paths.size()) : QString()));
    m_duration = 0;
    m_scrubbing = false;
    m_seek->setRange(0, 0);
    m_seek->setEnabled(false);
    m_timeLabel->setText(QStringLiteral("00:00 / 00:00"));
    m_player->setSource(QUrl::fromLocalFile(path));
    m_previous->setEnabled(m_index > 0);
    m_next->setEnabled(m_index < m_paths.size() - 1);
    if (autoplay)
        m_player->play();
}

void VideoPlayerWindow::togglePlayback()
{
    if (m_player->playbackState() == QMediaPlayer::PlayingState)
        m_player->pause();
    else
        m_player->play();
}

void VideoPlayerWindow::toggleFullscreen()
{
    if (isFullScreen())
        showNormal();
    else
        showFullScreen();
    updateFullscreenChrome();
}

void VideoPlayerWindow::updateFullscreenChrome()
{
    if (!isFullScreen()) {
        m_controls->show();
        m_video->unsetCursor();
        unsetCursor();
        return;
    }
    if (m_player->playbackState() == QMediaPlayer::PlayingState)
        m_hideTimer->start(kControlsHideMilliseconds);
    else {
        m_hideTimer->stop();
        m_controls->show();
        m_video->unsetCursor();
        unsetCursor();
    }
}

void VideoPlayerWindow::stepPosition(qint64 milliseconds)
{
    const qint64 position = qBound(0LL, m_player->position() + milliseconds,
        m_duration > 0 ? m_duration : m_player->position());
    m_player->setPosition(position);
    m_timeLabel->setText(positionText());
}

void VideoPlayerWindow::stepVolume(int delta)
{
    m_volume->setValue(qBound(0, m_volume->value() + delta, 100));
}

QString VideoPlayerWindow::positionText() const
{
    return QStringLiteral("%1 / %2").arg(formatTime(m_player->position()), formatTime(m_duration));
}

void VideoPlayerWindow::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        if (isFullScreen())
            showNormal();
        else
            close();
        return;
    case Qt::Key_Space:
        togglePlayback();
        return;
    case Qt::Key_Left:
        stepPosition(-5000);
        return;
    case Qt::Key_Right:
        stepPosition(5000);
        return;
    case Qt::Key_Up:
        stepVolume(5);
        return;
    case Qt::Key_Down:
        stepVolume(-5);
        return;
    case Qt::Key_F:
        toggleFullscreen();
        return;
    case Qt::Key_M:
        m_muteButton->click();
        return;
    default:
        QWidget::keyPressEvent(event);
    }
}

void VideoPlayerWindow::mouseMoveEvent(QMouseEvent* event)
{
    QWidget::mouseMoveEvent(event);
    // 移动鼠标时唤回控制条（仅全屏下控制条会被隐藏）。
    if (isFullScreen() && !m_controls->isVisible()) {
        m_controls->show();
        m_video->unsetCursor();
        unsetCursor();
    }
    if (isFullScreen() && m_player->playbackState() == QMediaPlayer::PlayingState)
        m_hideTimer->start(kControlsHideMilliseconds);
}

bool VideoPlayerWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_video && event->type() == QEvent::MouseButtonDblClick) {
        // 双击失效未触发的单击，再切换全屏。
        ++m_clickGeneration;
        toggleFullscreen();
        return true;
    }
    if (watched == m_video && event->type() == QEvent::MouseButtonPress) {
        const int generation = ++m_clickGeneration;
        QTimer::singleShot(kClickMilliseconds, this, [this, generation]() {
            if (generation == m_clickGeneration)
                togglePlayback();
        });
        return true;
    }
    return QWidget::eventFilter(watched, event);
}
