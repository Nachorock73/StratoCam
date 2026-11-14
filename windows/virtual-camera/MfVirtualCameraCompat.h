#pragma once

#include <windows.h>
#include <devpropdef.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>

// MinGW currently ships the Windows 10 Media Foundation headers. These ABI
// declarations are copied from the Windows 11 SDK interfaces documented by
// Microsoft and are kept deliberately minimal.

#ifndef __IMFMediaStream2_INTERFACE_DEFINED__
#define __IMFMediaStream2_INTERFACE_DEFINED__
MIDL_INTERFACE("C5BC37D6-75C7-46A1-A132-81B5F723C20F")
IMFMediaStream2 : public IMFMediaStream {
public:
    virtual HRESULT STDMETHODCALLTYPE SetStreamState(MF_STREAM_STATE value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStreamState(MF_STREAM_STATE* value) = 0;
};
#endif

#ifndef __IMFSampleAllocatorControl_INTERFACE_DEFINED__
#define __IMFSampleAllocatorControl_INTERFACE_DEFINED__
enum MFSampleAllocatorUsage {
    MFSampleAllocatorUsage_UsesProvidedAllocator = 0,
    MFSampleAllocatorUsage_UsesCustomAllocator = 1,
    MFSampleAllocatorUsage_DoesNotAllocate = 2
};
MIDL_INTERFACE("DA62B958-3A38-4A97-BD27-149C640C0771")
IMFSampleAllocatorControl : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE SetDefaultAllocator(DWORD outputStreamId, IUnknown* allocator) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAllocatorUsage(DWORD outputStreamId, DWORD* inputStreamId,
                                                        MFSampleAllocatorUsage* usage) = 0;
};
#endif

enum MFVirtualCameraType { MFVirtualCameraType_SoftwareCameraSource = 0 };
enum MFVirtualCameraLifetime { MFVirtualCameraLifetime_Session = 0, MFVirtualCameraLifetime_System = 1 };
enum MFVirtualCameraAccess { MFVirtualCameraAccess_CurrentUser = 0, MFVirtualCameraAccess_AllUsers = 1 };

struct IMFCameraSyncObject;
MIDL_INTERFACE("1C08A864-EF6C-4C75-AF59-5F2D68DA9563")
IMFVirtualCamera : public IMFAttributes {
public:
    virtual HRESULT STDMETHODCALLTYPE AddDeviceSourceInfo(LPCWSTR deviceSourceInfo) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddProperty(const DEVPROPKEY* key, DEVPROPTYPE type,
                                                  const BYTE* data, ULONG dataBytes) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddRegistryEntry(LPCWSTR entryName, LPCWSTR subkeyPath,
                                                       DWORD registryType, const BYTE* data,
                                                       ULONG dataBytes) = 0;
    virtual HRESULT STDMETHODCALLTYPE Start(IMFAsyncCallback* callback) = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop() = 0;
    virtual HRESULT STDMETHODCALLTYPE Remove() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMediaSource(IMFMediaSource** mediaSource) = 0;
    virtual HRESULT STDMETHODCALLTYPE SendCameraProperty(REFGUID propertySet, ULONG propertyId,
                                                         ULONG propertyFlags, void* propertyPayload,
                                                         ULONG propertyPayloadLength, void* data,
                                                         ULONG dataLength, ULONG* dataWritten) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSyncEvent(REFGUID eventSet, ULONG eventId, ULONG eventFlags,
                                                      HANDLE eventHandle, IMFCameraSyncObject** object) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSyncSemaphore(REFGUID eventSet, ULONG eventId, ULONG eventFlags,
                                                          HANDLE semaphoreHandle, LONG adjustment,
                                                          IMFCameraSyncObject** object) = 0;
    virtual HRESULT STDMETHODCALLTYPE Shutdown() = 0;
};

extern "C" HRESULT WINAPI MFCreateVirtualCamera(MFVirtualCameraType type,
                                                  MFVirtualCameraLifetime lifetime,
                                                  MFVirtualCameraAccess access,
                                                  LPCWSTR friendlyName,
                                                  LPCWSTR sourceId,
                                                  const GUID* categories,
                                                  ULONG categoryCount,
                                                  IMFVirtualCamera** virtualCamera);

inline constexpr GUID IID_IMFMediaStream2_Compat =
    {0xc5bc37d6, 0x75c7, 0x46a1, {0xa1, 0x32, 0x81, 0xb5, 0xf7, 0x23, 0xc2, 0x0f}};
inline constexpr GUID IID_IMFSampleAllocatorControl_Compat =
    {0xda62b958, 0x3a38, 0x4a97, {0xbd, 0x27, 0x14, 0x9c, 0x64, 0x0c, 0x07, 0x71}};

inline constexpr GUID MF_DEVICESTREAM_STREAM_ID_Compat =
    {0x11bd5120, 0xd124, 0x446b, {0x88, 0xe6, 0x17, 0x06, 0x02, 0x57, 0xff, 0xf9}};
inline constexpr GUID MF_DEVICESTREAM_STREAM_CATEGORY_Compat =
    {0x2939e7b8, 0xa62e, 0x4579, {0xb6, 0x74, 0xd4, 0x07, 0x3d, 0xfa, 0xbb, 0xba}};
inline constexpr GUID MF_DEVICESTREAM_FRAMESERVER_SHARED_Compat =
    {0x1cb378e9, 0xb279, 0x41d4, {0xaf, 0x97, 0x34, 0xa2, 0x43, 0xe6, 0x83, 0x20}};
inline constexpr GUID MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES_Compat =
    {0x17145fd1, 0x1b2b, 0x423c, {0x80, 0x01, 0x2b, 0x68, 0x33, 0xed, 0x35, 0x88}};
inline constexpr GUID MF_DEVICEMFT_SENSORPROFILE_COLLECTION_Compat =
    {0x36ebdc44, 0xb12c, 0x441b, {0x89, 0xf4, 0x08, 0xb2, 0xf4, 0x1a, 0x9c, 0xfc}};
inline constexpr GUID KSCAMERAPROFILE_Legacy_Compat =
    {0xb4894d81, 0x62b7, 0x4eec, {0x87, 0x40, 0x80, 0x65, 0x8c, 0x4a, 0x9d, 0x3e}};

struct SENSORPROFILEID_Compat { GUID Type; UINT32 Index; UINT32 Unused; };
MIDL_INTERFACE("22F765D1-8DAB-4107-846D-56BAF72215E7")
IMFSensorProfileCompat : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE GetProfileId(SENSORPROFILEID_Compat* id) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddProfileFilter(UINT32 streamId, LPCWSTR filter) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsMediaTypeSupported(UINT32 streamId, IMFMediaType* type, BOOL* supported) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddBlockedControl(LPCWSTR control) = 0;
};
MIDL_INTERFACE("C95EA55B-0187-48BE-9353-8D2507662351")
IMFSensorProfileCollectionCompat : public IUnknown {
public:
    virtual DWORD STDMETHODCALLTYPE GetProfileCount() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProfile(DWORD index, IMFSensorProfileCompat** profile) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddProfile(IMFSensorProfileCompat* profile) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindProfile(SENSORPROFILEID_Compat* id, IMFSensorProfileCompat** profile) = 0;
    virtual void STDMETHODCALLTYPE RemoveProfileByIndex(DWORD index) = 0;
    virtual void STDMETHODCALLTYPE RemoveProfile(SENSORPROFILEID_Compat* id) = 0;
};

inline constexpr GUID CLSID_AndroidCamVirtualCamera =
    {0x82575855, 0x8d10, 0x4908, {0x91, 0x76, 0x1b, 0x7e, 0x5b, 0x7b, 0x0c, 0x8f}};
inline constexpr wchar_t kAndroidCamVirtualCameraClsid[] = L"{82575855-8D10-4908-9176-1B7E5B7B0C8F}";
inline constexpr wchar_t kAndroidCamVirtualCameraName[] = L"StratoCam";
