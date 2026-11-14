#pragma once

#include "Common.h"

#include <filesystem>
#include <fstream>
#include <iomanip>

namespace PipelineDiagnostics {

inline std::mutex logMutex;
inline std::filesystem::path logPath;

inline void Reset(const std::filesystem::path& path) {
    std::lock_guard lock(logMutex);
    logPath = path;
    std::filesystem::create_directories(logPath.parent_path());
    std::ofstream(logPath, std::ios::trunc) << "# StratoCam native video pipeline diagnostics\n";
}

inline void Log(const char* stage, const std::string& message) {
    const std::string line = "[" + std::to_string(static_cast<uint64_t>(QpcNanoseconds())) + "] [" +
                             stage + "] " + message + "\n";
    OutputDebugStringA(line.c_str());
    std::lock_guard lock(logMutex);
    if (!logPath.empty()) std::ofstream(logPath, std::ios::app) << line;
}

}  // namespace PipelineDiagnostics
