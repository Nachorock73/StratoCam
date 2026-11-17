#include "AppWindow.h"
#include "DeviceDiscoveryRetry.h"
#include "Resource.h"
#include "Version.h"
#include "PipelineDiagnostics.h"

#include <dwmapi.h>
#include <iomanip>
#include <shobjidl.h>
#include <uxtheme.h>

namespace {
constexpr wchar_t kWindowClass[] = L"AndroidCamMainWindow";
constexpr wchar_t kPreviewClass[] = L"AndroidCamPreviewWindow";
constexpr wchar_t kPanelClass[] = L"StratoCamControlsPanel";
constexpr UINT WM_APP_STATUS = WM_APP + 50;
constexpr UINT WM_APP_CONNECTED = WM_APP + 51;
constexpr UINT WM_APP_STREAM_INFO = WM_APP + 52;
constexpr UINT WM_APP_ORIENTATION = WM_APP + 53;
constexpr UINT WM_APP_FOCUS_STATUS = WM_APP + 54;
constexpr UINT WM_APP_DEVICE_STATUS = WM_APP + 55;

constexpr COLORREF kBackground = RGB(10, 14, 20);
constexpr COLORREF kPanel = RGB(18, 24, 33);
constexpr COLORREF kSurface = RGB(25, 33, 44);
constexpr COLORREF kBorder = RGB(46, 57, 71);
constexpr COLORREF kText = RGB(232, 237, 243);
constexpr COLORREF kMuted = RGB(139, 151, 164);
constexpr COLORREF kAccent = RGB(64, 139, 245);
constexpr COLORREF kAccentDark = RGB(34, 83, 148);
constexpr COLORREF kGreen = RGB(63, 185, 128);
constexpr COLORREF kRed = RGB(248, 81, 73);

const char* SizeStateLabel(WPARAM state) {
    switch (state) {
        case SIZE_MAXIMIZED: return "SIZE_MAXIMIZED";
        case SIZE_MINIMIZED: return "SIZE_MINIMIZED";
        case SIZE_RESTORED: return "SIZE_RESTORED";
        default: return "SIZE_UNKNOWN";
    }
}

enum ControlId {
    ID_START = 100, ID_STOP, ID_AE, ID_ISO, ID_SHUTTER, ID_AF, ID_FOCUS, ID_AWB,
    ID_RED, ID_BLUE, ID_ZOOM, ID_ZOOM_1, ID_ZOOM_15, ID_ZOOM_2, ID_RESET,
    ID_ORIENTATION, ID_SCALE, ID_CONNECTION, ID_STREAM, ID_STATUS, ID_RESUME_FOCUS, ID_MODE, ID_RECORD,
    ID_AUDIO_INPUT, ID_AUDIO_OFFSET, ID_RECORDING_FOLDER
};

constexpr wchar_t kPreferenceKey[] = L"Software\\AndroidCamStudio";
constexpr wchar_t kRecordingFolderValue[] = L"RecordingFolder";

HFONT MakeFont(int pixels, int weight, const wchar_t* face = L"Segoe UI Variable Text") {
    return CreateFontW(-pixels, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, face);
}

void FillRounded(HDC dc, const RECT& rect, int radius, COLORREF color, COLORREF border = CLR_INVALID) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, border == CLR_INVALID ? color : border);
    const auto oldBrush = SelectObject(dc, brush);
    const auto oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

bool IsSlider(HWND window) {
    const int id = GetDlgCtrlID(window);
    return id == ID_ISO || id == ID_SHUTTER || id == ID_FOCUS || id == ID_RED || id == ID_BLUE || id == ID_ZOOM;
}
}

AppWindow::AppWindow()
    : renderer_(
          [this](const FrameTiming& timing) {
              latency_.Add(timing);
              if (++renderedFrames_ % 30 == 0 && window_) PostMessageW(window_, WM_APP_STREAM_INFO, 0, 0);
          },
          [this](const std::string& status) { PostStatus(status); }),
      decoder_(
          [this](std::shared_ptr<DecodedFrame> frame) {
              const bool obsActive = obsPublisher_.HasActiveConsumer();
              const bool virtualCameraActive = virtualCameraPublisher_.HasActiveConsumer();
              if ((obsActive || virtualCameraActive) && !renderer_.EnsureCpuI420(*frame)) {
                  PipelineDiagnostics::Log("consumers", "DROP CPU consumers: NV12 readback failed");
              } else {
                  if (obsActive) obsPublisher_.Publish(*frame, effectiveRotationDegrees_.load());
                  if (virtualCameraActive) virtualCameraPublisher_.Publish(*frame, effectiveRotationDegrees_.load());
              }
              renderer_.Submit(std::move(frame));
          },
          [this](const std::string& status) { PostStatus(status); }),
      recorder_([this](const std::string& status) { PostStatus(status); }),
      audioCapture_(
          [this](AudioCaptureChunk&& chunk) { recorder_.EnqueueAudio(std::move(chunk)); },
          [this](const std::string& status) { PostStatus(status); }),
      transport_(
          [this](EncodedAccessUnit&& packet) {
              recorder_.Enqueue(packet);
              decoder_.Decode(std::move(packet));
          },
          [this](const StreamInfo& info) {
              { std::lock_guard lock(stateMutex_); streamInfo_ = info; }
              if (window_) PostMessageW(window_, WM_APP_STREAM_INFO, 0, 0);
          },
          [this](const std::string& status) { PostStatus(status); },
          [this](bool active) {
              if (active) {
                  obsPublisher_.StartStream();
                  virtualCameraPublisher_.StartStream();
              } else {
                  obsPublisher_.StopStream();
                  virtualCameraPublisher_.StopStream();
              }
              if (window_) PostMessageW(window_, WM_APP_STREAM_INFO, 0, 0);
          },
          [this](std::string& status) { return adb_.RepairVideoForward(status); }) {}

AppWindow::~AppWindow() { OnDestroy(); }

bool AppWindow::Create(HINSTANCE instance, int showCommand) {
    instance_ = instance;
    backgroundBrush_ = CreateSolidBrush(kBackground);
    panelBrush_ = CreateSolidBrush(kPanel);

    WNDCLASSEXW previewClass{};
    previewClass.cbSize = sizeof(previewClass);
    previewClass.lpfnWndProc = PreviewProc;
    previewClass.hInstance = instance;
    previewClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    previewClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    previewClass.lpszClassName = kPreviewClass;
    RegisterClassExW(&previewClass);
    WNDCLASSEXW panelClass = previewClass;
    panelClass.lpfnWndProc = PanelProc;
    panelClass.hbrBackground = panelBrush_;
    panelClass.lpszClassName = kPanelClass;
    RegisterClassExW(&panelClass);

    WNDCLASSEXW mainClass{};
    mainClass.cbSize = sizeof(mainClass);
    mainClass.lpfnWndProc = WindowProc;
    mainClass.hInstance = instance;
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.hbrBackground = backgroundBrush_;
    mainClass.lpszClassName = kWindowClass;
    mainClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_STRATOCAM));
    mainClass.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(IDI_STRATOCAM));
    if (!RegisterClassExW(&mainClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    dpi_ = GetDpiForSystem();
    RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    window_ = CreateWindowExW(0, kWindowClass, kProductName,
                              WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                              std::min(Px(1460), static_cast<int>(work.right - work.left)),
                              std::min(Px(920), static_cast<int>(work.bottom - work.top)),
                              nullptr, nullptr, instance, this);
    if (!window_) return false;
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));
    const COLORREF caption = kBackground;
    DwmSetWindowAttribute(window_, 35, &caption, sizeof(caption));
    ShowWindow(window_, showCommand);
    UpdateWindow(window_);
    return true;
}

HWND AppWindow::AddControl(const wchar_t* type, const wchar_t* text, DWORD style, int id) {
    HWND control = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                                   window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    StyleControl(control);
    return control;
}

HWND AppWindow::AddLabel(const wchar_t* text, int id, HFONT font) {
    HWND label = AddControl(L"STATIC", text, SS_LEFTNOWORDWRAP | SS_NOPREFIX, id);
    SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : bodyFont_), TRUE);
    return label;
}

void AppWindow::StyleControl(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : bodyFont_), TRUE);
    SetWindowTheme(control, L"DarkMode_Explorer", nullptr);
}

void AppWindow::UpdateFonts() {
    const HFONT old[] = {titleFont_, subtitleFont_, sectionFont_, bodyFont_, metricFont_};
    titleFont_ = MakeFont(Px(25), FW_SEMIBOLD, L"Segoe UI Variable Display");
    subtitleFont_ = MakeFont(Px(12), FW_NORMAL);
    sectionFont_ = MakeFont(Px(13), FW_SEMIBOLD);
    bodyFont_ = MakeFont(Px(12), FW_NORMAL);
    metricFont_ = MakeFont(Px(11), FW_SEMIBOLD);
    struct Fonts { const HFONT* old; HFONT current[5]; int comboHeight; } fonts{
        old, {titleFont_, subtitleFont_, sectionFont_, bodyFont_, metricFont_}, Px(24)};
    EnumChildWindows(window_, [](HWND control, LPARAM data) -> BOOL {
        const auto& fonts = *reinterpret_cast<Fonts*>(data);
        const auto previous = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
        for (int i = 0; i < 5; ++i) if (previous && previous == fonts.old[i]) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(fonts.current[i]), FALSE); break;
        }
        wchar_t type[32]{}; GetClassNameW(control, type, 32);
        if (wcscmp(type, L"ComboBox") == 0) {
            SendMessageW(control, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), fonts.comboHeight);
            SendMessageW(control, CB_SETITEMHEIGHT, 0, fonts.comboHeight);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&fonts));
    for (HFONT font : old) if (font) DeleteObject(font);
}

bool AppWindow::OnCreate() {
    dpi_ = GetDpiForWindow(window_);
    std::string obsError;
    if (!obsPublisher_.Initialize(obsError)) latestStatus_ = obsError;
    std::string virtualCameraError;
    if (!virtualCameraPublisher_.Initialize(virtualCameraError) && latestStatus_.empty()) latestStatus_ = virtualCameraError;
    UpdateFonts();

    preview_ = CreateWindowExW(0, kPreviewClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 10, 10,
                               window_, nullptr, instance_, this);
    title_ = AddLabel(L"STRATOCAM", 0, titleFont_);
    subtitle_ = AddLabel(L"Low-latency USB camera control", 0, subtitleFont_);
    connectionStatus_ = AddLabel(L"●  CONNECTING", ID_CONNECTION, metricFont_);
    streamStatus_ = AddLabel(L"●  STOPPED", ID_STREAM, metricFont_);
    batteryStatus_ = AddLabel(L"Phone · Battery unknown", 0, metricFont_);
    modeCombo_ = AddControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, ID_MODE);
    for (const wchar_t* item : {L"1920×1080 · 30 FPS", L"3840×2160 · 30 FPS"})
        SendMessageW(modeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    SendMessageW(modeCombo_, CB_SETCURSEL, 0, 0);
    startButton_ = AddControl(L"BUTTON", L"Start", BS_OWNERDRAW, ID_START);
    stopButton_ = AddControl(L"BUTTON", L"Stop", BS_OWNERDRAW, ID_STOP);
    recordButton_ = AddControl(L"BUTTON", L"Record", BS_OWNERDRAW, ID_RECORD);
    recordStatus_ = AddLabel(L"REC  —", 0, metricFont_);
    status_ = AddLabel(L"Preparing camera connection…", ID_STATUS, subtitleFont_);

    audioTitle_ = AddLabel(L"AUDIO INPUT", 0, sectionFont_);
    audioInputCombo_ = AddControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED |
                                  CBS_HASSTRINGS | WS_VSCROLL, ID_AUDIO_INPUT);
    SendMessageW(audioInputCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"No audio"));
    std::string audioEnumerationError;
    audioDevices_ = WasapiAudioCapture::EnumerateDevices(audioEnumerationError);
    for (const auto& device : audioDevices_)
        SendMessageW(audioInputCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(device.name.c_str()));
    SendMessageW(audioInputCombo_, CB_SETCURSEL, 0, 0);
    audioLevel_ = AddLabel(L"LEVEL  —", 0, metricFont_);
    audioOffsetLabel_ = AddLabel(L"AUDIO SYNC OFFSET  ·  ms", 0, metricFont_);
    audioOffsetEdit_ = AddControl(L"EDIT", L"0", WS_BORDER | ES_CENTER | ES_AUTOHSCROLL, ID_AUDIO_OFFSET);
    SendMessageW(audioOffsetEdit_, EM_SETLIMITTEXT, 6, 0);
    if (!audioEnumerationError.empty()) latestStatus_ = audioEnumerationError;

    recordingFolderLabel_ = AddControl(L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_PATHELLIPSIS | SS_NOPREFIX);
    recordingFolderButton_ = AddControl(L"BUTTON", L"Browse…", BS_OWNERDRAW, ID_RECORDING_FOLDER);
    LoadRecordingFolderPreference();
    UpdateRecordingFolderLabel();

    orientationValue_ = AddLabel(L"VIEW  ·  AUTO", 0, metricFont_);
    orientationCombo_ = AddControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL, ID_ORIENTATION);
    for (const wchar_t* item : {L"Auto", L"Portrait", L"Landscape", L"Rotate 90°", L"Rotate 180°", L"Rotate 270°"})
        SendMessageW(orientationCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    SendMessageW(orientationCombo_, CB_SETCURSEL, 0, 0);
    scaleCombo_ = AddControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, ID_SCALE);
    for (const wchar_t* item : {L"Fit", L"Fill", L"Actual size · 1:1"})
        SendMessageW(scaleCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    SendMessageW(scaleCombo_, CB_SETCURSEL, 0, 0);

    exposureTitle_ = AddLabel(L"EXPOSURE", 0, sectionFont_);
    aeCheck_ = AddControl(L"BUTTON", L"AUTO", BS_OWNERDRAW, ID_AE);
    isoValue_ = AddLabel(L"ISO", 0, metricFont_);
    isoSlider_ = AddControl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, ID_ISO);
    shutterValue_ = AddLabel(L"SHUTTER", 0, metricFont_);
    shutterSlider_ = AddControl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, ID_SHUTTER);

    focusTitle_ = AddLabel(L"FOCUS", 0, sectionFont_);
    afCheck_ = AddControl(L"BUTTON", L"AUTO", BS_OWNERDRAW, ID_AF);
    focusValue_ = AddLabel(L"DISTANCE", 0, metricFont_);
    focusSlider_ = AddControl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, ID_FOCUS);
    resumeFocusButton_ = AddControl(L"BUTTON", L"Resume continuous AF", BS_OWNERDRAW, ID_RESUME_FOCUS);

    wbTitle_ = AddLabel(L"WHITE BALANCE", 0, sectionFont_);
    awbCheck_ = AddControl(L"BUTTON", L"AUTO", BS_OWNERDRAW, ID_AWB);
    redValue_ = AddLabel(L"RED GAIN", 0, metricFont_);
    redSlider_ = AddControl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, ID_RED);
    blueValue_ = AddLabel(L"BLUE GAIN", 0, metricFont_);
    blueSlider_ = AddControl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, ID_BLUE);

    zoomTitle_ = AddLabel(L"ZOOM", 0, sectionFont_);
    zoomValue_ = AddLabel(L"1.00×", 0, metricFont_);
    zoomSlider_ = AddControl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, ID_ZOOM);
    AddControl(L"BUTTON", L"1×", BS_OWNERDRAW, ID_ZOOM_1);
    AddControl(L"BUTTON", L"1.5×", BS_OWNERDRAW, ID_ZOOM_15);
    AddControl(L"BUTTON", L"2×", BS_OWNERDRAW, ID_ZOOM_2);
    AddControl(L"BUTTON", L"Reset camera", BS_OWNERDRAW, ID_RESET);

    metricResolution_ = AddLabel(L"RESOLUTION   —", 0, metricFont_);
    metricFps_ = AddLabel(L"FPS / RATE   —", 0, metricFont_);
    metricBitrate_ = AddLabel(L"LIVE   Unknown", 0, metricFont_);
    metricLatency_ = AddLabel(L"DECODE / PROC   —", 0, metricFont_);
    metricDrops_ = AddLabel(L"TRANSPORT / DROPS   —", 0, metricFont_);

    controlsPanel_ = CreateWindowExW(0, kPanelClass, L"", WS_CHILD | WS_VISIBLE |
        WS_VSCROLL | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 10, 10,
        window_, nullptr, instance_, this);
    for (HWND control : {audioTitle_, audioInputCombo_, audioLevel_, audioOffsetLabel_, audioOffsetEdit_,
            recordingFolderLabel_, recordingFolderButton_, exposureTitle_, aeCheck_, isoValue_, isoSlider_,
            shutterValue_, shutterSlider_, focusTitle_, afCheck_, focusValue_, focusSlider_, resumeFocusButton_,
            wbTitle_, awbCheck_, redValue_, redSlider_, blueValue_, blueSlider_, zoomTitle_, zoomValue_, zoomSlider_,
            GetDlgItem(window_, ID_ZOOM_1), GetDlgItem(window_, ID_ZOOM_15), GetDlgItem(window_, ID_ZOOM_2),
            GetDlgItem(window_, ID_RESET)}) {
        SetParent(control, controlsPanel_);
        SetWindowSubclass(control, [](HWND child, UINT message, WPARAM wParam, LPARAM lParam,
                                     UINT_PTR, DWORD_PTR) -> LRESULT {
            if (message == WM_MOUSEWHEEL)
                return SendMessageW(GetParent(child), message, wParam, lParam);
            return DefSubclassProc(child, message, wParam, lParam);
        }, 1, 0);
    }
    UpdateFonts();

    for (HWND slider : {isoSlider_, shutterSlider_, focusSlider_, redSlider_, blueSlider_, zoomSlider_})
        SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
    for (HWND toggle : {aeCheck_, afCheck_, awbCheck_}) SendMessageW(toggle, BM_SETCHECK, BST_CHECKED, 0);

    renderer_.Initialize(preview_);
    renderer_.SetViewTransform(90, PreviewScaleMode::Fit);
    previewPlaceholder_ = CreateWindowExW(0, L"STATIC", L"Please connect an Android device via USB.",
        WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE, 0, 0, 10, 10,
        window_, nullptr, instance_, nullptr);
    SendMessageW(previewPlaceholder_, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), FALSE);
    PipelineDiagnostics::Reset(adb_.DiagnosticsDirectory() / "windows-pipeline-diagnostics.log");
    LoadSessionPreferences();
    SetTimer(window_, 1, 500, nullptr);
    orientationThread_ = std::thread(&AppWindow::PollOrientation, this);
    focusThread_ = std::thread(&AppWindow::PollFocusStatus, this);
    deviceStatusThread_ = std::thread(&AppWindow::PollDeviceStatus, this);
    displayPowerThread_ = std::thread(&AppWindow::PollDisplayPower, this);
    BeginConnect();
    return true;
}

void AppWindow::Layout(int width, int height) {
    constexpr int margin = 20;
    width = MulDiv(width, 96, dpi_);
    height = MulDiv(height, 96, dpi_);
    StreamInfo info;
    { std::lock_guard lock(stateMutex_); info = streamInfo_; }
    const auto previewLayout = CalculateWindowPreviewLayout(width, height,
        info.width > 0 ? info.width : 1920, info.height > 0 ? info.height : 1080,
        effectiveRotationDegrees_.load());
    const int headerHeight = previewLayout.headerHeight;
    const int panelWidth = previewLayout.panelWidth;
    const int panelX = previewLayout.panelX;
    const int panelTop = headerHeight + 12;
    const int panelHeight = std::max(1, height - panelTop - margin);
    // Content is laid out in logical pixels; only the panel scrolls on short displays.
    constexpr int panelContentHeight = 845;
    panelScrollMax_ = std::max(0, panelContentHeight - panelHeight);
    panelScroll_ = std::clamp(panelScroll_, 0, panelScrollMax_);
    SCROLLINFO scroll{};
    scroll.cbSize = sizeof(scroll); scroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    scroll.nMin = 0; scroll.nMax = panelContentHeight - 1;
    scroll.nPage = panelHeight; scroll.nPos = panelScroll_;
    SetScrollInfo(controlsPanel_, SB_VERT, &scroll, TRUE);
    renderer_.SetResizeInProgress(true);
    const bool wasVisible = IsWindowVisible(window_);
    if (wasVisible) SendMessageW(window_, WM_SETREDRAW, FALSE, 0);
    struct ChildPlacement { HWND control; int x; int y; int width; int height; };
    std::vector<ChildPlacement> placements;
    placements.reserve(40);
    const auto move = [&](HWND control, int x, int y, int controlWidth, int controlHeight) {
        if (GetParent(control) == controlsPanel_) {
            x -= panelX; y -= panelTop + panelScroll_;
        }
        placements.push_back({control, Px(x), Px(y), Px(controlWidth), Px(controlHeight)});
    };
    move(controlsPanel_, panelX, panelTop, panelWidth, panelHeight);

    move(title_, margin, 12, 260, 30);
    move(subtitle_, margin, 43, width < 840 ? 0 : 300, 20);
    const bool compact = headerHeight > 72;
    move(modeCombo_, compact ? margin : 300, compact ? 70 : 17, 205, 200);
    move(batteryStatus_, width < 840 ? margin : (compact ? width - 520 : 300), compact ? 43 : 48,
         width < 840 ? width - 40 : (compact ? 470 : std::max(180, width - 1020)), 20);
    move(connectionStatus_, width - (compact ? 340 : 720), compact ? 15 : 23, 145, 26);
    move(streamStatus_, width - (compact ? 176 : 568), compact ? 15 : 23, 126, 26);
    move(recordButton_, width - (width < 800 ? 310 : 446), compact ? 68 : 17, width < 800 ? 90 : 124, 36);
    move(startButton_, width - (width < 800 ? 210 : 312), compact ? 68 : 17, width < 800 ? 90 : 126, 36);
    move(stopButton_, width - (width < 800 ? 110 : 176), compact ? 68 : 17, width < 800 ? 90 : 126, 36);
    move(recordStatus_, compact ? 250 : width - 720, compact ? 106 : 49,
         compact ? width - 300 : 670, compact && headerHeight < 128 ? 0 : 20);
    PipelineDiagnostics::Log("resize", "layout client=" + std::to_string(width) + "x" +
        std::to_string(height) + " preview=" + std::to_string(previewLayout.previewX) + "," +
        std::to_string(previewLayout.previewY) + "," + std::to_string(previewLayout.previewWidth) +
        "x" + std::to_string(previewLayout.previewHeight) + " panelX=" +
        std::to_string(previewLayout.panelX));
    const int leftWidth = previewLayout.leftWidth;
    move(preview_, previewLayout.previewX, previewLayout.previewY,
         previewLayout.previewWidth, previewLayout.previewHeight);
    if (previewPlaceholder_) move(previewPlaceholder_, previewLayout.previewX, previewLayout.previewY,
         previewLayout.previewWidth, previewLayout.previewHeight);
    const int toolbarY = previewLayout.toolbarY;
    const bool narrowPreview = leftWidth < 720;
    move(orientationValue_, margin + 10, toolbarY + 12, narrowPreview ? 64 : 190, 24);
    move(orientationCombo_, margin + (narrowPreview ? 80 : 200), toolbarY + 7,
         narrowPreview ? 150 : 170, 220);
    move(scaleCombo_, margin + (narrowPreview ? 240 : 380), toolbarY + 7,
         narrowPreview ? std::min(160, leftWidth - 250) : 170, 150);
    move(status_, margin + (narrowPreview ? 10 : 565), toolbarY + (narrowPreview ? 47 : 12),
         narrowPreview ? leftWidth - 20 : leftWidth - 575,
         narrowPreview && previewLayout.toolbarHeight < 76 ? 0 : 22);

    const int metricsY = previewLayout.metricsY;
    const int columns = narrowPreview ? 3 : 5;
    const int metricWidth = (leftWidth - 20) / columns;
    int metricIndex = 0;
    for (HWND metric : {metricResolution_, metricFps_, metricBitrate_, metricLatency_, metricDrops_}) {
        move(metric, margin + 10 + (metricIndex % columns) * metricWidth,
             metricsY + 17 + (metricIndex / columns) * 34, metricWidth - 8, 26);
        ++metricIndex;
    }

    const int x = panelX + 22;
    const int content = panelWidth - 44 - MulDiv(GetSystemMetricsForDpi(SM_CXVSCROLL, dpi_), 96, dpi_);
    int y = headerHeight + 30;
    auto section = [&](HWND title, HWND toggle) {
        move(title, x, y, content - 94, 25);
        move(toggle, x + content - 88, y - 4, 88, 29);
        y += 37;
    };
    auto row = [&](HWND label, HWND slider) {
        move(label, x, y, content, 22); y += 21;
        move(slider, x, y, content, 26); y += 37;
    };
    move(audioTitle_, x, y, content, 25); y += 28;
    move(audioInputCombo_, x, y, content, 240); y += 38;
    move(audioLevel_, x, y + 3, content, 22); y += 27;
    move(audioOffsetLabel_, x, y + 3, content - 56, 22);
    move(audioOffsetEdit_, x + content - 48, y, 48, 26); y += 38;
    move(recordingFolderLabel_, x, y + 5, content - 94, 22);
    move(recordingFolderButton_, x + content - 86, y, 86, 29); y += 37;
    y += 8;
    section(exposureTitle_, aeCheck_); row(isoValue_, isoSlider_); row(shutterValue_, shutterSlider_);
    y += 8; section(focusTitle_, afCheck_); row(focusValue_, focusSlider_);
    move(resumeFocusButton_, x, y - 3, content, 29); y += 36;
    y += 10; section(wbTitle_, awbCheck_); row(redValue_, redSlider_); row(blueValue_, blueSlider_);
    y += 10; move(zoomTitle_, x, y, content / 2, 25);
    move(zoomValue_, x + content / 2, y, content / 2, 25); y += 29;
    move(zoomSlider_, x, y, content, 26); y += 36;
    const int quickWidth = (content - 16) / 3;
    move(GetDlgItem(controlsPanel_, ID_ZOOM_1), x, y, quickWidth, 32);
    move(GetDlgItem(controlsPanel_, ID_ZOOM_15), x + quickWidth + 8, y, quickWidth, 32);
    move(GetDlgItem(controlsPanel_, ID_ZOOM_2), x + (quickWidth + 8) * 2, y, quickWidth, 32); y += 46;
    move(GetDlgItem(controlsPanel_, ID_RESET), x, y, content, 38);

    const auto positionGroup = [&](HWND parent) {
        HDWP deferred = BeginDeferWindowPos(static_cast<int>(placements.size()));
        if (deferred) {
            for (const auto& placement : placements) {
                if (GetParent(placement.control) != parent) continue;
                deferred = DeferWindowPos(deferred, placement.control, nullptr,
                                          placement.x, placement.y, placement.width, placement.height,
                                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOREDRAW);
                if (!deferred) break;
            }
        }
        bool positioned = deferred && EndDeferWindowPos(deferred);
        if (!positioned) {
            PipelineDiagnostics::Log("resize", "deferred layout failed; applying no-redraw fallback");
            for (const auto& placement : placements) {
                if (GetParent(placement.control) != parent) continue;
                SetWindowPos(placement.control, nullptr, placement.x, placement.y,
                             placement.width, placement.height,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOREDRAW);
            }
        }
    };
    positionGroup(window_);
    positionGroup(controlsPanel_);

    RECT previewClient{};
    GetClientRect(preview_, &previewClient);
    PipelineDiagnostics::Log("resize", "preview HWND final client after deferred layout=" +
        std::to_string(previewClient.right) + "x" + std::to_string(previewClient.bottom));
    renderer_.Resize(previewClient.right, previewClient.bottom, false);
    renderer_.SetDestinationRect(0, 0, previewClient.right, previewClient.bottom);
    if (wasVisible) SendMessageW(window_, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(window_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_ALLCHILDREN);
    PipelineDiagnostics::Log("resize", "deferred parent/child redraw completed after layout");
    renderer_.SetResizeInProgress(false);
}

void AppWindow::PaintBackground(HDC dc) {
    RECT client{};
    GetClientRect(window_, &client);
    PipelineDiagnostics::Log("resize", "WM_PAINT background client=" + std::to_string(client.right) + "x" +
        std::to_string(client.bottom));
    FillRect(dc, &client, backgroundBrush_);
    HPEN divider = CreatePen(PS_SOLID, 1, kBorder);
    const auto oldPen = SelectObject(dc, divider);
    StreamInfo info;
    { std::lock_guard lock(stateMutex_); info = streamInfo_; }
    const auto layout = CalculateWindowPreviewLayout(MulDiv(client.right, 96, dpi_),
        MulDiv(client.bottom, 96, dpi_), info.width, info.height, effectiveRotationDegrees_.load());
    MoveToEx(dc, Px(20), Px(layout.headerHeight - 1), nullptr);
    LineTo(dc, client.right - Px(20), Px(layout.headerHeight - 1));
    SelectObject(dc, oldPen); DeleteObject(divider);

    const int margin = Px(20);
    RECT panel{Px(layout.panelX), Px(layout.headerHeight + 12), client.right - margin, client.bottom - margin};
    FillRounded(dc, panel, Px(18), kPanel, kBorder);

    if (preview_) {
        RECT preview{}; GetWindowRect(preview_, &preview);
        MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&preview), 2);
        InflateRect(&preview, 1, 1);
        HBRUSH frame = CreateSolidBrush(kBorder);
        FrameRect(dc, &preview, frame);
        DeleteObject(frame);
        RECT toolbar{margin, Px(layout.toolbarY), panel.left - margin, Px(layout.toolbarY + layout.toolbarHeight)};
        FillRounded(dc, toolbar, Px(12), kPanel, kBorder);
        RECT metrics{margin, Px(layout.metricsY), panel.left - margin, Px(layout.metricsY + layout.metricsHeight)};
        FillRounded(dc, metrics, Px(12), kPanel, kBorder);
    }
}

bool AppWindow::DrawButton(const DRAWITEMSTRUCT& item) {
    if (item.CtlType == ODT_COMBOBOX) {
        RECT rect = item.rcItem;
        const bool selected = (item.itemState & ODS_SELECTED) != 0;
        FillRect(item.hDC, &rect, selected ? reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)) : panelBrush_);
        if (selected) FillRounded(item.hDC, rect, 5, kAccentDark);
        int index = static_cast<int>(item.itemID);
        if (index < 0) index = static_cast<int>(SendMessageW(item.hwndItem, CB_GETCURSEL, 0, 0));
        wchar_t label[128]{};
        if (index >= 0) SendMessageW(item.hwndItem, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(label));
        rect.left += 10;
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, selected ? kText : RGB(205, 214, 226));
        const auto oldFont = SelectObject(item.hDC, bodyFont_);
        DrawTextW(item.hDC, label, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(item.hDC, oldFont);
        return true;
    }
    if (item.CtlType != ODT_BUTTON) return false;
    COLORREF fill = kSurface;
    COLORREF border = kBorder;
    COLORREF text = kText;
    if (item.CtlID == ID_START) { fill = RGB(34, 105, 75); border = kGreen; }
    else if (item.CtlID == ID_STOP) { fill = RGB(104, 42, 43); border = kRed; }
    else if (item.CtlID == ID_RECORD) {
        const bool recording = recorder_.Snapshot().Active();
        fill = recording ? RGB(124, 34, 42) : RGB(61, 34, 39);
        border = recording ? kRed : RGB(142, 62, 69);
        text = recording ? RGB(255, 235, 235) : kText;
    }
    else if (item.CtlID == ID_AE || item.CtlID == ID_AF || item.CtlID == ID_AWB) {
        bool checked = true;
        {
            std::lock_guard lock(stateMutex_);
            if (item.CtlID == ID_AE) checked = state_.autoExposure;
            else if (item.CtlID == ID_AF) checked = state_.autoFocus;
            else checked = state_.autoWhiteBalance;
        }
        fill = checked ? kAccentDark : kSurface;
        border = checked ? kAccent : kBorder;
        text = checked ? RGB(220, 235, 255) : kMuted;
    }
    if (item.itemState & ODS_SELECTED) fill = RGB(GetRValue(fill) * 3 / 4, GetGValue(fill) * 3 / 4, GetBValue(fill) * 3 / 4);
    RECT rect = item.rcItem;
    FillRounded(item.hDC, rect, 9, fill, border);
    wchar_t label[128]{};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, text);
    const auto oldFont = SelectObject(item.hDC, bodyFont_);
    DrawTextW(item.hDC, label, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(item.hDC, oldFont);
    return true;
}

LRESULT AppWindow::DrawControl(NMCUSTOMDRAW* customDraw) {
    if (!customDraw || !IsSlider(customDraw->hdr.hwndFrom)) return CDRF_DODEFAULT;
    if (customDraw->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (customDraw->dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    const bool enabled = IsWindowEnabled(customDraw->hdr.hwndFrom);
    if (customDraw->dwItemSpec == TBCD_CHANNEL) {
        RECT bounds{}; GetClientRect(customDraw->hdr.hwndFrom, &bounds);
        RECT channel{bounds.left + 7, bounds.top + bounds.bottom / 2 - 3, bounds.right - 7, bounds.top + bounds.bottom / 2 + 3};
        FillRounded(customDraw->hdc, channel, 6, enabled ? RGB(47, 58, 72) : RGB(34, 40, 49));
        const int position = static_cast<int>(SendMessageW(customDraw->hdr.hwndFrom, TBM_GETPOS, 0, 0));
        RECT active = channel;
        active.right = active.left + static_cast<int>((active.right - active.left) * position / 1000.0);
        if (active.right > active.left) FillRounded(customDraw->hdc, active, 6, enabled ? kAccent : RGB(71, 81, 94));
        return CDRF_SKIPDEFAULT;
    }
    if (customDraw->dwItemSpec == TBCD_THUMB) {
        RECT thumb = customDraw->rc;
        InflateRect(&thumb, -1, -1);
        HBRUSH brush = CreateSolidBrush(enabled ? RGB(226, 235, 247) : RGB(99, 108, 120));
        HPEN pen = CreatePen(PS_SOLID, 2, enabled ? kAccent : kBorder);
        const auto oldBrush = SelectObject(customDraw->hdc, brush);
        const auto oldPen = SelectObject(customDraw->hdc, pen);
        Ellipse(customDraw->hdc, thumb.left, thumb.top, thumb.right, thumb.bottom);
        SelectObject(customDraw->hdc, oldPen); SelectObject(customDraw->hdc, oldBrush);
        DeleteObject(pen); DeleteObject(brush);
        return CDRF_SKIPDEFAULT;
    }
    return CDRF_SKIPDEFAULT;
}

void AppWindow::PostStatus(const std::string& status) {
    if (!window_ || closing_.load()) return;
    PostMessageW(window_, WM_APP_STATUS, 0, reinterpret_cast<LPARAM>(new std::string(status)));
}

void AppWindow::BeginConnect() {
    connectionRequested_.store(true);
    if (transport_.Running()) { PostStatus("Video stream is already running"); return; }
    if (connecting_.exchange(true)) return;
    discoveryRetryAtMs_.store(0);
    if (connectionThread_.joinable()) connectionThread_.join();
    connectionThread_ = std::thread([this] {
        PipelineDiagnostics::Log("lifecycle", "BeginConnect");
        PipelineDiagnostics::Log("renderer", "D3D11 adapter=" + renderer_.AdapterName());
        std::string status;
        if (!adb_.Prepare(status)) {
            PipelineDiagnostics::Log("lifecycle", "ADB prepare failed: " + status);
            discoveryRetryAtMs_.store(GetTickCount64() + 3000);
            PipelineDiagnostics::Log("adb", "Discovery retry scheduled in 3000 ms");
            connected_.store(false); PostStatus(status); connecting_.store(false); return;
        }
        if (closing_.load() || !connectionRequested_.load()) { connecting_.store(false); return; }
        PipelineDiagnostics::Log("lifecycle", "ADB prepared; control/video forwards recreated");
        PostStatus(status);
        const int mode = modeSelection_.load();
        const int width = mode == 1 ? 3840 : 1920;
        const int height = mode == 1 ? 2160 : 1080;
        const int bitrate = mode == 1 ? 25'000'000 : 8'000'000;
        std::string error;
        PipelineDiagnostics::Log("lifecycle", "Requesting Android video mode " +
            std::to_string(width) + "x" + std::to_string(height));
        bool modeReady = false;
        for (int attempt = 0; attempt < 20 && !closing_.load(); ++attempt) {
            if (controls_.SetVideoMode(width, height, 30, bitrate, error)) {
                modeReady = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (!modeReady) {
            PipelineDiagnostics::Log("lifecycle", "SetVideoMode failed: " + error);
            PostStatus(error); connecting_.store(false); return;
        }
        PipelineDiagnostics::Log("lifecycle", "Android video mode accepted; waiting for capabilities");
        CameraCapabilities capabilities;
        CameraState state;
        bool ready = false;
        for (int attempt = 0; attempt < 20 && !closing_.load(); ++attempt) {
            if (controls_.GetCapabilities(capabilities, state, error)) { ready = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (!ready) {
            PipelineDiagnostics::Log("lifecycle", "Capabilities failed: " + error);
            PostStatus(error); connecting_.store(false); return;
        }
        if (closing_.load() || !connectionRequested_.load()) { connecting_.store(false); return; }
        std::optional<CameraState> previous;
        { std::lock_guard lock(stateMutex_); if (cameraStateAvailable_) previous = state_; }
        if (previous) {
            const auto requested = RestoreSupportedCameraState(*previous, capabilities);
            bool restored = true;
            restored = controls_.SetExposure(requested.autoExposure, requested.iso, requested.exposureNs, state, error) && restored;
            restored = controls_.SetFocus(requested.autoFocus, requested.focusDistance, state, error) && restored;
            restored = controls_.SetWhiteBalance(requested.autoWhiteBalance, requested.redGain, requested.blueGain, state, error) && restored;
            restored = controls_.SetZoom(requested.zoom, state, error) && restored;
            PipelineDiagnostics::Log("preferences", restored ? "Camera settings restored" : "Camera settings partially restored: " + error);
            if (!restored) PostStatus("Some saved camera settings could not be restored: " + error);
        }
        connected_.store(true);
        PipelineDiagnostics::Log("lifecycle", "Capabilities ready; starting decoder");
        {
            std::lock_guard lock(stateMutex_);
            capabilities_ = capabilities;
            state_ = state;
            cameraStateAvailable_ = true;
        }
        PostMessageW(window_, WM_APP_CONNECTED, 0, 0);
        latency_.Reset();
        renderedFrames_.store(0);
        renderer_.ResetStatistics();
        recorder_.ResetCodecConfig();
        if (decoder_.Start(width, height, 30, renderer_.Device())) {
            PipelineDiagnostics::Log("lifecycle", "Decoder ready; starting video transport");
            transport_.Start();
        } else {
            PipelineDiagnostics::Log("lifecycle", "Decoder initialization failed");
        }
        connecting_.store(false);
    });
}

void AppWindow::StopStream() {
    connectionRequested_.store(false);
    discoveryRetryAtMs_.store(0);
    PipelineDiagnostics::Log("lifecycle", "StopStream");
    recorder_.Stop();
    transport_.Stop();
    decoder_.Stop();
    obsPublisher_.StopStream();
    virtualCameraPublisher_.StopStream();
    ExportLatency();
    PostStatus("Video stopped · native, OBS and virtual camera metrics saved under captures/");
}

void AppWindow::ToggleRecording() {
    const auto current = recorder_.Snapshot();
    if (current.Active()) {
        recorder_.Stop();
        const auto stopped = recorder_.Snapshot();
        PostStatus(stopped.packetsWritten
            ? "Recording saved · " + WideToUtf8(stopped.path.wstring())
            : "Recording stopped before the first SPS/PPS + IDR");
        UpdateStatusPanel();
        return;
    }
    if (transport_.State() != VideoTransportState::Streaming) {
        PostStatus("Start the H.264 stream before recording");
        return;
    }
    std::string folderError;
    if (!ValidateRecordingFolder(folderError)) {
        PostStatus(folderError);
        MessageBoxW(window_, Utf8ToWide(folderError).c_str(), L"Recording folder", MB_OK | MB_ICONERROR);
        return;
    }
    StreamInfo info;
    { std::lock_guard lock(stateMutex_); info = streamInfo_; }
    std::optional<AudioCaptureFormat> audioFormat;
    const auto audio = audioCapture_.Snapshot();
    if (audioSelection_.load() > 0) {
        if (!audio.running) {
            PostStatus("Selected audio input is not running");
            return;
        }
        audioFormat = audio.format;
    }
    wchar_t offsetText[32]{};
    GetWindowTextW(audioOffsetEdit_, offsetText, static_cast<int>(std::size(offsetText)));
    wchar_t* offsetEnd = nullptr;
    const long parsedOffset = std::wcstol(offsetText, &offsetEnd, 10);
    if (offsetEnd == offsetText || *offsetEnd != L'\0' || parsedOffset < -5000 || parsedOffset > 5000) {
        PostStatus("Audio Sync Offset must be between -5000 and 5000 ms");
        return;
    }
    SYSTEMTIME local{};
    GetLocalTime(&local);
    wchar_t stem[128]{};
    swprintf_s(stem, L"stratocam-%04u%02u%02u-%02u%02u%02u-%dx%d",
               local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond,
               info.width, info.height);
    const auto captures = adb_.DiagnosticsDirectory();
    const auto path = recordingFolder_ / (std::wstring(stem) + L".mp4");
    const auto metrics = captures / (std::wstring(L"direct-recording-") + stem + L".csv");
    std::string error;
    if (!recorder_.Start(path, metrics, info, audioFormat, static_cast<int>(parsedOffset),
                         effectiveRotationDegrees_.load(), error)) PostStatus(error);
    else PostStatus("REC armed · " + WideToUtf8(path.wstring()));
    UpdateStatusPanel();
}

void AppWindow::LoadRecordingFolderPreference() {
    DWORD bytes = 0;
    const LSTATUS sizeResult = RegGetValueW(HKEY_CURRENT_USER, kPreferenceKey, kRecordingFolderValue,
                                            RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
    if (sizeResult == ERROR_SUCCESS && bytes >= sizeof(wchar_t)) {
        std::wstring value(bytes / sizeof(wchar_t), L'\0');
        if (RegGetValueW(HKEY_CURRENT_USER, kPreferenceKey, kRecordingFolderValue,
                         RRF_RT_REG_SZ, nullptr, value.data(), &bytes) == ERROR_SUCCESS) {
            value.resize(std::wcslen(value.c_str()));
            if (!value.empty()) recordingFolder_ = value;
        }
    }
    if (recordingFolder_.empty()) {
        recordingFolder_ = adb_.DefaultRecordingDirectory();
        std::error_code ignored;
        std::filesystem::create_directories(recordingFolder_, ignored);
    }
}

void AppWindow::LoadSessionPreferences() {
    const auto saved = LoadSessionSettings();
    if (!saved) return;
    sessionSettings_ = *saved;
    modeSelection_.store(saved->videoMode);
    orientationSelection_.store(saved->orientation);
    scaleSelection_.store(saved->scale);
    SendMessageW(modeCombo_, CB_SETCURSEL, saved->videoMode, 0);
    SendMessageW(orientationCombo_, CB_SETCURSEL, saved->orientation, 0);
    SendMessageW(scaleCombo_, CB_SETCURSEL, saved->scale, 0);
    SetWindowTextW(audioOffsetEdit_, std::to_wstring(saved->audioOffsetMs).c_str());
    { std::lock_guard lock(stateMutex_); state_ = saved->camera; cameraStateAvailable_ = saved->hasCameraState; }
    if (!saved->audioDeviceId.empty()) {
        const auto device = std::find_if(audioDevices_.begin(), audioDevices_.end(), [&](const auto& candidate) {
            return WideToUtf8(candidate.id) == saved->audioDeviceId;
        });
        if (device != audioDevices_.end()) {
            const int selection = static_cast<int>(std::distance(audioDevices_.begin(), device)) + 1;
            SendMessageW(audioInputCombo_, CB_SETCURSEL, selection, 0);
            SelectAudioInput(selection);
        } else PostStatus("Saved audio input is unavailable; No audio selected");
    }
    PipelineDiagnostics::Log("preferences", "Session settings loaded");
}

void AppWindow::SaveSessionPreferences() {
    sessionSettings_.videoMode = modeSelection_.load();
    sessionSettings_.orientation = orientationSelection_.load();
    sessionSettings_.scale = scaleSelection_.load();
    wchar_t text[32]{};
    GetWindowTextW(audioOffsetEdit_, text, static_cast<int>(std::size(text)));
    wchar_t* end = nullptr;
    const long offset = std::wcstol(text, &end, 10);
    if (end != text && *end == L'\0' && offset >= -5000 && offset <= 5000)
        sessionSettings_.audioOffsetMs = static_cast<int>(offset);
    { std::lock_guard lock(stateMutex_);
      sessionSettings_.camera = state_; sessionSettings_.hasCameraState = cameraStateAvailable_; }
    PipelineDiagnostics::Log("preferences", SaveSessionSettings(sessionSettings_)
        ? "Session settings saved" : "Could not save session settings");
    std::string error;
    if (!SaveRecordingFolderPreference(error)) PipelineDiagnostics::Log("preferences", error);
}

bool AppWindow::SaveRecordingFolderPreference(std::string& error) const {
    HKEY key = nullptr;
    LSTATUS result = RegCreateKeyExW(HKEY_CURRENT_USER, kPreferenceKey, 0, nullptr, 0, KEY_SET_VALUE,
                                     nullptr, &key, nullptr);
    if (result == ERROR_SUCCESS) {
        const auto value = recordingFolder_.wstring();
        result = RegSetValueExW(key, kRecordingFolderValue, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(value.c_str()),
                                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
    if (result != ERROR_SUCCESS) {
        error = "Could not save the recording folder preference (Windows error " +
                std::to_string(result) + ")";
        return false;
    }
    return true;
}

bool AppWindow::ValidateRecordingFolder(std::string& error) const {
    std::error_code filesystemError;
    if (!std::filesystem::exists(recordingFolder_, filesystemError) || filesystemError) {
        error = "Recording folder does not exist: " + WideToUtf8(recordingFolder_.wstring());
        return false;
    }
    if (!std::filesystem::is_directory(recordingFolder_, filesystemError) || filesystemError) {
        error = "Recording destination is not a folder: " + WideToUtf8(recordingFolder_.wstring());
        return false;
    }
    const auto probe = recordingFolder_ /
        (L".stratocam-write-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()) + L".tmp");
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "Recording folder is not writable: " + WideToUtf8(recordingFolder_.wstring()) +
                " (Windows error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(file);
    return true;
}

void AppWindow::UpdateRecordingFolderLabel() {
    const std::wstring label = L"FOLDER  ·  " + recordingFolder_.wstring();
    SetWindowTextW(recordingFolderLabel_, label.c_str());
}

void AppWindow::ChooseRecordingFolder() {
    if (recorder_.Snapshot().Active()) {
        PostStatus("Stop recording before changing the recording folder");
        return;
    }
    IFileDialog* dialog = nullptr;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&dialog));
    if (FAILED(result)) {
        PostStatus("Could not open the Windows folder picker");
        return;
    }
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options)))
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Choose recording folder");
    IShellItem* initial = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(recordingFolder_.c_str(), nullptr, IID_PPV_ARGS(&initial)))) {
        dialog->SetFolder(initial);
        initial->Release();
    }
    result = dialog->Show(window_);
    if (SUCCEEDED(result)) {
        IShellItem* selected = nullptr;
        PWSTR path = nullptr;
        if (SUCCEEDED(dialog->GetResult(&selected)) &&
            SUCCEEDED(selected->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            const auto previous = recordingFolder_;
            recordingFolder_ = path;
            std::string error;
            if (!ValidateRecordingFolder(error) || !SaveRecordingFolderPreference(error)) {
                recordingFolder_ = previous;
                PostStatus(error);
                MessageBoxW(window_, Utf8ToWide(error).c_str(), L"Recording folder", MB_OK | MB_ICONERROR);
            } else {
                UpdateRecordingFolderLabel();
                PostStatus("Recording folder · " + WideToUtf8(recordingFolder_.wstring()));
            }
            CoTaskMemFree(path);
        }
        if (selected) selected->Release();
    }
    dialog->Release();
}

void AppWindow::SelectAudioInput(int selection) {
    if (recorder_.Snapshot().Active()) {
        SendMessageW(audioInputCombo_, CB_SETCURSEL, audioSelection_.load(), 0);
        PostStatus("Stop recording before changing the audio input");
        return;
    }
    audioCapture_.Stop();
    if (selection <= 0) {
        audioSelection_.store(0);
        PostStatus("Recording audio disabled");
        UpdateStatusPanel();
        return;
    }
    const size_t deviceIndex = static_cast<size_t>(selection - 1);
    if (deviceIndex >= audioDevices_.size()) {
        SendMessageW(audioInputCombo_, CB_SETCURSEL, 0, 0);
        audioSelection_.store(0);
        return;
    }
    std::string error;
    if (!audioCapture_.Start(audioDevices_[deviceIndex], error)) {
        SendMessageW(audioInputCombo_, CB_SETCURSEL, 0, 0);
        audioSelection_.store(0);
        PostStatus(error);
    } else {
        audioSelection_.store(selection);
        const auto snapshot = audioCapture_.Snapshot();
        PostStatus("Audio input ready · " + WideToUtf8(snapshot.format.deviceName) + " · " +
                   std::to_string(snapshot.format.sampleRate) + " Hz · " +
                   std::to_string(snapshot.format.channels) + " ch");
    }
    UpdateStatusPanel();
}

void AppWindow::ExportLatency() {
    StreamInfo info;
    { std::lock_guard lock(stateMutex_); info = streamInfo_; }
    const std::string resolution = info.width > 0 ? "-" + std::to_string(info.width) + "x" + std::to_string(info.height) : "";
    const std::string suffix = resolution + "-" + std::to_string(GetTickCount64());
    latency_.ExportCsv(adb_.DiagnosticsDirectory() / ("windows-gui-latency" + suffix + ".csv"));
    obsPublisher_.ExportCsv(adb_.DiagnosticsDirectory() / ("windows-obs-delivery" + suffix + ".csv"));
    virtualCameraPublisher_.ExportCsv(adb_.DiagnosticsDirectory() / ("windows-virtual-camera-delivery" + suffix + ".csv"));
}

void AppWindow::PollOrientation() {
    int failures = 0;
    while (!closing_.load()) {
        const auto rotation = connected_.load() ? adb_.GetDisplayRotationDegrees() : std::nullopt;
        if (rotation) {
            failures = 0;
            if (deviceRotationDegrees_.exchange(*rotation) != *rotation && window_)
                PostMessageW(window_, WM_APP_ORIENTATION, 0, 0);
        } else if (++failures >= 3 && connected_.exchange(false) && window_) {
            PostMessageW(window_, WM_APP_ORIENTATION, 0, 0);
        }
        for (int part = 0; part < 15 && !closing_.load(); ++part)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void AppWindow::PollDeviceStatus() {
    while (!closing_.load()) {
        DeviceStatus status;
        std::string error;
        if (connected_.load() && controls_.GetDeviceStatus(status, error)) {
            { std::lock_guard lock(stateMutex_); deviceStatus_ = status; }
            if (window_) PostMessageW(window_, WM_APP_DEVICE_STATUS, 0, 0);
        }
        for (int part = 0; part < 100 && !closing_.load(); ++part)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void AppWindow::PollDisplayPower() {
    bool applied = false;
    while (!closing_.load()) {
        const bool active = transport_.State() == VideoTransportState::Streaming && connected_.load();
        if (active != applied) {
            if (active) {
                std::string status;
                const bool poweredOff = adb_.BeginDisplayOff(status);
                PipelineDiagnostics::Log("display-power", status);
                if (!poweredOff) PostStatus(status);
            } else {
                adb_.EndDisplayOff();
                PipelineDiagnostics::Log("display-power", "Phone display restored");
            }
            applied = active;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    adb_.EndDisplayOff();
}

void AppWindow::UpdateViewTransform() {
    StreamInfo info;
    { std::lock_guard lock(stateMutex_); info = streamInfo_; }
    const int selection = orientationSelection_.load();
    const int rotation = ResolvePreviewRotation(static_cast<PreviewOrientationMode>(selection), info.sensorOrientation,
                                                deviceRotationDegrees_.load(), info.width, info.height);
    PreviewScaleMode scale = PreviewScaleMode::Fit;
    if (scaleSelection_.load() == 1) scale = PreviewScaleMode::Fill;
    else if (scaleSelection_.load() == 2) scale = PreviewScaleMode::Actual;
    renderer_.SetViewTransform(rotation, scale);
    const int previousRotation = effectiveRotationDegrees_.exchange(rotation);
    const wchar_t* mode = selection == 0 ? L"AUTO" : selection == 1 ? L"PORTRAIT" : selection == 2 ? L"LANDSCAPE" : L"ROTATE";
    const wchar_t* sizing = scale == PreviewScaleMode::Fit ? L"FIT" : scale == PreviewScaleMode::Fill ? L"FILL" : L"1:1";
    std::wstring label = L"VIEW  ·  " + std::wstring(mode) + L"  ·  " + std::to_wstring(rotation) + L"°  ·  " + sizing;
    SetWindowTextW(orientationValue_, label.c_str());
    if (previousRotation != rotation && window_) {
        RECT client{}; GetClientRect(window_, &client);
        Layout(client.right, client.bottom);
    }
}

void AppWindow::HandlePreviewClick(int x, int y) {
    CameraState state;
    CameraCapabilities capabilities;
    StreamInfo info;
    { std::lock_guard lock(stateMutex_); state = state_; capabilities = capabilities_; info = streamInfo_; }
    if (!state.autoFocus) { PostStatus("Click-to-focus requires Auto Focus"); return; }
    if (capabilities.maxAfRegions <= 0) { PostStatus("Camera reports no autofocus regions"); return; }
    RECT bounds{}; GetClientRect(preview_, &bounds);
    PreviewScaleMode scale = PreviewScaleMode::Fit;
    if (scaleSelection_.load() == 1) scale = PreviewScaleMode::Fill;
    else if (scaleSelection_.load() == 2) scale = PreviewScaleMode::Actual;
    const SensorActiveArray active{
        capabilities.activeArrayLeft, capabilities.activeArrayTop,
        capabilities.activeArrayRight, capabilities.activeArrayBottom
    };
    const auto mapping = MapPreviewPoint(
        x, y, bounds.right, bounds.bottom, info.width, info.height, effectiveRotationDegrees_.load(),
        scale, active, state.zoom, capabilities.zoomRatioSupported
    );
    if (!mapping.inside) return;
    renderer_.SetFocusIndicator(static_cast<float>(mapping.indicatorU), static_cast<float>(mapping.indicatorV), 1);
    focusVisualState_ = 1;
    focusIndicatorExpiresNs_ = 0;
    FocusPointResult result;
    std::string error;
    if (!controls_.FocusAt(mapping.sensorX, mapping.sensorY, result, error)) {
        renderer_.SetFocusIndicator(static_cast<float>(mapping.indicatorU), static_cast<float>(mapping.indicatorV), 3);
        focusVisualState_ = 3;
        focusIndicatorExpiresNs_ = QpcNanoseconds() + 2'000'000'000.0;
        PostStatus(error);
        return;
    }
    {
        std::lock_guard lock(focusMutex_);
        activeFocusRequestId_ = result.requestId;
    }
    PostStatus("Point AF · focusing at sensor " + std::to_string(mapping.sensorX) + "," + std::to_string(mapping.sensorY));
}

void AppWindow::PollFocusStatus() {
    while (!closing_.load()) {
        std::string requestId;
        { std::lock_guard lock(focusMutex_); requestId = activeFocusRequestId_; }
        if (!requestId.empty()) {
            FocusPointResult result;
            std::string error;
            if (controls_.GetFocusStatus(result, error) && result.requestId == requestId && result.status != "focusing") {
                {
                    std::lock_guard lock(focusMutex_);
                    if (activeFocusRequestId_ == requestId) activeFocusRequestId_.clear();
                }
                if (window_) PostMessageW(window_, WM_APP_FOCUS_STATUS, 0, reinterpret_cast<LPARAM>(new FocusPointResult(result)));
            }
        }
        for (int part = 0; part < 2 && !closing_.load(); ++part)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void AppWindow::ApplyFocusStatus(const FocusPointResult& result) {
    if (result.status == "focused") {
        focusVisualState_ = 2;
        renderer_.SetFocusIndicator(renderer_.FocusIndicatorU(), renderer_.FocusIndicatorV(), 2);
        focusIndicatorExpiresNs_ = QpcNanoseconds() + 2'000'000'000.0;
        PostStatus("Point AF · focus locked · use Resume continuous AF to track again");
    } else if (result.status == "failed" || result.status == "unsupported") {
        focusVisualState_ = 3;
        renderer_.SetFocusIndicator(renderer_.FocusIndicatorU(), renderer_.FocusIndicatorV(), 3);
        focusIndicatorExpiresNs_ = QpcNanoseconds() + 2'000'000'000.0;
        PostStatus(result.status == "failed" ? "Point AF · focus not acquired" : "Point AF is not supported");
    } else if (result.status == "idle") {
        focusVisualState_ = 0;
        renderer_.ClearFocusIndicator();
        focusIndicatorExpiresNs_ = 0;
    }
}

void AppWindow::ResumeContinuousFocus() {
    FocusPointResult result;
    std::string error;
    if (controls_.ResumeContinuousFocus(result, error)) {
        { std::lock_guard lock(focusMutex_); activeFocusRequestId_.clear(); }
        ApplyFocusStatus(result);
        PostStatus("Continuous-video AF resumed");
    } else PostStatus(error);
}

double AppWindow::LogValue(int position, double minimum, double maximum) {
    if (minimum <= 0 || maximum <= minimum) return minimum;
    return minimum * std::pow(maximum / minimum, position / 1000.0);
}

int AppWindow::LogPosition(double value, double minimum, double maximum) {
    if (value <= minimum || minimum <= 0 || maximum <= minimum) return 0;
    return std::clamp(static_cast<int>(std::log(value / minimum) / std::log(maximum / minimum) * 1000.0), 0, 1000);
}

void AppWindow::ApplyCapabilities() {
    CameraState state;
    CameraCapabilities capabilities;
    { std::lock_guard lock(stateMutex_); state = state_; capabilities = capabilities_; }
    SetWindowTextW(aeCheck_, state.autoExposure ? L"AUTO" : L"MANUAL");
    SetWindowTextW(afCheck_, state.autoFocus ? L"AUTO" : L"MANUAL");
    SetWindowTextW(awbCheck_, state.autoWhiteBalance ? L"AUTO" : L"MANUAL");
    SendMessageW(isoSlider_, TBM_SETPOS, TRUE, LogPosition(state.iso, capabilities.isoMin, capabilities.isoMax));
    const double shutterMax = std::min<double>(capabilities.exposureMaxNs, capabilities.maxFrameDurationNs);
    SendMessageW(shutterSlider_, TBM_SETPOS, TRUE, LogPosition(state.exposureNs, capabilities.exposureMinNs, shutterMax));
    SendMessageW(focusSlider_, TBM_SETPOS, TRUE, capabilities.focusMax > 0 ? static_cast<int>(state.focusDistance / capabilities.focusMax * 1000) : 0);
    SendMessageW(redSlider_, TBM_SETPOS, TRUE, static_cast<int>((state.redGain - capabilities.gainMin) /
        std::max(0.001, capabilities.gainMax - capabilities.gainMin) * 1000));
    SendMessageW(blueSlider_, TBM_SETPOS, TRUE, static_cast<int>((state.blueGain - capabilities.gainMin) /
        std::max(0.001, capabilities.gainMax - capabilities.gainMin) * 1000));
    SendMessageW(zoomSlider_, TBM_SETPOS, TRUE, static_cast<int>((state.zoom - capabilities.zoomMin) /
        std::max(0.001, capabilities.zoomMax - capabilities.zoomMin) * 1000));
    EnableWindow(isoSlider_, capabilities.manualExposure && !state.autoExposure);
    EnableWindow(shutterSlider_, capabilities.manualExposure && !state.autoExposure);
    EnableWindow(focusSlider_, capabilities.manualFocus && !state.autoFocus);
    EnableWindow(redSlider_, capabilities.manualWhiteBalance && !state.autoWhiteBalance);
    EnableWindow(blueSlider_, capabilities.manualWhiteBalance && !state.autoWhiteBalance);
    EnableWindow(resumeFocusButton_, state.autoFocus && capabilities.maxAfRegions > 0);
    for (HWND control : {aeCheck_, afCheck_, awbCheck_, isoSlider_, shutterSlider_, focusSlider_, redSlider_, blueSlider_})
        InvalidateRect(control, nullptr, TRUE);
}

std::wstring AppWindow::FormatMs(double value) {
    std::wostringstream output;
    output << std::fixed << std::setprecision(1) << value << L" ms";
    return output.str();
}

std::wstring AppWindow::FormatNumber(double value, int precision) {
    std::wostringstream output;
    output << std::fixed << std::setprecision(precision) << value;
    return output.str();
}

void AppWindow::UpdateStatusPanel() {
    CameraState state;
    StreamInfo info;
    std::string latest;
    DeviceStatus device;
    { std::lock_guard lock(stateMutex_); state = state_; info = streamInfo_; latest = latestStatus_; device = deviceStatus_; }
    const auto latency = latency_.Get();
    const auto obs = obsPublisher_.GetSnapshot();
    const auto virtualCamera = virtualCameraPublisher_.GetSnapshot();
    const auto recording = recorder_.Snapshot();
    const auto audio = audioCapture_.Snapshot();
    const auto transportState = transport_.State();
    const bool streaming = transportState == VideoTransportState::Streaming;
    const bool connected = connected_.load();
    if (previewPlaceholder_) {
        const bool hasPreview = connected && streaming && renderedFrames_.load() > 0;
        const int placeholderState = hasPreview ? 4 : !connected ? 0
            : transportState == VideoTransportState::Stopped ? 1
            : transportState == VideoTransportState::Error ? 2 : 3;
        if (previewPlaceholderState_ != placeholderState) {
            previewPlaceholderState_ = placeholderState;
            const wchar_t* message = placeholderState == 0 ? L"Please connect an Android device via USB."
                : placeholderState == 1 ? L"Video stopped. Select Start to begin."
                : placeholderState == 2 ? L"Video unavailable. Select Start to reconnect."
                : L"Starting camera preview…";
            SetWindowTextW(previewPlaceholder_, message);
            ShowWindow(previewPlaceholder_, hasPreview ? SW_HIDE : SW_SHOWNA);
            PipelineDiagnostics::Log("preview-state", hasPreview ? "Live preview visible" : WideToUtf8(message));
        }
    }
    SetWindowTextW(connectionStatus_, connected ? L"●  CONNECTED" : L"●  DISCONNECTED");
    SetWindowTextW(streamStatus_, VideoTransportStateLabel(transportState));
    const int64_t totalSeconds = recording.durationUs / 1'000'000;
    wchar_t recordingTime[32]{};
    swprintf_s(recordingTime, L"%02lld:%02lld", totalSeconds / 60, totalSeconds % 60);
    std::wstring recordingText = recording.state == DirectRecorderState::WaitingForKeyFrame
        ? L"● REC ARMED · waiting for IDR"
        : recording.state == DirectRecorderState::Recording
            ? L"● REC " + std::wstring(recordingTime)
            : recording.state == DirectRecorderState::Failed ? L"REC FAILED" : L"REC  —";
    if (!recording.path.empty() && recording.state != DirectRecorderState::Idle)
        recordingText += L" · " + recording.path.filename().wstring();
    SetWindowTextW(recordStatus_, recordingText.c_str());
    SetWindowTextW(recordButton_, recording.Active() ? L"Stop REC" : L"Record");
    EnableWindow(modeCombo_, !recording.Active());
    EnableWindow(audioInputCombo_, !recording.Active());
    EnableWindow(audioOffsetEdit_, !recording.Active());
    EnableWindow(recordingFolderButton_, !recording.Active());
    EnableWindow(orientationCombo_, !recording.Active());
    if (audioSelection_.load() <= 0) {
        SetWindowTextW(audioLevel_, L"LEVEL  —  ·  No audio");
    } else if (!audio.running) {
        SetWindowTextW(audioLevel_, L"LEVEL  —  ·  Input unavailable");
    } else {
        const double db = audio.peak > 0.000001 ? std::max(-60.0, 20.0 * std::log10(audio.peak)) : -60.0;
        wchar_t level[160]{};
        swprintf_s(level, L"LEVEL  %+.1f dBFS  ·  %d Hz  ·  %d ch", db,
                   audio.format.sampleRate, audio.format.channels);
        SetWindowTextW(audioLevel_, level);
    }
    std::wstring battery = L"Phone · ";
    battery += device.batteryPercent >= 0 ? std::to_wstring(device.batteryPercent) + L"% · " : L"Battery unknown · ";
    if (device.batteryState == "charging") battery += L"Charging via " + Utf8ToWide(device.powerSource);
    else if (device.batteryState == "full") battery += L"Full via " + Utf8ToWide(device.powerSource);
    else if (device.batteryState == "discharging") battery += L"Discharging";
    else battery += L"Power unknown";
    if (std::isfinite(device.batteryTemperatureC)) battery += L" · " + FormatNumber(device.batteryTemperatureC, 1) + L" °C";
    if (std::isfinite(device.currentNowMilliamps))
        battery += L" · " + FormatNumber(device.currentNowMilliamps, 0) + L" mA";
    if (std::isfinite(device.voltageVolts)) battery += L" · " + FormatNumber(device.voltageVolts, 2) + L" V";
    if (std::isfinite(device.approximatePowerWatts))
        battery += L" · ~" + FormatNumber(device.approximatePowerWatts, 2) + L" W";
    battery += L" · Thermal " + Utf8ToWide(device.thermalStatus);
    SetWindowTextW(batteryStatus_, battery.c_str());
    SetWindowTextW(status_, Utf8ToWide(latest).c_str());
    SetWindowTextW(metricResolution_, (L"RES   " + (streaming && info.width ? std::to_wstring(info.width) + L"×" + std::to_wstring(info.height) : L"—")).c_str());
    std::wstring fpsRate = L"FPS   " + (streaming && info.fps ? std::to_wstring(info.fps) : L"—");
    if (streaming && info.bitrate) fpsRate += L"  ·  " + FormatNumber(info.bitrate / 1'000'000.0, 1) + L"M";
    SetWindowTextW(metricFps_, fpsRate.c_str());
    SetWindowTextW(metricBitrate_, (L"LIVE   " + (streaming && latency.latestLiveFrameAgeMs
        ? FormatMs(*latency.latestLiveFrameAgeMs) : L"Unknown")).c_str());
    std::wstring processing = L"DEC   " + (streaming && latency.latestDecodeCallMs > 0 ? FormatMs(latency.latestDecodeCallMs) : L"—");
    if (streaming && latency.processingLatency.samples) processing += L"  ·  PROC " + FormatMs(latency.processingLatency.medianMs);
    SetWindowTextW(metricLatency_, processing.c_str());
    std::wstring transport = L"USB   " + (streaming && latency.latestTransportMs >= 0 ? FormatMs(latency.latestTransportMs) : L"Unknown");
    transport += L"  ·  D " + std::to_wstring(renderer_.DroppedDecodedFrames()) + L"/" +
        std::to_wstring(obs.producerDrops + obs.obsSkippedFrames) + L"/" +
        std::to_wstring(virtualCamera.producerDrops + virtualCamera.consumerDrops);
    SetWindowTextW(metricDrops_, transport.c_str());
    SetWindowTextW(isoValue_, (L"ISO                                      " + std::to_wstring(state.iso)).c_str());
    const int64_t denominator = state.exposureNs > 0 ? static_cast<int64_t>(1'000'000'000.0 / state.exposureNs) : 0;
    SetWindowTextW(shutterValue_, (L"SHUTTER                              1/" + std::to_wstring(denominator) + L" s").c_str());
    SetWindowTextW(focusValue_, (L"DISTANCE                              " + FormatNumber(state.focusDistance, 2) + L" D").c_str());
    SetWindowTextW(redValue_, (L"RED GAIN                              " + FormatNumber(state.redGain, 2)).c_str());
    SetWindowTextW(blueValue_, (L"BLUE GAIN                           " + FormatNumber(state.blueGain, 2)).c_str());
    SetWindowTextW(zoomValue_, (FormatNumber(state.zoom, 2) + L"×").c_str());
    InvalidateRect(connectionStatus_, nullptr, TRUE);
    InvalidateRect(streamStatus_, nullptr, TRUE);
    InvalidateRect(recordStatus_, nullptr, TRUE);
    InvalidateRect(recordButton_, nullptr, TRUE);
}

void AppWindow::SendExposure(std::optional<bool> automaticOverride) {
    CameraState state; CameraCapabilities capabilities;
    { std::lock_guard lock(stateMutex_); state = state_; capabilities = capabilities_; }
    const bool automatic = automaticOverride.value_or(state.autoExposure);
    const int iso = static_cast<int>(std::llround(LogValue(static_cast<int>(SendMessageW(isoSlider_, TBM_GETPOS, 0, 0)), capabilities.isoMin, capabilities.isoMax)));
    const double shutterMax = std::min<double>(capabilities.exposureMaxNs, capabilities.maxFrameDurationNs);
    const int64_t shutter = static_cast<int64_t>(std::llround(LogValue(static_cast<int>(SendMessageW(shutterSlider_, TBM_GETPOS, 0, 0)), capabilities.exposureMinNs, shutterMax)));
    std::string error;
    if (controls_.SetExposure(automatic, iso, shutter, state, error)) {
        { std::lock_guard lock(stateMutex_); state_ = state; }
        ApplyCapabilities(); UpdateStatusPanel();
    } else PostStatus(error);
}

void AppWindow::SendFocus(std::optional<bool> automaticOverride) {
    CameraState state; CameraCapabilities capabilities;
    { std::lock_guard lock(stateMutex_); state = state_; capabilities = capabilities_; }
    const bool automatic = automaticOverride.value_or(state.autoFocus);
    const double focus = SendMessageW(focusSlider_, TBM_GETPOS, 0, 0) / 1000.0 * capabilities.focusMax;
    std::string error;
    if (controls_.SetFocus(automatic, focus, state, error)) {
        { std::lock_guard lock(stateMutex_); state_ = state; }
        ApplyCapabilities(); UpdateStatusPanel();
    } else PostStatus(error);
}

void AppWindow::SendWhiteBalance(std::optional<bool> automaticOverride) {
    CameraState state; CameraCapabilities capabilities;
    { std::lock_guard lock(stateMutex_); state = state_; capabilities = capabilities_; }
    const bool automatic = automaticOverride.value_or(state.autoWhiteBalance);
    const auto gain = [&](HWND slider) { return capabilities.gainMin + SendMessageW(slider, TBM_GETPOS, 0, 0) / 1000.0 * (capabilities.gainMax - capabilities.gainMin); };
    std::string error;
    if (controls_.SetWhiteBalance(automatic, gain(redSlider_), gain(blueSlider_), state, error)) {
        { std::lock_guard lock(stateMutex_); state_ = state; }
        ApplyCapabilities(); UpdateStatusPanel();
    } else PostStatus(error);
}

void AppWindow::SendZoom(double requested) {
    CameraState state; CameraCapabilities capabilities;
    { std::lock_guard lock(stateMutex_); state = state_; capabilities = capabilities_; }
    const double zoom = requested > 0 ? requested : capabilities.zoomMin + SendMessageW(zoomSlider_, TBM_GETPOS, 0, 0) / 1000.0 * (capabilities.zoomMax - capabilities.zoomMin);
    std::string error;
    if (controls_.SetZoom(zoom, state, error)) {
        { std::lock_guard lock(stateMutex_); state_ = state; }
        ApplyCapabilities(); UpdateStatusPanel();
    } else PostStatus(error);
}

void AppWindow::HandleSlider(HWND slider, int notification) {
    if (notification != TB_ENDTRACK && notification != TB_THUMBPOSITION) return;
    if (slider == isoSlider_ || slider == shutterSlider_) SendExposure();
    else if (slider == focusSlider_) SendFocus();
    else if (slider == redSlider_ || slider == blueSlider_) SendWhiteBalance();
    else if (slider == zoomSlider_) SendZoom();
}

void AppWindow::HandleCommand(int id, int notification) {
    if (id == ID_AUDIO_OFFSET && notification == EN_CHANGE) {
        wchar_t text[32]{};
        GetWindowTextW(audioOffsetEdit_, text, static_cast<int>(std::size(text)));
        wchar_t* end = nullptr;
        const long offset = std::wcstol(text, &end, 10);
        if (end != text && *end == L'\0' && offset >= -5000 && offset <= 5000)
            sessionSettings_.audioOffsetMs = static_cast<int>(offset);
        return;
    }
    if (id == ID_MODE && notification == CBN_SELCHANGE) {
        if (recorder_.Snapshot().Active()) {
            SendMessageW(modeCombo_, CB_SETCURSEL, modeSelection_.load(), 0);
            PostStatus("Stop recording before changing resolution");
            return;
        }
        const int selected = static_cast<int>(SendMessageW(modeCombo_, CB_GETCURSEL, 0, 0));
        modeSelection_.store(selected);
        if (transport_.Running()) StopStream();
        BeginConnect();
        return;
    }
    if (id == ID_AUDIO_INPUT && notification == CBN_SELCHANGE) {
        const int selected = static_cast<int>(SendMessageW(audioInputCombo_, CB_GETCURSEL, 0, 0));
        if (!recorder_.Snapshot().Active() && selected >= 0 && selected <= static_cast<int>(audioDevices_.size()))
            sessionSettings_.audioDeviceId = selected == 0 ? "" : WideToUtf8(audioDevices_[selected - 1].id);
        SelectAudioInput(selected);
        return;
    }
    if (id == ID_ORIENTATION && notification == CBN_SELCHANGE) {
        if (recorder_.Snapshot().Active()) {
            SendMessageW(orientationCombo_, CB_SETCURSEL, orientationSelection_.load(), 0);
            PostStatus("Stop recording before changing orientation");
            return;
        }
        orientationSelection_.store(static_cast<int>(SendMessageW(orientationCombo_, CB_GETCURSEL, 0, 0)));
        UpdateViewTransform(); return;
    }
    if (id == ID_SCALE && notification == CBN_SELCHANGE) {
        scaleSelection_.store(static_cast<int>(SendMessageW(scaleCombo_, CB_GETCURSEL, 0, 0)));
        UpdateViewTransform(); return;
    }
    switch (id) {
        case ID_START: BeginConnect(); break;
        case ID_STOP: StopStream(); break;
        case ID_RECORD: ToggleRecording(); break;
        case ID_RECORDING_FOLDER: ChooseRecordingFolder(); break;
        case ID_AE:
        case ID_AF:
        case ID_AWB: {
            CameraState requested;
            {
                std::lock_guard lock(stateMutex_);
                const auto control = id == ID_AE ? AutomaticControl::Exposure :
                    id == ID_AF ? AutomaticControl::Focus : AutomaticControl::WhiteBalance;
                requested = ToggleAutomaticControl(state_, control);
            }
            if (id == ID_AE) SendExposure(requested.autoExposure);
            else if (id == ID_AF) SendFocus(requested.autoFocus);
            else SendWhiteBalance(requested.autoWhiteBalance);
            if (id == ID_AF && !requested.autoFocus) {
                { std::lock_guard lock(focusMutex_); activeFocusRequestId_.clear(); }
                renderer_.ClearFocusIndicator();
                focusVisualState_ = 0;
            }
            break;
        }
        case ID_RESUME_FOCUS: ResumeContinuousFocus(); break;
        case ID_ZOOM_1: SendZoom(1.0); break;
        case ID_ZOOM_15: SendZoom(1.5); break;
        case ID_ZOOM_2: SendZoom(2.0); break;
        case ID_RESET: {
            CameraState state; std::string error;
            if (controls_.Reset(state, error)) {
                { std::lock_guard lock(stateMutex_); state_ = state; }
                ApplyCapabilities(); UpdateStatusPanel();
                { std::lock_guard lock(focusMutex_); activeFocusRequestId_.clear(); }
                renderer_.ClearFocusIndicator(); focusVisualState_ = 0;
            } else PostStatus(error);
            break;
        }
    }
}

void AppWindow::OnDestroy() {
    if (closing_.exchange(true)) return;
    KillTimer(window_, 1);
    recorder_.Stop();
    audioCapture_.Stop();
    obsPublisher_.StopStream();
    virtualCameraPublisher_.StopStream();
    transport_.Stop();
    decoder_.Stop();
    if (connectionThread_.joinable()) connectionThread_.join();
    if (orientationThread_.joinable()) orientationThread_.join();
    if (focusThread_.joinable()) focusThread_.join();
    if (deviceStatusThread_.joinable()) deviceStatusThread_.join();
    if (displayPowerThread_.joinable()) displayPowerThread_.join();
    SaveSessionPreferences();
    ExportLatency();
    obsPublisher_.Shutdown();
    virtualCameraPublisher_.Shutdown();
    for (HFONT font : {titleFont_, subtitleFont_, sectionFont_, bodyFont_, metricFont_}) if (font) DeleteObject(font);
    if (panelBrush_) DeleteObject(panelBrush_);
    if (backgroundBrush_) DeleteObject(backgroundBrush_);
}

LRESULT CALLBACK AppWindow::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    AppWindow* app = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<AppWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        app->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
        case WM_CREATE: return app->OnCreate() ? 0 : -1;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
            GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
            limits->ptMinTrackSize = {
                std::min(app->Px(720), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left)),
                std::min(app->Px(480), static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top))};
            return 0;
        }
        case WM_DPICHANGED: {
            app->dpi_ = HIWORD(wParam);
            app->UpdateFonts();
            const auto& suggested = *reinterpret_cast<RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested.left, suggested.top,
                suggested.right - suggested.left, suggested.bottom - suggested.top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            RECT client{}; GetClientRect(window, &client);
            app->Layout(client.right, client.bottom);
            return 0;
        }
        case WM_WINDOWPOSCHANGING: {
            const auto* position = reinterpret_cast<const WINDOWPOS*>(lParam);
            if (position && !(position->flags & SWP_NOSIZE)) app->renderer_.SetResizeInProgress(true);
            return DefWindowProcW(window, message, wParam, lParam);
        }
        case WM_WINDOWPOSCHANGED: {
            const LRESULT result = DefWindowProcW(window, message, wParam, lParam);
            app->renderer_.SetResizeInProgress(false);
            return result;
        }
        case WM_SIZE:
            PipelineDiagnostics::Log("resize", std::string("main WM_SIZE ") + SizeStateLabel(wParam) +
                " client=" + std::to_string(LOWORD(lParam)) + "x" + std::to_string(HIWORD(lParam)));
            if (wParam == SIZE_MINIMIZED) { app->renderer_.SetMinimized(true); return 0; }
            app->renderer_.SetMinimized(false);
            app->Layout(LOWORD(lParam), HIWORD(lParam));
            return 0;
        case WM_COMMAND: app->HandleCommand(LOWORD(wParam), HIWORD(wParam)); return 0;
        case WM_QUERYENDSESSION: app->SaveSessionPreferences(); return TRUE;
        case WM_HSCROLL: app->HandleSlider(reinterpret_cast<HWND>(lParam), LOWORD(wParam)); return 0;
        case WM_MOUSEWHEEL: return SendMessageW(app->controlsPanel_, message, wParam, lParam);
        case WM_TIMER:
            if (DeviceDiscoveryRetryDue(app->connectionRequested_.load(), app->connecting_.load(),
                    app->transport_.Running(), GetTickCount64(), app->discoveryRetryAtMs_.load()))
                app->BeginConnect();
            if (app->focusVisualState_ != 1 && app->focusIndicatorExpiresNs_ > 0 &&
                QpcNanoseconds() >= app->focusIndicatorExpiresNs_) {
                app->renderer_.ClearFocusIndicator(); app->focusVisualState_ = 0; app->focusIndicatorExpiresNs_ = 0;
            }
            app->UpdateStatusPanel(); return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint); app->PaintBackground(dc); EndPaint(window, &paint); return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_DRAWITEM: return app->DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam)) ? TRUE : FALSE;
        case WM_NOTIFY: return app->DrawControl(reinterpret_cast<NMCUSTOMDRAW*>(lParam));
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam); HWND control = reinterpret_cast<HWND>(lParam);
            SetBkMode(dc, TRANSPARENT);
            COLORREF color = kText;
            if (control == app->subtitle_ || control == app->status_ || control == app->batteryStatus_ ||
                control == app->recordingFolderLabel_) color = kMuted;
            else if (control == app->recordStatus_) color = app->recorder_.Snapshot().Active() ? kRed : kMuted;
            else if (control == app->connectionStatus_) color = app->connected_.load() ? kGreen : kMuted;
            else if (control == app->streamStatus_) {
                const auto state = app->transport_.State();
                color = state == VideoTransportState::Streaming ? kGreen :
                    state == VideoTransportState::Error ? kRed :
                    state == VideoTransportState::Starting ? kAccent : kMuted;
            }
            SetTextColor(dc, color);
            const bool inPanel = control == app->exposureTitle_ || control == app->isoValue_ || control == app->shutterValue_ ||
                control == app->focusTitle_ || control == app->focusValue_ || control == app->wbTitle_ ||
                control == app->redValue_ || control == app->blueValue_ || control == app->zoomTitle_ ||
                control == app->zoomValue_ || control == app->audioTitle_ || control == app->audioLevel_ ||
                control == app->audioOffsetLabel_ || control == app->recordingFolderLabel_;
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, inPanel ? kPanel : kBackground);
            return reinterpret_cast<LRESULT>(inPanel ? app->panelBrush_ : app->backgroundBrush_);
        }
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wParam); SetBkColor(dc, kPanel); SetTextColor(dc, kText);
            return reinterpret_cast<LRESULT>(app->panelBrush_);
        }
        case WM_APP_STATUS: {
            std::unique_ptr<std::string> status(reinterpret_cast<std::string*>(lParam));
            { std::lock_guard lock(app->stateMutex_); app->latestStatus_ = *status; }
            app->UpdateStatusPanel(); return 0;
        }
        case WM_APP_CONNECTED: app->ApplyCapabilities(); app->UpdateStatusPanel(); return 0;
        case WM_APP_STREAM_INFO: app->UpdateViewTransform(); app->UpdateStatusPanel(); return 0;
        case WM_APP_ORIENTATION: app->UpdateViewTransform(); app->UpdateStatusPanel(); return 0;
        case WM_APP_FOCUS_STATUS: {
            std::unique_ptr<FocusPointResult> result(reinterpret_cast<FocusPointResult*>(lParam));
            app->ApplyFocusStatus(*result); return 0;
        }
        case WM_APP_DEVICE_STATUS: app->UpdateStatusPanel(); return 0;
        case WM_DESTROY: app->OnDestroy(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void AppWindow::ScrollPanel(int position) {
    position = std::clamp(position, 0, panelScrollMax_);
    if (position == panelScroll_) return;
    panelScroll_ = position;
    RECT client{}; GetClientRect(window_, &client);
    Layout(client.right, client.bottom);
}

LRESULT CALLBACK AppWindow::PanelProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<AppWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (app) switch (message) {
        case WM_COMMAND: case WM_HSCROLL: case WM_NOTIFY: case WM_DRAWITEM:
        case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORBTN: case WM_CTLCOLORLISTBOX:
            return SendMessageW(app->window_, message, wParam, lParam);
        case WM_ERASEBKGND: {
            RECT client{}; GetClientRect(window, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client, app->panelBrush_); return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{}; const HDC dc = BeginPaint(window, &paint);
            RECT client{}; GetClientRect(window, &client); FillRect(dc, &client, app->panelBrush_);
            EndPaint(window, &paint); return 0;
        }
        case WM_MOUSEWHEEL:
            app->ScrollPanel(app->panelScroll_ - GET_WHEEL_DELTA_WPARAM(wParam) * 48 / WHEEL_DELTA);
            return 0;
        case WM_VSCROLL: {
            SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = SIF_ALL;
            GetScrollInfo(window, SB_VERT, &info);
            int position = app->panelScroll_;
            switch (LOWORD(wParam)) {
                case SB_LINEUP: position -= 32; break;
                case SB_LINEDOWN: position += 32; break;
                case SB_PAGEUP: position -= info.nPage; break;
                case SB_PAGEDOWN: position += info.nPage; break;
                case SB_THUMBTRACK: case SB_THUMBPOSITION: position = info.nTrackPos; break;
                case SB_TOP: position = 0; break;
                case SB_BOTTOM: position = app->panelScrollMax_; break;
            }
            app->ScrollPanel(position); return 0;
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK AppWindow::PreviewProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    AppWindow* app = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<AppWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (app) {
        if (message == WM_LBUTTONDOWN) {
            app->HandlePreviewClick(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
            return 0;
        }
        if (message == WM_APP_RENDER_FRAME) { app->renderer_.RenderLatest(); return 0; }
        if (message == WM_SIZE) {
            PipelineDiagnostics::Log("resize", std::string("preview WM_SIZE ") + SizeStateLabel(wParam) +
                " client=" + std::to_string(LOWORD(lParam)) + "x" + std::to_string(HIWORD(lParam)));
            app->renderer_.Resize(LOWORD(lParam), HIWORD(lParam),
                                  wParam == SIZE_MINIMIZED || LOWORD(lParam) == 0 || HIWORD(lParam) == 0);
            return 0;
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
