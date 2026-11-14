#pragma once

#include "AdbManager.h"
#include "ControlClient.h"
#include "D3DRenderer.h"
#include "DirectH264Recorder.h"
#include "H264Decoder.h"
#include "Metrics.h"
#include "ObsFramePublisher.h"
#include "PreviewGeometry.h"
#include "VideoTransport.h"
#include "VirtualCameraFramePublisher.h"
#include "WasapiAudioCapture.h"

#include <commctrl.h>

class AppWindow {
public:
    AppWindow();
    ~AppWindow();
    bool Create(HINSTANCE instance, int showCommand);
    HWND Handle() const { return window_; }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK PreviewProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
    bool OnCreate();
    void OnDestroy();
    void Layout(int width, int height);
    void PaintBackground(HDC dc);
    bool DrawButton(const DRAWITEMSTRUCT& item);
    LRESULT DrawControl(NMCUSTOMDRAW* customDraw);
    void BeginConnect();
    void StopStream();
    void ToggleRecording();
    void ChooseRecordingFolder();
    void LoadRecordingFolderPreference();
    bool SaveRecordingFolderPreference(std::string& error) const;
    bool ValidateRecordingFolder(std::string& error) const;
    void UpdateRecordingFolderLabel();
    void SelectAudioInput(int selection);
    void ExportLatency();
    void ApplyCapabilities();
    void UpdateStatusPanel();
    void UpdateViewTransform();
    void PollOrientation();
    void PollFocusStatus();
    void PollDeviceStatus();
    void HandlePreviewClick(int x, int y);
    void ResumeContinuousFocus();
    void ApplyFocusStatus(const FocusPointResult& result);
    void HandleCommand(int id, int notification);
    void HandleSlider(HWND slider, int notification);
    void SendExposure(std::optional<bool> automatic = std::nullopt);
    void SendFocus(std::optional<bool> automatic = std::nullopt);
    void SendWhiteBalance(std::optional<bool> automatic = std::nullopt);
    void SendZoom(double requested = -1.0);
    void PostStatus(const std::string& status);
    HWND AddControl(const wchar_t* type, const wchar_t* text, DWORD style, int id = 0);
    HWND AddLabel(const wchar_t* text, int id = 0, HFONT font = nullptr);
    void StyleControl(HWND control, HFONT font = nullptr);
    static double LogValue(int position, double minimum, double maximum);
    static int LogPosition(double value, double minimum, double maximum);
    static std::wstring FormatMs(double value);
    static std::wstring FormatNumber(double value, int precision);

    HINSTANCE instance_{};
    HWND window_{};
    HWND preview_{};
    HWND title_{};
    HWND subtitle_{};
    HWND connectionStatus_{};
    HWND streamStatus_{};
    HWND batteryStatus_{};
    HWND modeCombo_{};
    HWND startButton_{};
    HWND stopButton_{};
    HWND recordButton_{};
    HWND recordStatus_{};
    HWND audioTitle_{};
    HWND audioInputCombo_{};
    HWND audioLevel_{};
    HWND audioOffsetLabel_{};
    HWND audioOffsetEdit_{};
    HWND recordingFolderLabel_{};
    HWND recordingFolderButton_{};
    HWND status_{};
    HWND orientationCombo_{};
    HWND scaleCombo_{};
    HWND orientationValue_{};
    HWND exposureTitle_{};
    HWND aeCheck_{};
    HWND isoSlider_{};
    HWND isoValue_{};
    HWND shutterSlider_{};
    HWND shutterValue_{};
    HWND focusTitle_{};
    HWND afCheck_{};
    HWND focusSlider_{};
    HWND focusValue_{};
    HWND resumeFocusButton_{};
    HWND wbTitle_{};
    HWND awbCheck_{};
    HWND redSlider_{};
    HWND redValue_{};
    HWND blueSlider_{};
    HWND blueValue_{};
    HWND zoomTitle_{};
    HWND zoomSlider_{};
    HWND zoomValue_{};
    HWND metricResolution_{};
    HWND metricFps_{};
    HWND metricBitrate_{};
    HWND metricLatency_{};
    HWND metricDrops_{};

    HFONT titleFont_{};
    HFONT subtitleFont_{};
    HFONT sectionFont_{};
    HFONT bodyFont_{};
    HFONT metricFont_{};
    HBRUSH backgroundBrush_{};
    HBRUSH panelBrush_{};

    std::mutex stateMutex_;
    CameraCapabilities capabilities_;
    CameraState state_;
    StreamInfo streamInfo_;
    std::string latestStatus_{"Starting"};
    std::atomic<bool> closing_{false};
    std::atomic<bool> connecting_{false};
    std::atomic<bool> connectionRequested_{true};
    std::atomic<uint64_t> discoveryRetryAtMs_{};
    std::thread connectionThread_;
    std::thread orientationThread_;
    std::thread focusThread_;
    std::thread deviceStatusThread_;
    std::atomic<uint64_t> renderedFrames_{};
    std::atomic<int> deviceRotationDegrees_{};
    std::atomic<int> orientationSelection_{};
    std::atomic<int> scaleSelection_{};
    std::atomic<bool> connected_{};
    std::atomic<int> modeSelection_{};
    std::atomic<int> audioSelection_{};
    std::vector<AudioCaptureDevice> audioDevices_;
    std::filesystem::path recordingFolder_;
    DeviceStatus deviceStatus_;
    std::atomic<int> effectiveRotationDegrees_{90};
    std::mutex focusMutex_;
    std::string activeFocusRequestId_;
    int focusVisualState_{};
    double focusIndicatorExpiresNs_{};

    LatencyCollector latency_;
    ObsFramePublisher obsPublisher_;
    VirtualCameraFramePublisher virtualCameraPublisher_;
    AdbManager adb_;
    ControlClient controls_;
    D3DRenderer renderer_;
    H264Decoder decoder_;
    DirectH264Recorder recorder_;
    WasapiAudioCapture audioCapture_;
    VideoTransport transport_;
};
