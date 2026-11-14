#pragma once

#include "ControlClient.h"
#include <iomanip>
#include <locale>
#include <climits>

struct SessionSettings {
    int videoMode{};
    int orientation{};
    int scale{};
    int audioOffsetMs{};
    std::string audioDeviceId;
    bool hasCameraState{};
    CameraState camera;
};

inline std::string EncodeSessionSettings(const SessionSettings& value) {
    std::ostringstream json;
    json.imbue(std::locale::classic());
    json << std::setprecision(17) << std::boolalpha
         << "{\"schema\":1,\"videoMode\":" << value.videoMode
         << ",\"orientation\":" << value.orientation << ",\"scale\":" << value.scale
         << ",\"audioOffsetMs\":" << value.audioOffsetMs << ",\"audioDeviceId\":";
    json << '"';
    for (const unsigned char c : value.audioDeviceId) {
        if (c == '"' || c == '\\') json << '\\';
        if (c < 32) continue;
        json << static_cast<char>(c);
    }
    json << "\",\"hasCameraState\":" << value.hasCameraState
         << ",\"autoExposure\":" << value.camera.autoExposure << ",\"iso\":" << value.camera.iso
         << ",\"exposureNs\":" << value.camera.exposureNs
         << ",\"autoFocus\":" << value.camera.autoFocus << ",\"focusDistance\":" << value.camera.focusDistance
         << ",\"autoWhiteBalance\":" << value.camera.autoWhiteBalance
         << ",\"redGain\":" << value.camera.redGain << ",\"blueGain\":" << value.camera.blueGain
         << ",\"zoom\":" << value.camera.zoom << '}';
    return json.str();
}

inline std::optional<SessionSettings> DecodeSessionSettings(const std::string& json) {
    if (JsonLite::Number(json, "schema") != 1) return std::nullopt;
    SessionSettings value;
    const auto number = [&](const char* key, double fallback, double low, double high) {
        const double parsed = JsonLite::Number(json, key).value_or(fallback);
        return std::isfinite(parsed) && parsed >= low && parsed <= high ? parsed : fallback;
    };
    value.videoMode = static_cast<int>(number("videoMode", 0, 0, 1));
    value.orientation = static_cast<int>(number("orientation", 0, 0, 5));
    value.scale = static_cast<int>(number("scale", 0, 0, 2));
    value.audioOffsetMs = static_cast<int>(number("audioOffsetMs", 0, -5000, 5000));
    value.audioDeviceId = JsonLite::String(json, "audioDeviceId").value_or("");
    value.hasCameraState = JsonLite::Boolean(json, "hasCameraState").value_or(false);
    value.camera.autoExposure = JsonLite::Boolean(json, "autoExposure").value_or(true);
    value.camera.iso = static_cast<int>(number("iso", 100, 1, INT_MAX));
    value.camera.exposureNs = static_cast<int64_t>(number("exposureNs", 16'666'667, 1, 1e15));
    value.camera.autoFocus = JsonLite::Boolean(json, "autoFocus").value_or(true);
    value.camera.focusDistance = number("focusDistance", 0, 0, 1e6);
    value.camera.autoWhiteBalance = JsonLite::Boolean(json, "autoWhiteBalance").value_or(true);
    value.camera.redGain = number("redGain", 1, 0, 1e6);
    value.camera.blueGain = number("blueGain", 1, 0, 1e6);
    value.camera.zoom = number("zoom", 1, 1e-6, 1e6);
    return value;
}

inline CameraState RestoreSupportedCameraState(CameraState value, const CameraCapabilities& caps) {
    value.autoExposure = value.autoExposure || !caps.manualExposure;
    value.autoFocus = value.autoFocus || !caps.manualFocus;
    value.autoWhiteBalance = value.autoWhiteBalance || !caps.manualWhiteBalance;
    value.iso = std::clamp(value.iso, caps.isoMin, std::max(caps.isoMin, caps.isoMax));
    const auto exposureMax = std::max(caps.exposureMinNs, std::min(caps.exposureMaxNs, caps.maxFrameDurationNs));
    value.exposureNs = std::clamp(value.exposureNs, caps.exposureMinNs, exposureMax);
    value.focusDistance = std::clamp(value.focusDistance, 0.0, std::max(0.0, caps.focusMax));
    value.redGain = std::clamp(value.redGain, caps.gainMin, std::max(caps.gainMin, caps.gainMax));
    value.blueGain = std::clamp(value.blueGain, caps.gainMin, std::max(caps.gainMin, caps.gainMax));
    value.zoom = std::clamp(value.zoom, caps.zoomMin, std::max(caps.zoomMin, caps.zoomMax));
    return value;
}

inline constexpr wchar_t kSessionPreferenceKey[] = L"Software\\AndroidCamStudio";

inline std::optional<SessionSettings> LoadSessionSettings(const wchar_t* keyPath = kSessionPreferenceKey) {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, keyPath, L"SessionSettings", RRF_RT_REG_SZ,
                     nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t) || bytes > 64 * 1024)
        return std::nullopt;
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, keyPath, L"SessionSettings", RRF_RT_REG_SZ,
                     nullptr, value.data(), &bytes) != ERROR_SUCCESS) return std::nullopt;
    value.resize(std::wcslen(value.c_str()));
    return DecodeSessionSettings(WideToUtf8(value));
}

inline bool SaveSessionSettings(const SessionSettings& value, const wchar_t* keyPath = kSessionPreferenceKey) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, keyPath, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    const auto json = Utf8ToWide(EncodeSessionSettings(value));
    const auto result = RegSetValueExW(key, L"SessionSettings", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(json.c_str()), static_cast<DWORD>((json.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}
