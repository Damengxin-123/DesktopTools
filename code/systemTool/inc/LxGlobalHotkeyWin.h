#pragma once

#include <QObject>
#include <QAbstractNativeEventFilter>
#include <windows.h>

class LxGlobalHotkeyWin : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    LxGlobalHotkeyWin(QObject *parent);
    ~LxGlobalHotkeyWin();

    // ×¢²áÈÈ¼ü
    bool registerHotkey(int modifier, int key); 
    void unregisterHotkey();

signals:
    void hotkeyPressed();

protected:
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result);

private:
    int hotkeyId = 1001; // ÈÈ¼ü ID
};
