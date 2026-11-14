#pragma once

inline int NormalizeRecordingRotation(int rotationDegrees) {
    const int normalized = ((rotationDegrees % 360) + 360) % 360;
    return normalized == 90 || normalized == 180 || normalized == 270 ? normalized : 0;
}
