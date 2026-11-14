# Building StratoCam

Development prerequisites: Windows, Git, PowerShell, and Python 3 with the `py` launcher.

```powershell
.\scripts\bootstrap-windows-deps.ps1
.\scripts\bootstrap-android-sdk.ps1 -AcceptAndroidSdkLicenses
.\scripts\bootstrap-inno-setup.ps1
.\scripts\generate-icons.ps1
.\scripts\build-windows-app.ps1
.\scripts\build-virtual-camera.ps1
.\scripts\build-obs-plugin.ps1
.\scripts\build-android-release.ps1
.\scripts\package-release.ps1 -CleanDist
```

Dependencies are installed under `.tools/` without changing the global PATH. `version.properties` defines the Windows, Android, and installer version. Android signing configuration remains local and outside Git; see [RELEASE_SIGNING.md](RELEASE_SIGNING.md).

`build-windows-app.ps1` runs native tests. `build-android-release.ps1` runs release unit tests and verifies the APK signature when complete signing configuration is available.

Artifacts, redistributed licenses, and `SHA256SUMS.txt` are generated under `dist/`. The installer includes ADB, media libraries, the APK, virtual-camera components, and the OBS module. OBS integration is always installed. Webcam registration runs automatically with the installer's administrator privileges, and its exit code is checked.

Manual component-installation scripts remain available for development and diagnostics. They are not required by users who download the installer.

Packaging also compiles `android-shell/src/com/stratocam/shell/DisplayPower.java` with the local JDK and D8. The resulting DEX is installed under `Support/` and executed through the authorized ADB shell to control the physical display. It does not require privileged APK permissions. The session holds stdin open and restores the display when that channel closes. Some manufacturers or Android versions do not expose the required system APIs; in that case streaming continues with the display on.
