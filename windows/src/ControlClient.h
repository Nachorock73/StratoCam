#pragma once

#include "Common.h"

struct CameraCapabilities {
    int isoMin{100};
    int isoMax{100};
    int64_t exposureMinNs{16'666'667};
    int64_t exposureMaxNs{16'666'667};
    int64_t maxFrameDurationNs{33'333'333};
    double focusMax{0.0};
    double gainMin{1.0};
    double gainMax{3.0};
    double zoomMin{1.0};
    double zoomMax{1.0};
    bool manualExposure{};
    bool manualFocus{};
    bool manualWhiteBalance{};
    bool zoomRatioSupported{};
    int maxAfRegions{};
    int activeArrayLeft{};
    int activeArrayTop{};
    int activeArrayRight{};
    int activeArrayBottom{};
};

struct FocusPointResult {
    std::string requestId;
    std::string status;
    int sensorX{};
    int sensorY{};
};

struct CameraState {
    bool autoExposure{true};
    int iso{100};
    int64_t exposureNs{16'666'667};
    bool autoFocus{true};
    double focusDistance{};
    bool autoWhiteBalance{true};
    double redGain{1.0};
    double blueGain{1.0};
    double zoom{1.0};
};

struct DeviceStatus {
    int batteryPercent{-1};
    std::string batteryState{"unknown"};
    std::string powerSource{"unknown"};
    std::string batteryHealth{"unknown"};
    double batteryTemperatureC{std::numeric_limits<double>::quiet_NaN()};
    double currentNowMilliamps{std::numeric_limits<double>::quiet_NaN()};
    double currentAverageMilliamps{std::numeric_limits<double>::quiet_NaN()};
    double voltageVolts{std::numeric_limits<double>::quiet_NaN()};
    double approximatePowerWatts{std::numeric_limits<double>::quiet_NaN()};
    std::string thermalStatus{"unknown"};
};

enum class AutomaticControl { Exposure, Focus, WhiteBalance };

inline CameraState ToggleAutomaticControl(CameraState state, AutomaticControl control) {
    switch (control) {
        case AutomaticControl::Exposure: state.autoExposure = !state.autoExposure; break;
        case AutomaticControl::Focus: state.autoFocus = !state.autoFocus; break;
        case AutomaticControl::WhiteBalance: state.autoWhiteBalance = !state.autoWhiteBalance; break;
    }
    return state;
}

namespace JsonLite {
std::optional<std::string> Object(const std::string& json, const std::string& key);
std::optional<double> Number(const std::string& json, const std::string& key);
std::optional<bool> Boolean(const std::string& json, const std::string& key);
std::optional<std::string> String(const std::string& json, const std::string& key);
}

class ControlClient {
public:
    bool GetCapabilities(CameraCapabilities& capabilities, CameraState& state, std::string& error);
    bool SetExposure(bool automatic, int iso, int64_t exposureNs, CameraState& state, std::string& error);
    bool SetFocus(bool automatic, double diopters, CameraState& state, std::string& error);
    bool SetWhiteBalance(bool automatic, double red, double blue, CameraState& state, std::string& error);
    bool SetZoom(double zoom, CameraState& state, std::string& error);
    bool Reset(CameraState& state, std::string& error);
    bool FocusAt(int sensorX, int sensorY, FocusPointResult& result, std::string& error);
    bool GetFocusStatus(FocusPointResult& result, std::string& error);
    bool ResumeContinuousFocus(FocusPointResult& result, std::string& error);
    bool GetDeviceStatus(DeviceStatus& status, std::string& error);
    bool SetVideoMode(int width, int height, int fps, int bitrate, std::string& error);

private:
    bool Send(const std::string& command, const std::string& params, std::string& response, std::string& error);
    static bool ParseState(const std::string& response, CameraState& state, std::string& error);
    static bool ParseFocus(const std::string& response, FocusPointResult& result, std::string& error);
};
