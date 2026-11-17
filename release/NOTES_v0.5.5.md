# StratoCam v0.5.5

- Adapt the Windows interface to display scaling and window size, including per-monitor DPI changes.
- Reorganize the header on narrower windows and keep camera and audio controls accessible in a scrollable panel.
- Keep the preview within its layout bounds and preserve its aspect ratio and click-to-focus coordinate mapping.
- Fix Android controls overlapping the bottom buttons by measuring the action bar and respecting system bars and display cutouts.
- Preserve session settings, hardware decoding, direct H.264 recording, OBS integration, and the virtual camera.

Validation includes automated Windows layout checks at 100%, 125%, 150%, and 200% scaling, native and Android unit tests, Android portrait/landscape checks with normal and enlarged text, and Windows controls/settings restoration. Compatibility with other devices and physical multi-monitor configurations still needs feedback.

Download `StratoCam-Setup-v0.5.5.exe` for Windows 11 x64 and `StratoCam-Android-v0.5.5.apk` for Android. The installer registers the webcam automatically and includes OBS integration. USB debugging is still required; this release does not introduce an ADB-free connection.

Development release. See the README for requirements and known limitations. The Windows installer is not Authenticode-signed; the APK is signed. Verify downloads with `SHA256SUMS.txt`.
