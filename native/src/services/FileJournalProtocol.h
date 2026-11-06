#pragma once

#include <windows.h>
#include <winioctl.h>

// 本地只读日志代理的协议；不接受路径、写盘指令或任意控制码。
namespace FileJournalProtocol {
// 服务名称、管道名称与协议签名。
inline constexpr wchar_t ServiceName[] = L"DesktopToolFileIndex";
inline constexpr wchar_t PipeName[] = L"\\\\.\\pipe\\DesktopTool.FileJournal.v1";
inline constexpr DWORD Magic = 0x4454464A;
inline constexpr DWORD Capacity = 65536;
// 固定头后最多携带一个原生日志查询结构。
struct Request {
    DWORD magic = Magic; // 协议签名。
    DWORD operation = 0; // 仅允许查询、枚举和读取日志。
    DWORD drive = 0; // 盘符编号 1 至 26。
    DWORD bytes = 0; // 输入结构字节数。
};
// 固定响应头后跟日志原始数据。
struct Response {
    DWORD error = ERROR_SUCCESS; // 原生错误码。
    DWORD bytes = 0; // 输出结构字节数。
};
// 使用重叠 I/O 完整收发，超时或服务停止时取消挂起请求。
inline bool transfer(HANDLE pipe, void* buffer, DWORD bytes, bool writing, HANDLE stop = nullptr)
{
    auto* cursor = static_cast<unsigned char*>(buffer);
    while (bytes) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return false;
        DWORD done = 0;
        const BOOL ready = writing ? WriteFile(pipe, cursor, bytes, &done, &overlapped)
                                   : ReadFile(pipe, cursor, bytes, &done, &overlapped);
        DWORD error = ready ? ERROR_SUCCESS : GetLastError();
        if (!ready && error == ERROR_IO_PENDING) {
            HANDLE events[] = {overlapped.hEvent, stop};
            const DWORD wait = WaitForMultipleObjects(stop ? 2 : 1, events, FALSE, 10000);
            if (wait != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &overlapped);
                GetOverlappedResult(pipe, &overlapped, &done, TRUE);
                error = ERROR_TIMEOUT;
            } else if (!GetOverlappedResult(pipe, &overlapped, &done, FALSE)) {
                error = GetLastError();
            } else error = ERROR_SUCCESS;
        }
        CloseHandle(overlapped.hEvent);
        if (error != ERROR_SUCCESS || !done) {
            SetLastError(error == ERROR_SUCCESS ? ERROR_BROKEN_PIPE : error);
            return false;
        }
        cursor += done;
        bytes -= done;
    }
    return true;
}
// 验证请求边界，防止客户端借代理执行其他卷操作。
inline bool valid(const Request& request)
{
    if (request.magic != Magic || request.drive < 1 || request.drive > 26) return false;
    return (request.operation == FSCTL_QUERY_USN_JOURNAL && request.bytes == 0)
        || (request.operation == FSCTL_ENUM_USN_DATA && request.bytes == sizeof(MFT_ENUM_DATA_V0))
        || (request.operation == FSCTL_READ_USN_JOURNAL && request.bytes == sizeof(READ_USN_JOURNAL_DATA_V0));
}
}
