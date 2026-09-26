#include "GlobalHotkey.h"

#include <QCoreApplication>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

GlobalHotkey::GlobalHotkey(bool enabled, QObject* parent)
    : QObject(parent), m_enabled(enabled)
{
    QCoreApplication::instance()->installNativeEventFilter(this);
}

GlobalHotkey::~GlobalHotkey()
{
    cancelShortcut();
#ifdef Q_OS_WIN
    if (m_enabled && m_id)
        UnregisterHotKey(nullptr, m_id);
#endif
    QCoreApplication::instance()->removeNativeEventFilter(this);
}

bool GlobalHotkey::setShortcut(int modifier, int key, QString* error)
{
    if (!prepareShortcut(modifier, key, error))
        return false;
    commitShortcut();
    return true;
}

bool GlobalHotkey::prepareShortcut(int modifier, int key, QString* error)
{
    cancelShortcut();
    if (m_id && modifier == m_modifier && key == m_key)
        return true;
    const int nextId = m_id == 0x4D01 ? 0x4D02 : 0x4D01;
#ifdef Q_OS_WIN
    if (m_enabled && !RegisterHotKey(nullptr, nextId, modifier | MOD_NOREPEAT, key)) {
        if (error)
            *error = QStringLiteral("该全局快捷键已被占用或系统不支持，请更换组合。");
        return false;
    }
#else
    if (m_enabled) {
        if (error)
            *error = QStringLiteral("当前平台暂不支持全局快捷键。");
        return false;
    }
#endif
    m_pendingId = nextId;
    m_pendingModifier = modifier;
    m_pendingKey = key;
    return true;
}

void GlobalHotkey::commitShortcut()
{
    if (!m_pendingId)
        return;
#ifdef Q_OS_WIN
    if (m_enabled && m_id)
        UnregisterHotKey(nullptr, m_id);
#endif
    m_id = m_pendingId;
    m_modifier = m_pendingModifier;
    m_key = m_pendingKey;
    m_pendingId = 0;
}

void GlobalHotkey::cancelShortcut()
{
#ifdef Q_OS_WIN
    if (m_enabled && m_pendingId)
        UnregisterHotKey(nullptr, m_pendingId);
#endif
    m_pendingId = 0;
}

bool GlobalHotkey::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result)
{
    Q_UNUSED(result)
#ifdef Q_OS_WIN
    if (eventType == "windows_generic_MSG" || eventType == "windows_dispatcher_MSG") {
        const auto* msg = static_cast<const MSG*>(message);
        if (msg->message == WM_HOTKEY && msg->wParam == static_cast<WPARAM>(m_id)) {
            emit activated();
            return true;
        }
    }
#else
    Q_UNUSED(eventType)
    Q_UNUSED(message)
#endif
    return false;
}
