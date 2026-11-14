#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include "ComPtr.h"
#include "MfVirtualCameraCompat.h"

#include <mfapi.h>
#include <mferror.h>
#include <cstdint>
#include <iostream>
#include <string>

namespace {
std::wstring HresultText(HRESULT result) {
    wchar_t* message{};
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, result, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring text = message ? message : L"Unknown error";
    if (message) LocalFree(message);
    return text;
}

HRESULT OpenCamera(LPCWSTR friendlyName, MFVirtualCameraAccess access, IMFVirtualCamera** camera) {
    using CreateVirtualCamera = HRESULT (WINAPI*)(MFVirtualCameraType, MFVirtualCameraLifetime,
        MFVirtualCameraAccess, LPCWSTR, LPCWSTR, const GUID*, ULONG, IMFVirtualCamera**);
    HMODULE library = LoadLibraryW(L"mfsensorgroup.dll");
    if (!library) return HRESULT_FROM_WIN32(GetLastError());
    auto create = reinterpret_cast<CreateVirtualCamera>(GetProcAddress(library, "MFCreateVirtualCamera"));
    if (!create) { const HRESULT result = HRESULT_FROM_WIN32(GetLastError()); FreeLibrary(library); return result; }
    const HRESULT result = create(MFVirtualCameraType_SoftwareCameraSource,
                                  MFVirtualCameraLifetime_System,
                                  access,
                                  friendlyName,
                                  kAndroidCamVirtualCameraClsid,
                                  nullptr, 0, camera);
    // IMFVirtualCamera is implemented by this module; keep it loaded until process exit.
    return result;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || (std::wstring(argv[1]) != L"install" && std::wstring(argv[1]) != L"uninstall" &&
                      std::wstring(argv[1]) != L"uninstall-legacy" &&
                      std::wstring(argv[1]) != L"uninstall-current-user" &&
                      std::wstring(argv[1]) != L"uninstall-previous" &&
                      std::wstring(argv[1]) != L"uninstall-previous-current-user" &&
                      std::wstring(argv[1]) != L"status")) {
        std::wcerr << L"Usage: AndroidCamVirtualCameraManager.exe "
                      L"install|uninstall|uninstall-legacy|uninstall-current-user|"
                      L"uninstall-previous|uninstall-previous-current-user|status\n";
        return 2;
    }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) { std::wcerr << L"COM initialization failed: 0x" << std::hex << com << L"\n"; return 1; }
    const HRESULT startup = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(startup)) { std::wcerr << L"Media Foundation initialization failed: 0x" << std::hex << startup << L"\n"; CoUninitialize(); return 1; }
    const std::wstring command = argv[1];
    const bool previousName = command == L"uninstall-previous" || command == L"uninstall-previous-current-user";
    const wchar_t* friendlyName = command == L"uninstall-legacy" ? L"Android Cam"
        : previousName ? L"StratoCam Virtual Camera" : kAndroidCamVirtualCameraName;
    const bool removeCurrentUser = command == L"uninstall-legacy" || command == L"uninstall-current-user" ||
                                   command == L"uninstall-previous-current-user";
    const bool remove = command == L"uninstall" || removeCurrentUser || previousName;
    const MFVirtualCameraAccess access = removeCurrentUser
        ? MFVirtualCameraAccess_CurrentUser
        : MFVirtualCameraAccess_AllUsers;
    ComPtr<IMFVirtualCamera> camera;
    HRESULT result = OpenCamera(friendlyName, access, camera.Put());
    if (SUCCEEDED(result) && camera) {
        if (command == L"install") result = camera->Start(nullptr);
        else if (remove) {
            // Microsoft's manager sample disables a system-lifetime camera before
            // removing it.  Some Windows 11 builds fault inside Remove when the
            // device is still enabled instead of returning a useful HRESULT.
            // Remove is the authoritative operation.  In particular, Stop can
            // return E_ACCESSDENIED for a stale duplicate registration while
            // Remove can still retire that exact parameter-keyed instance.
            camera->Stop();
            result = camera->Remove();
        }
        else result = camera->Start(nullptr);
    }
    if (remove && HRESULT_CODE(result) == 0x36d5 &&
        HRESULT_FACILITY(result) == FACILITY_MF) result = S_OK;
    if (SUCCEEDED(result)) {
        std::wcout << friendlyName << (remove ? L" removed.\n" : L" is registered.\n");
    } else {
        std::wcerr << L"Virtual camera operation failed: 0x" << std::hex << result << L" — " << HresultText(result) << L"\n";
    }
    if (camera) camera->Shutdown();
    camera.Reset();
    MFShutdown();
    CoUninitialize();
    return SUCCEEDED(result) ? 0 : 1;
}
