#include "FileJournalClient.h"
#include <string>

namespace {
// 每个索引线程复用一个代理连接，线程退出时自动断开。
struct JournalConnection {
    HANDLE pipe = INVALID_HANDLE_VALUE; // 当前服务管道。
    // 自动释放线程关联的管道。
    ~JournalConnection() { if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe); }
};
thread_local JournalConnection connection;

// 获取 SCM 中正在运行的服务进程，拒绝伪造同名管道的普通进程。
DWORD serviceProcess()
{
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return 0;
    SC_HANDLE service = OpenServiceW(manager, FileJournalProtocol::ServiceName, SERVICE_QUERY_STATUS);
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    const bool ready = service && QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
        reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes) && status.dwCurrentState == SERVICE_RUNNING;
    if (service) CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return ready ? status.dwProcessId : 0;
}
}

bool fileJournalServiceRunning() { return serviceProcess() != 0; }

FileJournalVolume* openFileJournalVolume(DWORD drive)
{
    if (drive < 1 || drive > 26) { SetLastError(ERROR_INVALID_PARAMETER); return nullptr; }
    const std::wstring path = std::wstring(L"\\\\.\\") + wchar_t(L'A' + drive - 1) + L":";
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE && !fileJournalServiceRunning()) {
        SetLastError(ERROR_ACCESS_DENIED); return nullptr;
    }
    return new FileJournalVolume{handle, drive};
}

bool readFileJournal(FileJournalVolume* volume, DWORD operation, void* input, DWORD inputBytes,
    void* output, DWORD capacity, DWORD* returned)
{
    using namespace FileJournalProtocol;
    *returned = 0;
    if (volume->native != INVALID_HANDLE_VALUE)
        return DeviceIoControl(volume->native, operation, input, inputBytes, output, capacity, returned, nullptr) != FALSE;
    Request request{Magic, operation, volume->drive, inputBytes};
    if (!valid(request)) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    const DWORD expectedProcess = serviceProcess();
    if (!expectedProcess) { SetLastError(ERROR_SERVICE_NOT_ACTIVE); return false; }
    HANDLE& pipe = connection.pipe;
    // 连接已失效时释放，下一请求重新连接系统服务。
    DWORD available = 0;
    if (pipe != INVALID_HANDLE_VALUE && !PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
        CloseHandle(pipe); pipe = INVALID_HANDLE_VALUE;
    }
    for (int attempt = 0; pipe == INVALID_HANDLE_VALUE && attempt < 20; ++attempt) {
        pipe = CreateFileW(PipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        const DWORD error = GetLastError();
        if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) return false;
        WaitNamedPipeW(PipeName, 100);
        Sleep(5);
    }
    if (pipe == INVALID_HANDLE_VALUE) return false;
    ULONG serverProcess = 0;
    Response response;
    bool ok = GetNamedPipeServerProcessId(pipe, &serverProcess) && serverProcess == expectedProcess;
    if (!ok) SetLastError(ERROR_ACCESS_DENIED);
    if (ok) ok = transfer(pipe, &request, sizeof(request), true)
        && (!inputBytes || transfer(pipe, input, inputBytes, true))
        && transfer(pipe, &response, sizeof(response), false);
    if (ok && response.bytes > capacity) { ok = false; SetLastError(ERROR_INVALID_DATA); }
    if (ok && response.bytes) ok = transfer(pipe, output, response.bytes, false);
    const DWORD transportError = ok ? ERROR_SUCCESS : GetLastError();
    // 显式确认响应已读取，服务才断开管道，避免尾部数据被丢弃。
    DWORD ack = Magic;
    if (ok) transfer(pipe, &ack, sizeof(ack), true);
    if (!ok) { CloseHandle(pipe); pipe = INVALID_HANDLE_VALUE; SetLastError(transportError); return false; }
    *returned = response.bytes;
    SetLastError(response.error);
    return response.error == ERROR_SUCCESS;
}

void closeFileJournalVolume(FileJournalVolume* volume)
{
    if (!volume) return;
    if (volume->native != INVALID_HANDLE_VALUE) CloseHandle(volume->native);
    delete volume;
}
