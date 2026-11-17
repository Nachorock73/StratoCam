# Adaptive UI layout

The Windows UI uses 96-DPI logical coordinates and per-monitor V2 DPI awareness. Fonts, control bounds, and combo-box item heights are rebuilt on `WM_DPICHANGED`. The suggested window rectangle is applied before layout is recomputed.

The header wraps on narrower windows. The preview keeps its effective rotated aspect ratio. A separate opaque, scrollable controls panel keeps the camera and audio controls accessible on short displays. Its notifications are forwarded to the existing camera-control handlers. Scrolling does not change slider values. Preview hit testing continues to use the preview HWND's actual pixel dimensions.

The Android preview can extend behind system bars; interactive overlays cannot. Insets from system bars and display cutouts pad the overlay. The status area, scrollable controls viewport, and action bar use a vertical layout. The controls viewport uses only the space remaining above the measured buttons, rather than assuming a fixed button-bar height.

## Checks

- `scripts/build-windows-app.ps1`: native tests include preview bounds at logical client sizes from 640×360 upward, with all four rotations and existing Fit/Fill/Actual and sensor-coordinate tests.
- `scripts/test-adaptive-layout.ps1`: launches a hidden application, exercises DPI-change handling at 100%, 125%, 150%, and 200%, and checks preview/panel separation, control widths, and access to Reset after scrolling. Results are written to ignored `captures/adaptive-layout-test.json`. DPI messages simulate transitions; this does not replace testing on physical monitors with different scales.
- `scripts/test-session-settings.ps1`: uses the existing camera-control and preferences checks with controls inside their panel.
- Android unit tests and compilation cover the existing camera state and control behavior. Actual navigation modes, display cutouts, and manufacturer-specific UI still require device checks.

These changes do not replace USB/ADB. Network or Android Open Accessory transport would be separate work. In particular, the current physical-display power helper uses ADB shell privileges and cannot simply be carried over to an ordinary network connection.
