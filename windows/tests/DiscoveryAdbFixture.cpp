// Diagnostic-only ADB gate: simulate a phone arriving after application launch,
// then delegate to the real ADB. Never packaged with StratoCam.
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int wmain(int argc, wchar_t** argv) {
    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    const auto directory = std::filesystem::path(module).parent_path();
    if (argc == 2 && std::wstring(argv[1]) == L"devices" &&
        !std::filesystem::exists(directory / "phone-arrived")) {
        std::cout << "List of devices attached\n\n";
        return 0;
    }
    std::wifstream pathFile(directory / "real-adb.txt");
    std::wstring realAdb;
    std::getline(pathFile, realAdb);
    if (realAdb.empty()) return 2;
    std::wstring command = L"\"" + realAdb + L"\"";
    for (int index = 1; index < argc; ++index) command += L" \"" + std::wstring(argv[index]) + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) return 3;
    WaitForSingleObject(process.hProcess, 30'000);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
}
