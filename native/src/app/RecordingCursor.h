#pragma once

#include <QImage>
#include <QPainter>
#include <QRect>
#include <QScopeGuard>
#include <QString>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

// 在裁剪后的原始像素画面中合成系统光标，再交给现有分辨率缩放和编码流程。
namespace RecordingCursor {
// 原生桌面坐标映射到裁剪画面，支持负坐标副屏和不同抓屏像素尺寸。
inline QPoint imagePosition(const QPoint& position, const QRect& monitor,
    const QSize& frameSize, const QRect& crop)
{
    return QPoint(qRound(double(position.x() - monitor.x()) * frameSize.width() / monitor.width()),
        qRound(double(position.y() - monitor.y()) * frameSize.height() / monitor.height())) - crop.topLeft();
}

#ifdef Q_OS_WIN
// 按真实热点合成指定光标；仅为光标覆盖的小块画面创建 GDI 缓冲，保留透明与反色效果。
inline void drawCursor(QImage& image, HCURSOR cursor, const QPoint& position,
    qreal scaleX = 1.0, qreal scaleY = 1.0)
{
    ICONINFO icon{};
    if (image.isNull() || !cursor || !GetIconInfo(cursor, &icon))
        return;
    const auto releaseIcon = qScopeGuard([&]() {
        if (icon.hbmColor) DeleteObject(icon.hbmColor);
        if (icon.hbmMask) DeleteObject(icon.hbmMask);
    });
    BITMAP bitmap{};
    if (!GetObjectW(icon.hbmColor ? icon.hbmColor : icon.hbmMask, sizeof(bitmap), &bitmap))
        return;
    const QSize size(qMax(1, qRound(bitmap.bmWidth * scaleX)),
        qMax(1, qRound(bitmap.bmHeight * scaleY / (icon.hbmColor ? 1 : 2))));
    const QPoint topLeft = position - QPoint(qRound(icon.xHotspot * scaleX), qRound(icon.yHotspot * scaleY));
    const QRect patchRect = QRect(topLeft, size).intersected(image.rect());
    if (patchRect.isEmpty())
        return;
    const HDC dc = CreateCompatibleDC(nullptr);
    if (!dc)
        return;
    const auto releaseDc = qScopeGuard([&]() { DeleteDC(dc); });
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = patchRect.width();
    info.bmiHeader.biHeight = -patchRect.height();
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    const HBITMAP dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dib)
        return;
    const auto releaseDib = qScopeGuard([&]() { DeleteObject(dib); });
    const HGDIOBJ previous = SelectObject(dc, dib);
    const auto restoreBitmap = qScopeGuard([&]() { SelectObject(dc, previous); });
    QImage patch(static_cast<uchar*>(pixels), patchRect.width(), patchRect.height(), QImage::Format_RGB32);
    {
        QPainter painter(&patch);
        painter.drawImage(-patchRect.topLeft(), image);
    }
    if (!DrawIconEx(dc, topLeft.x() - patchRect.x(), topLeft.y() - patchRect.y(),
            cursor, size.width(), size.height(), 0, nullptr, DI_NORMAL))
        return;
    GdiFlush();
    // GDI 可能清零第四字节；恢复不透明 alpha，避免合成到带透明通道的抓屏帧时丢失光标。
    for (int y = 0; y < patch.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(patch.scanLine(y));
        for (int x = 0; x < patch.width(); ++x)
            row[x] |= 0xff000000U;
    }
    QPainter painter(&image);
    painter.drawImage(patchRect.topLeft(), patch);
}
#endif

// 读取可见系统光标，并以所在显示器的原生坐标定位；隐藏光标及其他屏幕的光标不绘制。
inline void drawCurrentCursor(QImage& image, const QRect& crop, const QSize& frameSize,
    const QString& screenName)
{
#ifdef Q_OS_WIN
    CURSORINFO cursor{};
    cursor.cbSize = sizeof(cursor);
    if (!GetCursorInfo(&cursor) || !(cursor.flags & CURSOR_SHOWING))
        return;
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromPoint(cursor.ptScreenPos, MONITOR_DEFAULTTONULL), reinterpret_cast<MONITORINFO*>(&monitor))
        || QString::fromWCharArray(monitor.szDevice).compare(screenName, Qt::CaseInsensitive) != 0)
        return;
    const QRect desktop(monitor.rcMonitor.left, monitor.rcMonitor.top,
        monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top);
    const QPoint position = imagePosition(QPoint(cursor.ptScreenPos.x, cursor.ptScreenPos.y), desktop, frameSize, crop);
    drawCursor(image, cursor.hCursor, position, double(frameSize.width()) / desktop.width(),
        double(frameSize.height()) / desktop.height());
#else
    Q_UNUSED(image);
    Q_UNUSED(crop);
    Q_UNUSED(frameSize);
    Q_UNUSED(screenName);
#endif
}
} // namespace RecordingCursor
