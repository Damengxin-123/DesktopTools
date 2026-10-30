#pragma once
#include "FileJournalProtocol.h"

// NTFS 读取会话；管理员直接访问卷，普通用户通过受限本地服务代理。
struct FileJournalVolume {
    HANDLE native = INVALID_HANDLE_VALUE; // 直接读取句柄，普通用户为空。
    DWORD drive = 0; // 盘符编号。
};
// 检查已安装的服务是否正在运行。
bool fileJournalServiceRunning();
// 打开卷会话；失败返回空并保留 Windows 错误码。
FileJournalVolume* openFileJournalVolume(DWORD drive);
// 读取受限日志控制码，保持与 DeviceIoControl 相同的错误语义。
bool readFileJournal(FileJournalVolume* volume, DWORD operation, void* input, DWORD inputBytes,
    void* output, DWORD capacity, DWORD* returned);
// 释放直接卷句柄与会话。
void closeFileJournalVolume(FileJournalVolume* volume);
