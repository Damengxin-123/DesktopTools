// 复用服务请求处理实现，在普通权限下验证管道协议；不注册系统服务。
#define wmain fileIndexServiceEntryForTest
#include "services/FileIndexServiceMain.cpp"
#undef wmain
#include <thread>
#include <cstdio>

// 启动一次临时管道连接，验证连续响应、拒绝非法操作与停止时解除阻塞。
int main()
{
    using namespace FileJournalProtocol;
    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent) return 1;
    const std::wstring name = std::wstring(L"\\\\.\\pipe\\DesktopTool.JournalTest.") + std::to_wstring(GetCurrentProcessId());
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, Capacity, 256, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return 2;
    OVERLAPPED connection{};
    connection.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ConnectNamedPipe(pipe, &connection);
    HANDLE client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (client == INVALID_HANDLE_VALUE) return 3;
    DWORD ignored = 0;
    if (!GetOverlappedResult(pipe, &connection, &ignored, TRUE)) return 4;
    std::thread worker([pipe]() { serve(pipe); DisconnectNamedPipe(pipe); });
    int failure = 0;
    for (int i = 0; i < 3; ++i) {
        Request request{Magic, FSCTL_QUERY_USN_JOURNAL, 3, 0};
        Response response;
        std::vector<unsigned char> buffer(Capacity);
        if (!transfer(client, &request, sizeof(request), true)
            || !transfer(client, &response, sizeof(response), false)
            || response.bytes > Capacity
            || (response.bytes && !transfer(client, buffer.data(), response.bytes, false))) {
            failure = 5; break;
        }
        if (response.error == ERROR_SUCCESS && response.bytes < sizeof(USN_JOURNAL_DATA_V0)) {
            failure = 6; break;
        }
        DWORD ack = Magic;
        if (!transfer(client, &ack, sizeof(ack), true)) { failure = 7; break; }
    }
    // 请求关闭会中断服务正在等待下一请求的读取，不需要等十秒超时。
    const ULONGLONG start = GetTickCount64();
    SetEvent(stopEvent);
    worker.join();
    if (GetTickCount64() - start > 2000) failure = 8;
    CloseHandle(client); CloseHandle(pipe); CloseHandle(connection.hEvent); CloseHandle(stopEvent);
    std::printf("FileJournalTransportTest: %s (%d)\n", failure ? "FAIL" : "PASS", failure);
    return failure;
}
