#pragma once

#include "Common.h"
#include <filesystem>
#include <shlobj.h>

class AdbManager {
public:
    AdbManager();
    bool Prepare(std::string& status);
    bool RepairVideoForward(std::string& status);
    std::optional<int> GetDisplayRotationDegrees() const;
    const std::filesystem::path& RuntimeRoot() const { return runtimeRoot_; }
    const std::filesystem::path& DiagnosticsDirectory() const { return diagnosticsDirectory_; }
    const std::filesystem::path& DefaultRecordingDirectory() const { return defaultRecordingDirectory_; }

private:
    struct ProcessResult { DWORD exitCode{}; std::string output; };
    static std::optional<std::filesystem::path> FindDevelopmentRoot(const std::filesystem::path& start);
    static std::filesystem::path KnownFolder(REFKNOWNFOLDERID id, const std::filesystem::path& fallback);
    ProcessResult Run(const std::wstring& arguments) const;
    ProcessResult RunForDevice(const std::wstring& arguments) const;
    bool RecreateForward(const wchar_t* port, std::string& status) const;

    std::filesystem::path applicationDirectory_;
    std::filesystem::path developmentRoot_;
    std::filesystem::path runtimeRoot_;
    std::filesystem::path diagnosticsDirectory_;
    std::filesystem::path defaultRecordingDirectory_;
    std::filesystem::path adbPath_;
    mutable std::mutex deviceMutex_;
    std::wstring deviceSerial_;
};
