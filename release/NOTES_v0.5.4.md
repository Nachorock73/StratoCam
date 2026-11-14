# StratoCam v0.5.4

- Save session settings on application close and restore them on the next launch.
- Remember resolution, orientation, preview scale, audio input, Audio Sync Offset, and exposure/focus/white-balance/zoom settings.
- Preserve independent AE, AF, and AWB modes and validate camera values against device capabilities when restoring.
- Match audio inputs by endpoint identity; use No audio if the saved device is unavailable.
- English installer and repository documentation, concise setup pages, automatic OBS integration, and a Launch StratoCam checkbox on the Finish page.

Download `StratoCam-Setup-v0.5.4.exe` for Windows 11 x64 and `StratoCam-Android-v0.5.4.apk` for Android. Webcam registration is handled automatically by the Windows installer.

Preferences are local to the Windows account and are not uploaded. Recording does not restart automatically. Development release; see the README for requirements and known limitations. The Windows installer is not Authenticode-signed; the APK is signed. Verify downloads with `SHA256SUMS.txt`.
