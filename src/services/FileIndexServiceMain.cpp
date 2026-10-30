#include "FileJournalProtocol.h"
#include <sddl.h>
#include <shlobj.h>
#include <string>
#include <vector>

namespace {
// 服务停止事件与 SCM 状态，由服务控制回调管理。
HANDLE stopEvent = nullptr;
SERVICE_STATUS_HANDLE statusHandle = nullptr;
SERVICE_STATUS status{};

// 向 Windows 服务管理器报告状态。
void report(DWORD state, DWORD error = ERROR_SUCCESS)
{
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    status.dwWin32ExitCode = error;
    status.dwWaitHint = state == SERVICE_STOP_PENDING ? 15000 : 0;
    SetServiceStatus(statusHandle, &status);
}
// 收到停止或系统关机请求时唤醒全部挂起 I/O。
void WINAPI control(DWORD code)
{
    if (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_SHUTDOWN) {
        report(SERVICE_STOP_PENDING);
        SetEvent(stopEvent);
    }
}
// 处理一个受限请求；服务不保存用户索引，不打开用户指定的路径。
void serve(HANDLE pipe)
{
    using namespace FileJournalProtocol;
    // 同一连接可读取多个批次，避免首次建立百万条索引时反复连接服务。
    for (;;) {
        Request request;
        if (!transfer(pipe, &request, sizeof(request), false, stopEvent) || !valid(request)) return;
        alignas(8) unsigned char input[sizeof(READ_USN_JOURNAL_DATA_V0)]{};
        if (request.bytes && !transfer(pipe, input, request.bytes, false, stopEvent)) return;
        if (request.operation == FSCTL_READ_USN_JOURNAL) {
            auto* read = reinterpret_cast<READ_USN_JOURNAL_DATA_V0*>(input);
            read->BytesToWaitFor = 0;
            read->Timeout = 0;
        }
        const wchar_t root[] = {wchar_t(L'A' + request.drive - 1), L':', L'\\', 0};
        wchar_t fileSystem[32]{};
        Response response;
        std::vector<unsigned char> output(Capacity);
        if (GetDriveTypeW(root) != DRIVE_FIXED
            || !GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, fileSystem, 32)
            || wcscmp(fileSystem, L"NTFS") != 0) {
            response.error = ERROR_NOT_SUPPORTED;
        } else {
            const std::wstring path = std::wstring(L"\\\\.\\") + root[0] + L":";
            HANDLE volume = CreateFileW(path.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (volume == INVALID_HANDLE_VALUE) response.error = GetLastError();
            else {
                if (!DeviceIoControl(volume, request.operation, input, request.bytes,
                        output.data(), request.operation == FSCTL_QUERY_USN_JOURNAL ? DWORD(sizeof(USN_JOURNAL_DATA_V0)) : Capacity, &response.bytes, nullptr)) {
                    response.error = GetLastError();
                    response.bytes = 0;
                }
                CloseHandle(volume);
            }
        }
        if (!transfer(pipe, &response, sizeof(response), true, stopEvent)
            || (response.bytes && !transfer(pipe, output.data(), response.bytes, true, stopEvent))) return;
        DWORD ack = 0;
        if (!transfer(pipe, &ack, sizeof(ack), false, stopEvent) || ack != Magic) return;
    }
}
// 服务事件循环：空闲时阻塞等待连接，没有周期扫描或常驻索引。
void WINAPI serviceMain(DWORD, wchar_t**)
{
    statusHandle = RegisterServiceCtrlHandlerW(FileJournalProtocol::ServiceName, control);
    if (!statusHandle) return;
    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent) { report(SERVICE_STOPPED, GetLastError()); return; }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    // 拒绝网络用户，仅本机已认证用户可请求文件名元数据；SYSTEM 拥有完整权限。
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(D;;GA;;;NU)(A;;GA;;;SY)(A;;GRGW;;;AU)", SDDL_REVISION_1, &descriptor, nullptr)) {
        report(SERVICE_STOPPED, GetLastError()); CloseHandle(stopEvent); return;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    report(SERVICE_RUNNING);
    DWORD failure = ERROR_SUCCESS;
    while (WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
        HANDLE pipe = CreateNamedPipeW(FileJournalProtocol::PipeName,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, FileJournalProtocol::Capacity, 256, 0, &attributes);
        if (pipe == INVALID_HANDLE_VALUE) { failure = GetLastError(); break; }
        OVERLAPPED connection{};
        connection.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!connection.hEvent) { failure = GetLastError(); CloseHandle(pipe); break; }
        bool connected = ConnectNamedPipe(pipe, &connection) != FALSE;
        const DWORD error = connected ? ERROR_SUCCESS : GetLastError();
        if (error == ERROR_PIPE_CONNECTED) connected = true;
        else if (error == ERROR_IO_PENDING) {
            HANDLE events[] = {connection.hEvent, stopEvent};
            connected = WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0;
            DWORD ignored = 0;
            if (!connected) CancelIoEx(pipe, &connection);
            GetOverlappedResult(pipe, &connection, &ignored, TRUE);
        }
        if (connected) serve(pipe);
        // 响应传输完成后客户端发起关闭；不使用可能无限等待客户端的 FlushFileBuffers。
        DisconnectNamedPipe(pipe);
        CloseHandle(connection.hEvent);
        CloseHandle(pipe);
    }
    LocalFree(descriptor);
    report(SERVICE_STOPPED, failure);
    CloseHandle(stopEvent);
}
// 等待服务进入运行态，避免安装进程先退出导致界面误判失败。
DWORD waitForRunning(SC_HANDLE service)
{
    for (int attempt = 0; attempt < 100; ++attempt) {
        SERVICE_STATUS current{};
        if (!QueryServiceStatus(service, &current)) return GetLastError();
        if (current.dwCurrentState == SERVICE_RUNNING) return ERROR_SUCCESS;
        if (current.dwCurrentState == SERVICE_STOPPED)
            return current.dwWin32ExitCode ? current.dwWin32ExitCode : ERROR_SERVICE_NOT_ACTIVE;
        Sleep(100);
    }
    return ERROR_TIMEOUT;
}
// 提权安装器把服务复制到受保护目录，避免 SYSTEM 执行便携目录中的可替换文件。
DWORD install()
{
    wchar_t programFiles[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr, SHGFP_TYPE_CURRENT, programFiles)))
        return ERROR_PATH_NOT_FOUND;
    const std::wstring folder = std::wstring(programFiles) + L"\\DesktopToolIndex";
    if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return GetLastError();
    // 拒绝目录重解析点，安装文件夹仅管理员与 SYSTEM 可写。
    const DWORD attributes = GetFileAttributesW(folder.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return ERROR_ACCESS_DENIED;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)",
            SDDL_REVISION_1, &descriptor, nullptr)) return GetLastError();
    const BOOL secured = SetFileSecurityW(folder.c_str(), OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
    LocalFree(descriptor);
    if (!secured) return GetLastError();
    wchar_t source[32768]{};
    if (!GetModuleFileNameW(nullptr, source, 32768)) return GetLastError();
    const std::wstring target = folder + L"\\DesktopToolIndexService.exe";
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!manager) return GetLastError();
    SC_HANDLE service = OpenServiceW(manager, FileJournalProtocol::ServiceName, SERVICE_QUERY_STATUS | SERVICE_START);
    if (service) {
        // 已安装时仅启动服务，不覆盖正在使用的受保护程序。
        DWORD error = StartServiceW(service, 0, nullptr) ? ERROR_SUCCESS : GetLastError();
        if (error == ERROR_SUCCESS || error == ERROR_SERVICE_ALREADY_RUNNING) error = waitForRunning(service);
        CloseServiceHandle(service); CloseServiceHandle(manager);
        return error;
    }
    if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
        CloseServiceHandle(manager); return ERROR_FILE_EXISTS;
    }
    // 从创建瞬间使用受保护 ACL 与管理员所有者，不继承便携源文件的可写权限。
    PSECURITY_DESCRIPTOR fileDescriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;BU)", SDDL_REVISION_1, &fileDescriptor, nullptr)) {
        const DWORD error = GetLastError(); CloseServiceHandle(manager); return error;
    }
    SECURITY_ATTRIBUTES fileSecurity{sizeof(fileSecurity), fileDescriptor, FALSE};
    HANDLE output = CreateFileW(target.c_str(), GENERIC_WRITE, 0, &fileSecurity, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    LocalFree(fileDescriptor);
    if (output == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError(); CloseServiceHandle(manager); return error;
    }
    HANDLE input = CreateFileW(source, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD copyError = input == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    std::vector<unsigned char> block(65536);
    while (copyError == ERROR_SUCCESS) {
        DWORD read = 0, written = 0;
        if (!ReadFile(input, block.data(), DWORD(block.size()), &read, nullptr)) { copyError = GetLastError(); break; }
        if (!read) break;
        if (!WriteFile(output, block.data(), read, &written, nullptr) || written != read) {
            copyError = ERROR_WRITE_FAULT; break;
        }
    }
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (copyError == ERROR_SUCCESS && !FlushFileBuffers(output)) copyError = GetLastError();
    CloseHandle(output);
    if (copyError != ERROR_SUCCESS) {
        DeleteFileW(target.c_str()); CloseServiceHandle(manager); return copyError;
    }
    const std::wstring command = L"\"" + target + L"\"";
    service = CreateServiceW(manager, FileJournalProtocol::ServiceName, L"桌面工具 NTFS 索引服务",
        SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    DWORD error = service ? ERROR_SUCCESS : GetLastError();
    if (service) {
        SERVICE_DESCRIPTIONW description{const_cast<wchar_t*>(L"为桌面工具提供只读 NTFS 文件名与 USN 日志访问，空闲时不扫描磁盘。")};
        ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);
        if (!StartServiceW(service, 0, nullptr)) error = GetLastError();
        else error = waitForRunning(service);
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    if (!service) DeleteFileW(target.c_str());
    return error;
}
// 显式卸载只停止并移除本服务与其固定程序文件，不接触用户索引和业务数据。
DWORD uninstall()
{
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return GetLastError();
    SC_HANDLE service = OpenServiceW(manager, FileJournalProtocol::ServiceName,
        SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    DWORD error = service ? ERROR_SUCCESS : GetLastError();
    if (service) {
        SERVICE_STATUS current{};
        if (!ControlService(service, SERVICE_CONTROL_STOP, &current) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE)
            error = GetLastError();
        if (error == ERROR_SUCCESS) {
            for (int i = 0; i < 150; ++i) {
                if (!QueryServiceStatus(service, &current)) { error = GetLastError(); break; }
                if (current.dwCurrentState == SERVICE_STOPPED) break;
                Sleep(100);
            }
            if (error == ERROR_SUCCESS && current.dwCurrentState != SERVICE_STOPPED) error = ERROR_TIMEOUT;
            if (error == ERROR_SUCCESS && !DeleteService(service)) error = GetLastError();
        }
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    if (error != ERROR_SUCCESS && error != ERROR_SERVICE_DOES_NOT_EXIST) return error;
    wchar_t programFiles[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr, SHGFP_TYPE_CURRENT, programFiles)))
        return ERROR_PATH_NOT_FOUND;
    const std::wstring folder = std::wstring(programFiles) + L"\\DesktopToolIndex";
    const DWORD attributes = GetFileAttributesW(folder.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return ERROR_SUCCESS;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_ACCESS_DENIED;
    const std::wstring target = folder + L"\\DesktopToolIndexService.exe";
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (DeleteFileW(target.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) break;
        const DWORD deletionError = GetLastError();
        if (attempt == 49 || (deletionError != ERROR_ACCESS_DENIED && deletionError != ERROR_SHARING_VIOLATION))
            return deletionError;
        Sleep(100);
    }
    RemoveDirectoryW(folder.c_str());
    return ERROR_SUCCESS;
}
}
// 服务程序入口；只有显式 --install 才修改系统服务配置。
int wmain(int argc, wchar_t** argv)
{
    if (argc == 2 && wcscmp(argv[1], L"--install") == 0) return int(install());
    if (argc == 2 && wcscmp(argv[1], L"--uninstall") == 0) return int(uninstall());
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<wchar_t*>(FileJournalProtocol::ServiceName), serviceMain}, {nullptr, nullptr}};
    return StartServiceCtrlDispatcherW(table) ? 0 : int(GetLastError());
}
