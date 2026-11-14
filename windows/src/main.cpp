#include "AppWindow.h"
#include "Version.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        MessageBoxW(nullptr, L"Could not initialize Windows COM services.", kProductName, MB_ICONERROR);
        return 1;
    }
    WSADATA sockets{};
    if (WSAStartup(MAKEWORD(2, 2), &sockets) != 0) {
        MessageBoxW(nullptr, L"Could not initialize Windows sockets.", kProductName, MB_ICONERROR);
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
    INITCOMMONCONTROLSEX controls{sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    int result = 1;
    try {
        AppWindow app;
        if (!app.Create(instance, showCommand)) throw std::runtime_error("Could not create the main window: " + LastErrorText());
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        result = static_cast<int>(message.wParam);
    } catch (const std::exception& error) {
        MessageBoxW(nullptr, Utf8ToWide(error.what()).c_str(), kProductName, MB_ICONERROR);
    }
    WSACleanup();
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
