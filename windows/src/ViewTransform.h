#pragma once

#include <algorithm>

enum class PreviewScaleMode { Fit, Fill, Actual };
enum class PreviewOrientationMode { Auto, Portrait, Landscape, Rotate90, Rotate180, Rotate270 };

inline int NormalizeRightAngle(int degrees) {
    const int normalized = ((degrees % 360) + 360) % 360;
    return ((normalized + 45) / 90 * 90) % 360;
}

inline int ResolvePreviewRotation(PreviewOrientationMode mode, int sensorOrientation, int deviceRotation,
                                  int sourceWidth, int sourceHeight) {
    switch (mode) {
        case PreviewOrientationMode::Auto:
            return NormalizeRightAngle(sensorOrientation - deviceRotation);
        case PreviewOrientationMode::Portrait:
            return sourceWidth >= sourceHeight ? 90 : 0;
        case PreviewOrientationMode::Landscape:
            return sourceWidth >= sourceHeight ? 0 : 90;
        case PreviewOrientationMode::Rotate90: return 90;
        case PreviewOrientationMode::Rotate180: return 180;
        case PreviewOrientationMode::Rotate270: return 270;
    }
    return 0;
}
