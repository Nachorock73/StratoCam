# StratoCam

Use your Android phone as a USB camera for Windows. StratoCam provides a live preview, camera controls, local recording, an OBS source, and a virtual webcam listed as **StratoCam (Windows Virtual Camera)**.

## Download

[**Download StratoCam for Windows and Android — v0.5.3**](https://github.com/Nachorock73/StratoCam/releases/tag/v0.5.3)

- **Windows:** `StratoCam-Setup-v0.5.3.exe`
- **Android:** `StratoCam-Android-v0.5.3.apk`
- **Download integrity:** `SHA256SUMS.txt`

You do not need development tools or registration commands to use StratoCam.

## Install on Windows

Run the Windows installer and accept the administrator prompt. The English setup wizard guides you through these steps:

1. **Welcome:** click **Next**.
2. **Installation folder:** use `C:\Program Files\StratoCam` or choose another folder, then click **Next**.
3. **Shortcuts:** choose whether to create Desktop and Start Menu shortcuts. The Start Menu shortcut does not launch StratoCam when Windows starts. Click **Install**.
4. **Installing:** follow the progress bar, percentage, file status, and destination path. Setup also registers the StratoCam webcam automatically.
5. **Finish:** check or uncheck **Launch StratoCam**, then click **Finish**.

ADB and the required runtime libraries are included in the installer.

## Install on Android and connect

1. Open the downloaded APK on your phone and install it. Android may ask you to allow installation from your browser or file manager.
2. Open StratoCam on Android and grant camera permission.
3. Enable **Developer options → USB debugging** and connect the phone using a USB data cable.
4. Accept the computer's USB debugging authorization on the phone. You can select **Always allow from this computer**.
5. Open StratoCam on Windows. It detects the authorized device and starts video. If you connect the phone later, device discovery retries automatically.

Windows authorization is requested during installation. Camera and USB debugging permissions must be accepted on Android; an application cannot grant itself these system permissions.

Without a connected device, the preview displays **Please connect an Android device via USB**. Once video arrives, StratoCam powers off the phone's physical display on supported devices while capture remains active. It restores the display when streaming stops or the application closes. If a device does not support this control, video continues with its display on.

## Use as a webcam

Keep StratoCam open with video running, then select **StratoCam (Windows Virtual Camera)** in your video-call application, browser, or capture software. Windows automatically appends the virtual-camera suffix to the base name **StratoCam**.

Restart applications that were already displaying a camera list when StratoCam was installed. Without an active stream, the virtual camera outputs black frames.

## Controls and recording

- **Start / Stop:** start or stop video.
- **Exposure, Focus, White Balance, and Zoom:** automatic modes and manual controls supported by the camera.
- **Click-to-focus:** click the image with autofocus enabled. Use **Resume continuous AF** to return to continuous focus.
- **Orientation and scale:** Auto, Portrait, Landscape, manual rotation, Fit, Fill, and actual size.
- **Record:** save the phone's original H.264 bitstream to fragmented MP4 without re-encoding video.
- **Audio Input:** select a Windows microphone or audio interface, or **No audio**. Audio is encoded as AAC-LC at 48 kHz.
- **Browse:** choose your recording folder. The default is `Videos\StratoCam`.
- **Audio Sync Offset:** optional manual audio timing adjustment in milliseconds.

Choose resolution, orientation, and audio input before recording. Use **Stop REC** to finish the file.

## OBS

OBS integration is installed automatically. Restart OBS Studio 32.2.2 x64 and add the **StratoCam** source. You can also select the StratoCam webcam as a video capture device.

## Requirements and current status

- Windows 11 x64, build 22000 or later for the virtual camera.
- Android 6.0 or later with Camera2, H.264 encoding, and USB debugging available.
- One phone connected with a USB data cable. Some manufacturers require a Windows USB driver.
- 1080p30, 4K30, and manual controls depend on the camera and device capabilities.
- Media Foundation/D3D11 hardware decoding with OpenH264 software fallback.

**Version 0.5.3 is under development.** Device compatibility varies, and a visual issue when maximizing the window remains open. The current camera selection uses camera ID `0`; a universal lens selector is not implemented. The Windows installer is not Authenticode-signed, so Windows may display an unknown-publisher warning. The Android APK is signed.

Android audio capture, HEVC, and high-frame-rate modes are not included.

## Troubleshooting

- Unlock the phone, enable USB debugging, and accept the computer's authorization.
- Use a data cable and try another USB port. Selecting file-transfer mode helps on some devices.
- Check Windows camera and microphone permissions for desktop applications.
- Run the installer again to repair the virtual camera. No registration scripts are required.
- Connect audio devices before opening StratoCam if they do not appear in the input list.

## Privacy

Video and control use USB/ADB with local sockets. StratoCam requires no account, cloud service, or Wi-Fi connection and includes no analytics. Recordings stay in your chosen folder.

## Source code

See [architecture](docs/ARCHITECTURE.md), [building from source](docs/BUILDING.md), and [Android signing](docs/RELEASE_SIGNING.md). These instructions are for development; release downloads include the components needed to run the application.

Application code is licensed under [Apache-2.0](LICENSE). The OBS module has a separate GPL-2.0-or-later notice. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for dependency licenses.
