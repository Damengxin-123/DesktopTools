#pragma once
#include <QMap>
#include <QString>
#include <QWidget>
#include <QPair>


// 窗口拖动放大缩小位置枚举
enum ResizeRegion {
    None = 0,
    Left = 1,
    Top = 2,
    Right = 4,
    Bottom = 8,
    TopLeft = Top | Left,
    TopRight = Top | Right,
    BottomLeft = Bottom | Left,
    BottomRight = Bottom | Right
};
// 左侧菜单栏目录
enum LxToolButType{
    None_Type = 0,
    Login_Type = 1,             // 登录功能
    Note_Type = 2,              // 便签功能
    Shortcuts_Type = 3,         // 快捷方式功能
    Download_Type = 4,          // 下载工具功能
    FileChange_Type = 5,        // 文件编辑
    SysSetting_Type = 6,        // 系统设置
    SysLog_Type = 7,            // 系统日志
};
// 任务栏目录对应名称
const QMap<LxToolButType, QPair<QString, QString>> LxToolButTypeName = {
    //{LxToolButType::None_Type, {"无",    "null"}},
    {LxToolButType::Login_Type,         {("登录"),            ":/images/login.png"}},
    {LxToolButType::Note_Type,          {("便签"),            ":/images/note.png"}},
    {LxToolButType::Shortcuts_Type,     {("快捷方式"),        ":/images/shortcut.png"}},
    {LxToolButType::Download_Type,      {("下载工具"),        ":/images/download.png"}},
    {LxToolButType::FileChange_Type,    {("文件转换工具"),    ":/images/file_change.png"}},
    {LxToolButType::SysSetting_Type,    {("系统设置"),        ":/images/setting.png"}},
    {LxToolButType::SysLog_Type,        {("系统日志"),        ":/images/setting.png"}},
}; 
// 初始化主界面嵌套界面
QMap<LxToolButType, QWidget*>& initMainWindowData(QWidget* parent);
