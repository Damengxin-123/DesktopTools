#pragma once

#include <QByteArray>
#include <QList>
#include <QtGlobal>

// 统一配置窗口与网页使用的图形设备，须在创建 Qt 应用前调用。
namespace RenderingPolicy {
// Windows 默认使用 Microsoft WARP，避开网页合成在独立显卡驱动中长期阻塞主线程。
inline void configure()
{
#ifdef Q_OS_WIN
    // 同时影响 Qt Quick 与 WebEngine；Qt 会把软件设备的 LUID 传给 Chromium。
    // 显式设置为 0 可恢复硬件设备，不覆盖调用方已经选择的渲染策略。
    if (!qEnvironmentVariableIsSet("QSG_RHI_PREFER_SOFTWARE_RENDERER"))
        qputenv("QSG_RHI_PREFER_SOFTWARE_RENDERER", QByteArrayLiteral("1"));
    // 用户显式指定其他图形后端或适配器时，保留 Chromium 的原有设备保护。
    const QByteArray backend = qgetenv("QSG_RHI_BACKEND").trimmed().toLower();
    if (qEnvironmentVariableIntValue("QSG_RHI_PREFER_SOFTWARE_RENDERER") != 0
        && (backend.isEmpty() || backend == "d3d11")
        && !qEnvironmentVariableIsSet("QT_D3D_ADAPTER_INDEX")) {
        // WARP 的 WebGL2 被 Chromium 默认名单拦截；仅在软件设备模式下放行，保留地形编辑。
        QByteArray flags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
        if (!flags.split(' ').contains(QByteArrayLiteral("--ignore-gpu-blocklist"))) {
            if (!flags.isEmpty())
                flags.append(' ');
            flags.append("--ignore-gpu-blocklist");
            qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags);
        }
    }
#endif
}
}
