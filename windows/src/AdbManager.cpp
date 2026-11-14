#include "AdbManager.h"
#include "PipelineDiagnostics.h"

#include <array>
#include <fstream>
#include <shlobj.h>

namespace fs = std::filesystem;

AdbManager::AdbManager() {
    std::array<wchar_t, 32768> modulePath{};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    applicationDirectory_ = length
        ? fs::path(std::wstring(modulePath.data(), length)).parent_path()
        : fs::current_path();

    const auto installedAdb = applicationDirectory_ / "platform-tools" / "adb.exe";
    if (fs::exists(installedAdb)) {
        adbPath_ = installedAdb;
        runtimeRoot_ = KnownFolder(FOLDERID_LocalAppData, applicationDirectory_) / "StratoCam";
        diagnosticsDirectory_ = runtimeRoot_ / "Diagnostics";
        defaultRecordingDirectory_ = KnownFolder(FOLDERID_Videos, runtimeRoot_) / "StratoCam";
    } else if (const auto root = FindDevelopmentRoot(applicationDirectory_)) {
        developmentRoot_ = *root;
        adbPath_ = developmentRoot_ / ".tools" / "android-sdk" / "platform-tools" / "adb.exe";
        runtimeRoot_ = developmentRoot_;
        diagnosticsDirectory_ = developmentRoot_ / "captures";
        defaultRecordingDirectory_ = developmentRoot_ / "recordings";
        const auto adbKey = developmentRoot_ / ".tools" / "adb-keys" / "adbkey";
        if (fs::exists(adbKey)) SetEnvironmentVariableW(L"ADB_VENDOR_KEYS", adbKey.wstring().c_str());
    } else {
        throw std::runtime_error("Could not locate bundled platform-tools/adb.exe beside StratoCam.");
    }
    std::error_code ignored;
    fs::create_directories(runtimeRoot_, ignored);
    fs::create_directories(diagnosticsDirectory_, ignored);
}

std::optional<fs::path> AdbManager::FindDevelopmentRoot(const fs::path& start) {
    fs::path current = start;
    for (int depth = 0; depth < 8; ++depth) {
        if (fs::exists(current / ".tools" / "android-sdk" / "platform-tools" / "adb.exe"))
            return current;
        if (!current.has_parent_path() || current.parent_path() == current) break;
        current = current.parent_path();
    }
    return std::nullopt;
}

fs::path AdbManager::KnownFolder(REFKNOWNFOLDERID id, const fs::path& fallback) {
    PWSTR value = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &value)) && value) {
        fs::path result(value);
        CoTaskMemFree(value);
        return result;
    }
    if (value) CoTaskMemFree(value);
    return fallback;
}

AdbManager::ProcessResult AdbManager::Run(const std::wstring& arguments) const {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) throw std::runtime_error("CreatePipe failed: " + LastErrorText());
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + adbPath_.wstring() + L"\" " + arguments;
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                        runtimeRoot_.wstring().c_str(), &startup, &process)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        throw std::runtime_error("Could not start adb: " + LastErrorText());
    }
    CloseHandle(writePipe);
    std::string output;
    std::array<char, 4096> buffer{};
    const ULONGLONG deadline = GetTickCount64() + 15'000;
    bool timedOut = false;
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr)) break;
        if (available > 0) {
            DWORD read = 0;
            if (!ReadFile(readPipe, buffer.data(), std::min<DWORD>(available, buffer.size()), &read, nullptr)) break;
            output.append(buffer.data(), read);
        } else if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
            // A newly started ADB server can inherit the output pipe. Waiting
            // for EOF would then wait for the server rather than this command.
            break;
        } else {
            Sleep(10);
        }
        if (GetTickCount64() >= deadline) {
            timedOut = true;
            TerminateProcess(process.hProcess, ERROR_TIMEOUT);
            WaitForSingleObject(process.hProcess, 1000);
            break;
        }
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(readPipe);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (timedOut) {
        output += "\nADB command timed out after 15 seconds";
        exitCode = ERROR_TIMEOUT;
    }
    return {exitCode, output};
}

AdbManager::ProcessResult AdbManager::RunForDevice(const std::wstring& arguments) const {
    std::wstring serial;
    { std::lock_guard lock(deviceMutex_); serial = deviceSerial_; }
    if (serial.empty()) return {1, "No authorized Android device selected"};
    return Run(L"-s " + serial + L" " + arguments);
}

bool AdbManager::RecreateForward(const wchar_t* port, std::string& status) const {
    RunForDevice(std::wstring(L"forward --remove ") + port);
    const auto forwarded = RunForDevice(std::wstring(L"forward ") + port + L" " + port);
    if (forwarded.exitCode != 0) {
        status = "adb forward failed for " + WideToUtf8(port) + ": " + forwarded.output;
        return false;
    }
    return true;
}

bool AdbManager::RepairVideoForward(std::string& status) {
    try {
        if (!RecreateForward(L"tcp:27183", status)) return false;
        if (!RecreateForward(L"tcp:27184", status)) return false;
        status = "Recreated ADB control/video forwards on tcp:27183 and tcp:27184";
        return true;
    } catch (const std::exception& error) {
        status = error.what();
        return false;
    }
}

bool AdbManager::Prepare(std::string& status) {
    try {
        if (!fs::exists(adbPath_)) throw std::runtime_error("ADB not found at " + adbPath_.string());
        PipelineDiagnostics::Log("adb", "Discovering device using " + adbPath_.string());
        const auto devices = Run(L"devices");
        if (devices.exitCode != 0) throw std::runtime_error("adb devices failed: " + devices.output);
        size_t authorized = 0;
        std::string selectedSerial;
        std::istringstream lines(devices.output);
        for (std::string line; std::getline(lines, line);) {
            const size_t deviceMarker = line.find("\tdevice");
            if (deviceMarker != std::string::npos) {
                ++authorized;
                selectedSerial = line.substr(0, deviceMarker);
            }
            if (line.find("\tunauthorized") != std::string::npos) {
                throw std::runtime_error("Phone is unauthorized. Accept USB debugging authorization on the phone.");
            }
        }
        if (authorized == 0) throw std::runtime_error("Waiting for an authorized Android phone. Check USB and accept debugging authorization.");
        if (authorized != 1) throw std::runtime_error("Expected exactly one authorized Android device; found " + std::to_string(authorized));
        if (selectedSerial.empty() || !std::all_of(selectedSerial.begin(), selectedSerial.end(), [](unsigned char value) {
                return std::isalnum(value) || value == '.' || value == '_' || value == '-' || value == ':';
            })) {
            throw std::runtime_error("ADB returned an invalid device serial");
        }
        { std::lock_guard lock(deviceMutex_); deviceSerial_ = Utf8ToWide(selectedSerial); }
        std::string forwardStatus;
        for (const wchar_t* port : {L"tcp:27183", L"tcp:27184"}) {
            if (!RecreateForward(port, forwardStatus)) throw std::runtime_error(forwardStatus);
        }
        const auto activityIsResumed = [this]() {
            const auto activities = RunForDevice(L"shell dumpsys activity activities");
            if (activities.exitCode != 0) return false;
            std::istringstream lines(activities.output);
            for (std::string line; std::getline(lines, line);) {
                if ((line.find("mResumedActivity") != std::string::npos ||
                     line.find("topResumedActivity") != std::string::npos ||
                     line.find("ResumedActivity:") != std::string::npos) &&
                    line.find("com.androidcam.inspector/.MainActivity") != std::string::npos) {
                    return true;
                }
            }
            return false;
        };
        if (!activityIsResumed()) {
            const auto launched = RunForDevice(L"shell am start -n com.androidcam.inspector/.MainActivity");
            if (launched.exitCode != 0 || launched.output.find("Error:") != std::string::npos ||
                launched.output.find("does not exist") != std::string::npos) {
                throw std::runtime_error("Could not launch Android app: " + launched.output);
            }
            bool resumed = false;
            for (int attempt = 0; attempt < 25 && !resumed; ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                resumed = activityIsResumed();
            }
            if (!resumed) throw std::runtime_error("Android app did not reach the resumed state");
        }
        status = "Android device connected through ADB";
        return true;
    } catch (const std::exception& error) {
        status = error.what();
        return false;
    }
}

std::optional<int> AdbManager::GetDisplayRotationDegrees() const {
    try {
        const auto result = RunForDevice(L"shell dumpsys input");
        if (result.exitCode != 0) return std::nullopt;
        const std::string marker = "SurfaceOrientation:";
        const size_t start = result.output.find(marker);
        if (start == std::string::npos) return std::nullopt;
        const char* value = result.output.c_str() + start + marker.size();
        char* end = nullptr;
        const long quarterTurns = std::strtol(value, &end, 10);
        if (end == value || quarterTurns < 0 || quarterTurns > 3) return std::nullopt;
        return static_cast<int>(quarterTurns) * 90;
    } catch (...) {
        return std::nullopt;
    }
}
