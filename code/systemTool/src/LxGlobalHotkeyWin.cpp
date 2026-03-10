#include <qapplication.h>
#include "LxGlobalHotkeyWin.h"

LxGlobalHotkeyWin::LxGlobalHotkeyWin(QObject *parent)
    : QObject(parent)
{
    qApp->installNativeEventFilter(this);
}

LxGlobalHotkeyWin::~LxGlobalHotkeyWin()
{
    unregisterHotkey();
    qApp->removeNativeEventFilter(this);
}

bool LxGlobalHotkeyWin::registerHotkey(int modifier, int key)
{
    return RegisterHotKey(nullptr, hotkeyId, modifier, key);
}

void LxGlobalHotkeyWin::unregisterHotkey()
{
    UnregisterHotKey(nullptr, hotkeyId);
}

bool LxGlobalHotkeyWin::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result)
{
    if (eventType == "windows_generic_MSG") {
        MSG* msg = static_cast<MSG*>(message);
        if (msg->message == WM_HOTKEY && msg->wParam == hotkeyId) {
            emit hotkeyPressed();
            return true;
        }
    }
    return false;
}
