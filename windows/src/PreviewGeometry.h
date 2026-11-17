#pragma once

#include "ViewTransform.h"

#include <algorithm>
#include <cmath>

struct SensorActiveArray {
    int left{};
    int top{};
    int right{};
    int bottom{};
};

struct PreviewPointMapping {
    bool inside{};
    double sourceU{};
    double sourceV{};
    double indicatorU{};
    double indicatorV{};
    int sensorX{};
    int sensorY{};
};

struct PreviewDisplayGeometry {
    bool valid{};
    double imageLeft{};
    double imageTop{};
    double imageWidth{};
    double imageHeight{};
    double sampleScaleX{1.0};
    double sampleScaleY{1.0};
};

struct WindowPreviewLayout {
    int headerHeight{};
    int panelWidth{};
    int toolbarHeight{};
    int metricsHeight{};
    int panelX{};
    int leftWidth{};
    int previewAreaHeight{};
    int previewX{};
    int previewY{};
    int previewWidth{};
    int previewHeight{};
    int toolbarY{};
    int metricsY{};
};

inline double EffectivePreviewAspect(int sourceWidth, int sourceHeight, int rotationDegrees) {
    const bool quarterTurn = rotationDegrees == 90 || rotationDegrees == 270;
    const double width = quarterTurn ? sourceHeight : sourceWidth;
    const double height = quarterTurn ? sourceWidth : sourceHeight;
    return width > 0 && height > 0 ? width / height : 16.0 / 9.0;
}

inline PreviewDisplayGeometry CalculatePreviewDisplayGeometry(
        double viewWidth, double viewHeight, int sourceWidth, int sourceHeight,
        int rotationDegrees, PreviewScaleMode scaleMode) {
    PreviewDisplayGeometry result{};
    if (viewWidth <= 0 || viewHeight <= 0 || sourceWidth <= 0 || sourceHeight <= 0) return result;
    const bool quarterTurn = NormalizeRightAngle(rotationDegrees) == 90 ||
                             NormalizeRightAngle(rotationDegrees) == 270;
    const double displayWidth = quarterTurn ? sourceHeight : sourceWidth;
    const double displayHeight = quarterTurn ? sourceWidth : sourceHeight;
    result.imageWidth = viewWidth;
    result.imageHeight = viewHeight;
    if (scaleMode == PreviewScaleMode::Fit || scaleMode == PreviewScaleMode::Actual) {
        const double fitScale = std::min(viewWidth / displayWidth, viewHeight / displayHeight);
        const double scale = scaleMode == PreviewScaleMode::Actual ? std::min(1.0, fitScale) : fitScale;
        result.imageWidth = displayWidth * scale;
        result.imageHeight = displayHeight * scale;
        result.imageLeft = (viewWidth - result.imageWidth) * 0.5;
        result.imageTop = (viewHeight - result.imageHeight) * 0.5;
    } else {
        const double sourceAspect = displayWidth / displayHeight;
        const double viewAspect = viewWidth / viewHeight;
        if (sourceAspect > viewAspect) result.sampleScaleX = viewAspect / sourceAspect;
        else result.sampleScaleY = sourceAspect / viewAspect;
    }
    result.valid = result.imageWidth > 0 && result.imageHeight > 0;
    return result;
}

inline WindowPreviewLayout CalculateWindowPreviewLayout(
        int clientWidth, int clientHeight, int sourceWidth, int sourceHeight, int rotationDegrees) {
    constexpr int margin = 20;
    constexpr int footerHeight = 26;
    WindowPreviewLayout result{};
    result.headerHeight = clientWidth < 1280 ? (clientHeight < 520 ? 108 : 128) : 72;
    result.panelWidth = clientWidth < 1000 ? 260 : (clientWidth < 1200 ? 300 : 360);
    result.panelX = clientWidth - result.panelWidth - margin;
    result.leftWidth = std::max(1, result.panelX - margin * 2);
    result.toolbarHeight = result.leftWidth < 720 && clientHeight >= 520 ? 76 : 48;
    result.metricsHeight = result.leftWidth < 720 ? (clientHeight < 520 ? 80 : 96) : 64;
    const int previewTop = result.headerHeight + 12;
    result.previewAreaHeight = std::max(
        1, clientHeight - previewTop - result.metricsHeight - result.toolbarHeight - footerHeight - margin * 2);
    const double aspect = EffectivePreviewAspect(sourceWidth, sourceHeight, rotationDegrees);
    result.previewWidth = std::max(1, std::min(
        result.leftWidth, static_cast<int>(result.previewAreaHeight * aspect)));
    result.previewHeight = std::max(1, std::min(
        result.previewAreaHeight, static_cast<int>(result.previewWidth / aspect)));
    result.previewX = margin + (result.leftWidth - result.previewWidth) / 2;
    result.previewY = previewTop + (result.previewAreaHeight - result.previewHeight) / 2;
    result.toolbarY = previewTop + result.previewAreaHeight + 10;
    result.metricsY = result.toolbarY + result.toolbarHeight + 8;
    return result;
}

inline PreviewPointMapping MapPreviewPoint(double mouseX, double mouseY, double viewWidth, double viewHeight,
                                           int sourceWidth, int sourceHeight, int rotationDegrees,
                                           PreviewScaleMode scaleMode, const SensorActiveArray& activeArray,
                                           double zoomRatio, bool zoomRatioCoordinates = true) {
    PreviewPointMapping result{};
    if (viewWidth <= 0 || viewHeight <= 0 || sourceWidth <= 0 || sourceHeight <= 0 ||
        activeArray.right <= activeArray.left || activeArray.bottom <= activeArray.top) return result;
    const auto geometry = CalculatePreviewDisplayGeometry(
        viewWidth, viewHeight, sourceWidth, sourceHeight, rotationDegrees, scaleMode);
    if (!geometry.valid) return result;
    double displayU{};
    double displayV{};
    if (scaleMode == PreviewScaleMode::Fit || scaleMode == PreviewScaleMode::Actual) {
        if (mouseX < geometry.imageLeft || mouseY < geometry.imageTop ||
            mouseX >= geometry.imageLeft + geometry.imageWidth ||
            mouseY >= geometry.imageTop + geometry.imageHeight) return result;
        displayU = (mouseX - geometry.imageLeft) / geometry.imageWidth;
        displayV = (mouseY - geometry.imageTop) / geometry.imageHeight;
        result.indicatorU = displayU;
        result.indicatorV = displayV;
    } else {
        if (mouseX < 0 || mouseY < 0 || mouseX >= viewWidth || mouseY >= viewHeight) return result;
        displayU = mouseX / viewWidth;
        displayV = mouseY / viewHeight;
        result.indicatorU = displayU;
        result.indicatorV = displayV;
        displayU = (displayU - 0.5) * geometry.sampleScaleX + 0.5;
        displayV = (displayV - 0.5) * geometry.sampleScaleY + 0.5;
    }
    switch (NormalizeRightAngle(rotationDegrees)) {
        case 90: result.sourceU = displayV; result.sourceV = 1.0 - displayU; break;
        case 180: result.sourceU = 1.0 - displayU; result.sourceV = 1.0 - displayV; break;
        case 270: result.sourceU = 1.0 - displayV; result.sourceV = displayU; break;
        default: result.sourceU = displayU; result.sourceV = displayV; break;
    }
    result.sourceU = std::clamp(result.sourceU, 0.0, 1.0);
    result.sourceV = std::clamp(result.sourceV, 0.0, 1.0);
    // API 30 CONTROL_ZOOM_RATIO keeps AF coordinates relative to the post-zoom field of view.
    // Retain the pre-API-30 crop calculation for a future SCALER_CROP_REGION transport.
    const double zoom = zoomRatioCoordinates ? 1.0 : std::max(1.0, zoomRatio);
    const double activeWidth = activeArray.right - activeArray.left;
    const double activeHeight = activeArray.bottom - activeArray.top;
    double cropWidth = activeWidth / zoom;
    double cropHeight = activeHeight / zoom;
    const double streamAspect = static_cast<double>(sourceWidth) / sourceHeight;
    if (cropWidth / cropHeight > streamAspect) cropWidth = cropHeight * streamAspect;
    else cropHeight = cropWidth / streamAspect;
    const double cropLeft = activeArray.left + (activeWidth - cropWidth) * 0.5;
    const double cropTop = activeArray.top + (activeHeight - cropHeight) * 0.5;
    result.sensorX = std::clamp(static_cast<int>(std::lround(cropLeft + result.sourceU * cropWidth)),
                                activeArray.left, activeArray.right - 1);
    result.sensorY = std::clamp(static_cast<int>(std::lround(cropTop + result.sourceV * cropHeight)),
                                activeArray.top, activeArray.bottom - 1);
    result.inside = true;
    return result;
}
