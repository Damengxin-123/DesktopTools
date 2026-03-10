#pragma once
#include "LxMainWindowData.h"
#include "widgets/inc/LxLoginWidget.h"
#include "widgets/inc/LxNoteWindow.h"
#include "widgets/inc/LxShortcutManageWidget.h"
#include "widgets/inc/LxSysLogManageWindow.h"
#include "widgets/inc/LxDownloadWidget.h"


static QMap<LxToolButType, QWidget*> LxToolButTypeWidget; 

QMap<LxToolButType, QWidget*>& initMainWindowData(QWidget* parent)
{
    // 初始化窗口类型对应的Widget
    if (LxToolButTypeWidget.isEmpty())
    {
        // 登录窗口
        LxToolButTypeWidget[LxToolButType::Login_Type] = new LxLoginWidget(parent);
        // 便签窗口
        LxToolButTypeWidget[LxToolButType::Note_Type] = new LxNoteWindow(parent);
        // 快捷方式管理窗口
        LxToolButTypeWidget[LxToolButType::Shortcuts_Type] = new LxShortcutManageWidget(parent);
        // 日志管理窗口
        LxToolButTypeWidget[LxToolButType::SysLog_Type] = new LxSysLogManageWindow(parent);

        LxToolButTypeWidget[LxToolButType::Download_Type] = new LxDownloadWidget(parent);
    }

    return LxToolButTypeWidget;
}
