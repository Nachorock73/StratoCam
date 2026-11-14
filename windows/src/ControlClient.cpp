#include "ControlClient.h"

#include <cctype>
#include <iomanip>

namespace {
std::optional<size_t> ValueStart(const std::string& json, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    size_t position = json.find(token);
    if (position == std::string::npos) return std::nullopt;
    position = json.find(':', position + token.size());
    if (position == std::string::npos) return std::nullopt;
    do { ++position; } while (position < json.size() && std::isspace(static_cast<unsigned char>(json[position])));
    return position;
}

std::string NumericParams(const std::vector<std::pair<std::string, std::string>>& values) {
    std::ostringstream json;
    json << '{';
    for (size_t index = 0; index < values.size(); ++index) {
        if (index) json << ',';
        json << '"' << values[index].first << "\":" << values[index].second;
    }
    json << '}';
    return json.str();
}

bool SendAll(SOCKET socket, const char* bytes, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        const int result = send(socket, bytes + sent, static_cast<int>(size - sent), 0);
        if (result <= 0) return false;
        sent += static_cast<size_t>(result);
    }
    return true;
}
}

namespace JsonLite {
std::optional<std::string> Object(const std::string& json, const std::string& key) {
    const auto startValue = ValueStart(json, key);
    if (!startValue || *startValue >= json.size() || json[*startValue] != '{') return std::nullopt;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (size_t index = *startValue; index < json.size(); ++index) {
        const char value = json[index];
        if (inString) {
            if (escaped) escaped = false;
            else if (value == '\\') escaped = true;
            else if (value == '"') inString = false;
            continue;
        }
        if (value == '"') inString = true;
        else if (value == '{') ++depth;
        else if (value == '}' && --depth == 0) return json.substr(*startValue, index - *startValue + 1);
    }
    return std::nullopt;
}

std::optional<double> Number(const std::string& json, const std::string& key) {
    const auto start = ValueStart(json, key);
    if (!start) return std::nullopt;
    char* end = nullptr;
    const double result = std::strtod(json.c_str() + *start, &end);
    if (end == json.c_str() + *start) return std::nullopt;
    return result;
}

std::optional<bool> Boolean(const std::string& json, const std::string& key) {
    const auto start = ValueStart(json, key);
    if (!start) return std::nullopt;
    if (json.compare(*start, 4, "true") == 0) return true;
    if (json.compare(*start, 5, "false") == 0) return false;
    return std::nullopt;
}

std::optional<std::string> String(const std::string& json, const std::string& key) {
    const auto start = ValueStart(json, key);
    if (!start || *start >= json.size() || json[*start] != '"') return std::nullopt;
    std::string result;
    bool escaped = false;
    for (size_t index = *start + 1; index < json.size(); ++index) {
        const char value = json[index];
        if (escaped) { result.push_back(value); escaped = false; }
        else if (value == '\\') escaped = true;
        else if (value == '"') return result;
        else result.push_back(value);
    }
    return std::nullopt;
}
}

bool ControlClient::Send(const std::string& command, const std::string& params, std::string& response, std::string& error) {
    SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == INVALID_SOCKET) { error = "Control socket creation failed"; return false; }
    DWORD timeout = 3000;
    setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(27183);
    inet_pton(AF_INET, "127.0.0.1", &endpoint.sin_addr);
    if (connect(socketHandle, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) == SOCKET_ERROR) {
        error = "Could not connect to Android control channel: " + std::to_string(WSAGetLastError());
        closesocket(socketHandle);
        return false;
    }
    std::ostringstream request;
    request << "{\"version\":1,\"id\":\"win-" << GetTickCount64() << "\",\"command\":\""
            << command << "\",\"params\":" << params << "}\n";
    const std::string wire = request.str();
    if (!SendAll(socketHandle, wire.data(), wire.size())) {
        error = "Failed to send control request";
        closesocket(socketHandle);
        return false;
    }
    std::array<char, 2048> buffer{};
    while (response.find('\n') == std::string::npos && response.size() < 64 * 1024) {
        const int received = recv(socketHandle, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received <= 0) break;
        response.append(buffer.data(), received);
    }
    closesocket(socketHandle);
    if (response.empty()) { error = "Android returned an empty control response"; return false; }
    if (JsonLite::Boolean(response, "ok") != true) { error = "Android rejected control request: " + response; return false; }
    return true;
}

bool ControlClient::ParseState(const std::string& response, CameraState& state, std::string& error) {
    const auto json = JsonLite::Object(response, "state");
    if (!json) { error = "Control response did not contain camera state"; return false; }
    state.autoExposure = JsonLite::Boolean(*json, "autoExposure").value_or(state.autoExposure);
    state.iso = static_cast<int>(JsonLite::Number(*json, "iso").value_or(state.iso));
    state.exposureNs = static_cast<int64_t>(JsonLite::Number(*json, "exposureTimeNs").value_or(static_cast<double>(state.exposureNs)));
    state.autoFocus = JsonLite::Boolean(*json, "autoFocus").value_or(state.autoFocus);
    state.focusDistance = JsonLite::Number(*json, "focusDistanceDiopters").value_or(state.focusDistance);
    state.autoWhiteBalance = JsonLite::Boolean(*json, "autoWhiteBalance").value_or(state.autoWhiteBalance);
    state.redGain = JsonLite::Number(*json, "redGain").value_or(state.redGain);
    state.blueGain = JsonLite::Number(*json, "blueGain").value_or(state.blueGain);
    state.zoom = JsonLite::Number(*json, "zoomRatio").value_or(state.zoom);
    return true;
}

bool ControlClient::GetCapabilities(CameraCapabilities& capabilities, CameraState& state, std::string& error) {
    std::string response;
    if (!Send("get_capabilities", "{}", response, error) || !ParseState(response, state, error)) return false;
    const auto json = JsonLite::Object(response, "capabilities");
    if (!json) { error = "Control response did not contain capabilities"; return false; }
    capabilities.manualExposure = JsonLite::Boolean(*json, "manualExposureSupported").value_or(false);
    capabilities.isoMin = static_cast<int>(JsonLite::Number(*json, "isoMin").value_or(100));
    capabilities.isoMax = static_cast<int>(JsonLite::Number(*json, "isoMax").value_or(100));
    capabilities.exposureMinNs = static_cast<int64_t>(JsonLite::Number(*json, "exposureTimeMinNs").value_or(1));
    capabilities.exposureMaxNs = static_cast<int64_t>(JsonLite::Number(*json, "exposureTimeMaxNs").value_or(33'333'333));
    capabilities.maxFrameDurationNs = static_cast<int64_t>(JsonLite::Number(*json, "maxFrameDurationNs").value_or(33'333'333));
    capabilities.manualFocus = JsonLite::Boolean(*json, "manualFocusSupported").value_or(false);
    capabilities.focusMax = JsonLite::Number(*json, "maxFocusDistanceDiopters").value_or(0.0);
    capabilities.manualWhiteBalance = JsonLite::Boolean(*json, "manualWhiteBalanceSupported").value_or(false);
    capabilities.gainMin = JsonLite::Number(*json, "colorGainMin").value_or(1.0);
    capabilities.gainMax = JsonLite::Number(*json, "colorGainMax").value_or(3.0);
    capabilities.zoomMin = JsonLite::Number(*json, "zoomMin").value_or(1.0);
    capabilities.zoomMax = JsonLite::Number(*json, "zoomMax").value_or(1.0);
    capabilities.zoomRatioSupported = JsonLite::Boolean(*json, "zoomRatioSupported").value_or(false);
    capabilities.maxAfRegions = static_cast<int>(JsonLite::Number(*json, "maxAfRegions").value_or(0));
    capabilities.activeArrayLeft = static_cast<int>(JsonLite::Number(*json, "activeArrayLeft").value_or(0));
    capabilities.activeArrayTop = static_cast<int>(JsonLite::Number(*json, "activeArrayTop").value_or(0));
    capabilities.activeArrayRight = static_cast<int>(JsonLite::Number(*json, "activeArrayRight").value_or(0));
    capabilities.activeArrayBottom = static_cast<int>(JsonLite::Number(*json, "activeArrayBottom").value_or(0));
    return true;
}

bool ControlClient::SetExposure(bool automatic, int iso, int64_t exposureNs, CameraState& state, std::string& error) {
    std::string response;
    const auto params = NumericParams({{"auto", automatic ? "true" : "false"}, {"iso", std::to_string(iso)},
                                       {"exposureTimeNs", std::to_string(exposureNs)}});
    return Send("set_exposure", params, response, error) && ParseState(response, state, error);
}

bool ControlClient::SetFocus(bool automatic, double diopters, CameraState& state, std::string& error) {
    std::string response;
    const auto params = NumericParams({{"auto", automatic ? "true" : "false"}, {"focusDistanceDiopters", std::to_string(diopters)}});
    return Send("set_focus", params, response, error) && ParseState(response, state, error);
}

bool ControlClient::SetWhiteBalance(bool automatic, double red, double blue, CameraState& state, std::string& error) {
    std::string response;
    const auto params = NumericParams({{"auto", automatic ? "true" : "false"}, {"redGain", std::to_string(red)},
                                       {"blueGain", std::to_string(blue)}});
    return Send("set_white_balance", params, response, error) && ParseState(response, state, error);
}

bool ControlClient::SetZoom(double zoom, CameraState& state, std::string& error) {
    std::string response;
    return Send("set_zoom", NumericParams({{"zoomRatio", std::to_string(zoom)}}), response, error) && ParseState(response, state, error);
}

bool ControlClient::Reset(CameraState& state, std::string& error) {
    std::string response;
    return Send("reset_auto", "{}", response, error) && ParseState(response, state, error);
}

bool ControlClient::ParseFocus(const std::string& response, FocusPointResult& result, std::string& error) {
    const auto focus = JsonLite::Object(response, "focus");
    if (!focus) { error = "Control response did not contain focus status"; return false; }
    result.requestId = JsonLite::String(*focus, "requestId").value_or("");
    result.status = JsonLite::String(*focus, "status").value_or("");
    result.sensorX = static_cast<int>(JsonLite::Number(*focus, "sensorX").value_or(0));
    result.sensorY = static_cast<int>(JsonLite::Number(*focus, "sensorY").value_or(0));
    return !result.status.empty();
}

bool ControlClient::FocusAt(int sensorX, int sensorY, FocusPointResult& result, std::string& error) {
    std::string response;
    const auto params = NumericParams({{"sensorX", std::to_string(sensorX)}, {"sensorY", std::to_string(sensorY)}});
    return Send("focus_at", params, response, error) && ParseFocus(response, result, error);
}

bool ControlClient::GetFocusStatus(FocusPointResult& result, std::string& error) {
    std::string response;
    return Send("get_focus_status", "{}", response, error) && ParseFocus(response, result, error);
}

bool ControlClient::ResumeContinuousFocus(FocusPointResult& result, std::string& error) {
    std::string response;
    return Send("resume_continuous_focus", "{}", response, error) && ParseFocus(response, result, error);
}

bool ControlClient::GetDeviceStatus(DeviceStatus& status, std::string& error) {
    std::string response;
    if (!Send("get_device_status", "{}", response, error)) return false;
    const auto json = JsonLite::Object(response, "deviceStatus");
    if (!json) { error = "Control response did not contain device status"; return false; }
    status.batteryPercent = static_cast<int>(JsonLite::Number(*json, "batteryPercent").value_or(-1));
    status.batteryState = JsonLite::String(*json, "batteryState").value_or("unknown");
    status.powerSource = JsonLite::String(*json, "powerSource").value_or("unknown");
    status.batteryHealth = JsonLite::String(*json, "batteryHealth").value_or("unknown");
    status.batteryTemperatureC = JsonLite::Number(*json, "batteryTemperatureC").value_or(
        std::numeric_limits<double>::quiet_NaN());
    status.currentNowMilliamps = JsonLite::Number(*json, "currentNowMilliamps").value_or(
        std::numeric_limits<double>::quiet_NaN());
    status.currentAverageMilliamps = JsonLite::Number(*json, "currentAverageMilliamps").value_or(
        std::numeric_limits<double>::quiet_NaN());
    status.voltageVolts = JsonLite::Number(*json, "voltageVolts").value_or(
        std::numeric_limits<double>::quiet_NaN());
    status.approximatePowerWatts = JsonLite::Number(*json, "approximatePowerWatts").value_or(
        std::numeric_limits<double>::quiet_NaN());
    status.thermalStatus = JsonLite::String(*json, "thermalStatus").value_or("unknown");
    return true;
}

bool ControlClient::SetVideoMode(int width, int height, int fps, int bitrate, std::string& error) {
    std::string response;
    return Send("set_video_mode", NumericParams({
        {"width", std::to_string(width)}, {"height", std::to_string(height)},
        {"fps", std::to_string(fps)}, {"bitrate", std::to_string(bitrate)}}), response, error);
}
