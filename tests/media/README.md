# 视频测试素材

`wallpaper-h264.mp4` 是 FFmpeg `testsrc2` 生成的彩色动态图案，160×90、10 帧/秒、0.6 秒，无音轨，H.264 / yuv420p。它用于检测浏览器缺少 H.264 解码时容易漏测的真实播放和循环问题，不包含用户提供的壁纸。

生成命令：

```text
ffmpeg -f lavfi -i testsrc2=size=160x90:rate=10:duration=0.6 -an -c:v libx264 -pix_fmt yuv420p -movflags +faststart wallpaper-h264.mp4
```
