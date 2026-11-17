#include "ControlClient.h"
#include "SessionSettings.h"
#include "DeviceDiscoveryRetry.h"
#include "DecoderAccessUnitQueue.h"
#include "Metrics.h"
#include "ViewTransform.h"
#include "PreviewGeometry.h"
#include "RecordingTimeline.h"
#include "RecordingClock.h"
#include "RecordingOrientation.h"
#include "VideoTransport.h"
#include "../shared/ObsFrameProtocol.h"
#include "../shared/VirtualCameraFrameProtocol.h"

#include <iostream>

#include <wels/codec_api.h>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        SessionSettings session;
        session.videoMode = 1;
        session.orientation = 5;
        session.scale = 1;
        session.audioOffsetMs = -145;
        session.audioDeviceId = "endpoint-\\device-\"input\"";
        session.hasCameraState = true;
        session.camera = {false, 640, 8'000'000, true, 2.5, false, 1.4, 2.2, 1.75};
        const auto restoredSession = DecodeSessionSettings(EncodeSessionSettings(session));
        Require(restoredSession && restoredSession->videoMode == 1 && restoredSession->orientation == 5 &&
                restoredSession->scale == 1 && restoredSession->audioOffsetMs == -145 &&
                restoredSession->audioDeviceId == session.audioDeviceId && restoredSession->hasCameraState,
                "session settings round-trip preserves UI and stable audio endpoint identity");
        Require(!restoredSession->camera.autoExposure && restoredSession->camera.autoFocus &&
                !restoredSession->camera.autoWhiteBalance && restoredSession->camera.iso == 640 &&
                restoredSession->camera.exposureNs == 8'000'000 && restoredSession->camera.zoom == 1.75 &&
                restoredSession->camera.redGain == 1.4 && restoredSession->camera.blueGain == 2.2,
                "session settings preserve independent AE/AF/AWB modes and manual values");
        Require(!DecodeSessionSettings("{}").has_value() &&
                !DecodeSessionSettings("{\"schema\":999}").has_value(),
                "missing and unsupported preference schemas use application defaults");
        const auto damaged = DecodeSessionSettings(
            "{\"schema\":1,\"videoMode\":99,\"orientation\":-9,\"scale\":1e300,\"audioOffsetMs\":5001,\"zoom\":1e999}");
        Require(damaged && damaged->videoMode == 0 && damaged->orientation == 0 && damaged->scale == 0 &&
                damaged->audioOffsetMs == 0 && damaged->camera.zoom == 1.0,
                "invalid saved values cannot become unsafe indices or non-finite camera parameters");
        CameraCapabilities restoreCaps;
        restoreCaps.manualExposure = true;
        restoreCaps.manualFocus = false;
        restoreCaps.manualWhiteBalance = true;
        restoreCaps.isoMin = 100; restoreCaps.isoMax = 400;
        restoreCaps.exposureMinNs = 1000; restoreCaps.exposureMaxNs = 20'000'000;
        restoreCaps.focusMax = 0; restoreCaps.zoomMin = 1; restoreCaps.zoomMax = 1.5;
        auto limited = session.camera;
        limited.autoFocus = false;
        limited.exposureNs = 30'000'000;
        limited = RestoreSupportedCameraState(limited, restoreCaps);
        Require(limited.iso == 400 && limited.exposureNs == 20'000'000 && limited.autoFocus &&
                !limited.autoExposure && !limited.autoWhiteBalance && limited.focusDistance == 0 && limited.zoom == 1.5,
                "restoring to different camera capabilities clamps values and preserves independent supported modes");
        const std::wstring testKey = L"Software\\StratoCam\\Tests\\Session-" + std::to_wstring(GetCurrentProcessId());
        Require(SaveSessionSettings(session, testKey.c_str()), "save preference snapshot in the per-user registry");
        const auto registrySession = LoadSessionSettings(testKey.c_str());
        RegDeleteKeyW(HKEY_CURRENT_USER, testKey.c_str());
        Require(registrySession && EncodeSessionSettings(*registrySession) == EncodeSessionSettings(session),
                "registry persistence reloads the complete snapshot without loss");
        Require(!DeviceDiscoveryRetryDue(true, false, false, 2999, 3000),
                "USB discovery waits for its retry deadline");
        Require(DeviceDiscoveryRetryDue(true, false, false, 3000, 3000),
                "empty/offline device discovery retries when the phone becomes available");
        Require(!DeviceDiscoveryRetryDue(true, true, false, 6000, 3000),
                "discovery retries cannot overlap an active connection attempt");
        Require(!DeviceDiscoveryRetryDue(false, false, false, 6000, 3000),
                "Stop cancels USB discovery retries");
        Require(!DeviceDiscoveryRetryDue(true, false, true, 6000, 3000) &&
                    !DeviceDiscoveryRetryDue(true, false, false, 6000, 0),
                "a prepared or streaming session does not restart discovery");
        const auto summary = Summarize({4.0, 1.0, 3.0, 2.0});
        Require(summary.samples == 4, "summary sample count");
        Require(std::abs(summary.averageMs - 2.5) < 0.001, "summary average");
        Require(std::abs(summary.medianMs - 2.5) < 0.001, "summary median");
        Require(std::abs(summary.maximumMs - 4.0) < 0.001, "summary maximum");
        Require(std::abs(summary.p95Ms - 3.0) < 0.001 && std::abs(summary.p99Ms - 3.0) < 0.001,
                "summary tail percentiles");
        LatencyCollector latencyCollector;
        FrameTiming unknownCapture{};
        unknownCapture.windowsReceiveNs = 1'000'000.0;
        unknownCapture.decoderOutputNs = 2'000'000.0;
        unknownCapture.renderSubmitNs = 3'000'000.0;
        unknownCapture.processingLatencyMs = 2.0;
        latencyCollector.Add(unknownCapture);
        Require(!latencyCollector.Get().latestLiveFrameAgeMs.has_value(),
                "missing capture timestamp reports unknown live latency");
        FrameTiming knownCapture = unknownCapture;
        knownCapture.liveFrameAgeMs = 207.5;
        knownCapture.encoderToWindowsReceiveMs = 3.9;
        knownCapture.decodeCallMs = 8.5;
        latencyCollector.Add(knownCapture);
        const auto latencySnapshot = latencyCollector.Get();
        Require(latencySnapshot.latestLiveFrameAgeMs &&
                    std::abs(*latencySnapshot.latestLiveFrameAgeMs - 207.5) < 0.001,
                "live latency tracks current presented frame age");

        const std::string json = R"({"ok":true,"state":{"autoExposure":false,"iso":200},"capabilities":{"zoomMin":1.0,"zoomMax":8.0}})";
        Require(JsonLite::Boolean(json, "ok") == true, "JSON boolean");
        const auto state = JsonLite::Object(json, "state");
        Require(state.has_value(), "JSON nested state");
        Require(JsonLite::Boolean(*state, "autoExposure") == false, "JSON nested boolean");
        Require(JsonLite::Number(*state, "iso").value_or(0) == 200, "JSON number");
        CameraState cameraState{};
        cameraState = ToggleAutomaticControl(cameraState, AutomaticControl::Focus);
        Require(cameraState.autoExposure && !cameraState.autoFocus && cameraState.autoWhiteBalance,
                "manual focus leaves AE and AWB automatic");
        cameraState = ToggleAutomaticControl(cameraState, AutomaticControl::Focus);
        Require(cameraState.autoFocus, "manual-to-auto focus transition restores click-to-focus eligibility");
        cameraState = ToggleAutomaticControl(cameraState, AutomaticControl::Exposure);
        cameraState = ToggleAutomaticControl(cameraState, AutomaticControl::Focus);
        Require(!cameraState.autoExposure && !cameraState.autoFocus && cameraState.autoWhiteBalance,
                "manual exposure and focus remain independent");
        cameraState = ToggleAutomaticControl(cameraState, AutomaticControl::Focus);
        Require(!cameraState.autoExposure && cameraState.autoFocus, "AF can return to auto while AE remains manual");
        cameraState = ToggleAutomaticControl(cameraState, AutomaticControl::Exposure);
        cameraState = ToggleAutomaticControl(cameraState, AutomaticControl::Focus);
        cameraState = ToggleAutomaticControl(cameraState, AutomaticControl::WhiteBalance);
        Require(cameraState.autoExposure && !cameraState.autoFocus && !cameraState.autoWhiteBalance,
                "manual focus and white balance remain independent");
        cameraState = ToggleAutomaticControl(cameraState, AutomaticControl::Focus);
        cameraState = ToggleAutomaticControl(cameraState, AutomaticControl::WhiteBalance);
        Require(cameraState.autoExposure && cameraState.autoFocus && cameraState.autoWhiteBalance,
                "independent controls return to all-auto");
        for (int iteration = 0; iteration < 10; ++iteration) {
            for (const auto control : {AutomaticControl::Exposure, AutomaticControl::Focus,
                                       AutomaticControl::WhiteBalance}) {
                cameraState = ToggleAutomaticControl(cameraState, control);
                cameraState = ToggleAutomaticControl(cameraState, control);
            }
            Require(cameraState.autoExposure && cameraState.autoFocus && cameraState.autoWhiteBalance,
                    "repeated auto-manual-auto transitions preserve all-auto state");
        }
        const auto capabilities = JsonLite::Object(json, "capabilities");
        Require(capabilities && JsonLite::Number(*capabilities, "zoomMax").value_or(0) == 8.0, "JSON nested capabilities");
        OpenH264Version version{};
        WelsGetCodecVersionEx(&version);
        Require(version.uMajor == 2 && version.uMinor == 6, "OpenH264 static runtime");
        EncodedAccessUnit uncorrelatedFrame{};
        uncorrelatedFrame.flags = 0;
        uncorrelatedFrame.payload = std::make_shared<const std::vector<uint8_t>>(std::initializer_list<uint8_t>{0, 0, 0, 1, 0x65});
        uncorrelatedFrame.timing.frameNumber = -1;
        Require(ShouldAssignDecoderTimestamp(uncorrelatedFrame),
                "video decode must not depend on Camera2 timing correlation");
        uncorrelatedFrame.flags = kVideoFlagCodecConfig;
        Require(!ShouldAssignDecoderTimestamp(uncorrelatedFrame),
                "codec configuration is not assigned a frame timestamp");
        Require(std::wstring(VideoTransportStateLabel(VideoTransportState::Starting)) == L"●  STARTING" &&
                    std::wstring(VideoTransportStateLabel(VideoTransportState::Streaming)) == L"●  STREAMING" &&
                    std::wstring(VideoTransportStateLabel(VideoTransportState::Error)) == L"●  ERROR" &&
                    std::wstring(VideoTransportStateLabel(VideoTransportState::Stopped)) == L"●  STOPPED",
                "video transport UI states distinguish requested, confirmed, failed, and stopped");
        DecoderAccessUnitQueue decoderQueue(2);
        EncodedAccessUnit codecConfig{};
        codecConfig.flags = kVideoFlagCodecConfig;
        codecConfig.payload = std::make_shared<const std::vector<uint8_t>>(std::initializer_list<uint8_t>{0, 0, 0, 1, 0x67});
        Require(decoderQueue.Push(std::move(codecConfig)) == DecoderQueuePushResult::Accepted,
                "decoder queue accepts SPS/PPS");
        EncodedAccessUnit predicted1{};
        predicted1.payload = std::make_shared<const std::vector<uint8_t>>(std::initializer_list<uint8_t>{0, 0, 0, 1, 0x41});
        decoderQueue.Push(std::move(predicted1));
        EncodedAccessUnit predicted2{};
        predicted2.payload = std::make_shared<const std::vector<uint8_t>>(std::initializer_list<uint8_t>{0, 0, 0, 1, 0x41});
        Require(decoderQueue.Push(std::move(predicted2)) == DecoderQueuePushResult::BacklogDiscarded &&
                    decoderQueue.Empty(),
                "decoder backlog discards the complete dependent GOP");
        EncodedAccessUnit predicted3{};
        predicted3.payload = std::make_shared<const std::vector<uint8_t>>(std::initializer_list<uint8_t>{0, 0, 0, 1, 0x41});
        Require(decoderQueue.Push(std::move(predicted3)) == DecoderQueuePushResult::DroppedUntilKeyFrame,
                "dependent access units stay blocked after backlog recovery");
        EncodedAccessUnit keyFrame{};
        keyFrame.flags = kVideoFlagKeyFrame;
        keyFrame.payload = std::make_shared<const std::vector<uint8_t>>(std::initializer_list<uint8_t>{0, 0, 0, 1, 0x65});
        Require(decoderQueue.Push(std::move(keyFrame)) == DecoderQueuePushResult::RecoveredOnKeyFrame &&
                    decoderQueue.Size() == 2 && decoderQueue.GopRecoveries() == 1,
                "decoder resumes atomically with cached SPS/PPS and IDR");
        Require((decoderQueue.Pop().flags & kVideoFlagCodecConfig) != 0 &&
                    (decoderQueue.Pop().flags & kVideoFlagKeyFrame) != 0,
                "recovery ordering is SPS/PPS before IDR");
        RecordingTimeline recordingTimeline(30);
        Require(recordingTimeline.Normalize(9'000'000).value_or(-1) == 0,
                "recording PTS begins at zero without using wall-clock write time");
        Require(recordingTimeline.Normalize(9'033'333).value_or(-1) == 33'333,
                "recording preserves the MediaCodec presentation timestamp delta");
        Require(!recordingTimeline.Normalize(9'033'333).has_value(),
                "recording rejects duplicate presentation timestamps");
        Require(recordingTimeline.Normalize(9'066'667).value_or(-1) == 66'667 &&
                    recordingTimeline.DurationUs() == 100'000,
                "recording duration combines the source timeline with one final frame duration");
        Require(CommonPresentationUs(2'250'500'000, 2'000'000'000) == 250'500,
                "audio and video share a Windows monotonic recording origin");
        Require(AudioSampleTimelineWindowsNs(1'000'000'000, 48'000, 48'000) == 2'000'000'000,
                "WASAPI output samples form a monotonic audio timeline at the negotiated capture rate");
        Require(NormalizeRecordingRotation(0) == 0 && NormalizeRecordingRotation(90) == 90 &&
                    NormalizeRecordingRotation(180) == 180 && NormalizeRecordingRotation(270) == 270,
                "recording preserves supported display rotations");
        Require(NormalizeRecordingRotation(-90) == 270 && NormalizeRecordingRotation(450) == 90 &&
                    NormalizeRecordingRotation(17) == 0,
                "recording rotation is normalized and rejects unsupported angles");
        Require(AndroidCamObs::kSlotCount == 3 && AndroidCamObs::kMappingBytes > 37'000'000,
                "OBS shared-memory triple buffer capacity");
        AndroidCamObs::SharedHeader sharedHeader{};
        std::memcpy(sharedHeader.magic, AndroidCamObs::kMagic, sizeof(AndroidCamObs::kMagic));
        sharedHeader.version = AndroidCamObs::kProtocolVersion;
        sharedHeader.headerBytes = static_cast<uint32_t>(AndroidCamObs::kHeaderBytes);
        sharedHeader.slotCount = AndroidCamObs::kSlotCount;
        sharedHeader.slotStride = static_cast<uint32_t>(AndroidCamObs::kSlotStride);
        sharedHeader.maxWidth = AndroidCamObs::kMaxWidth;
        sharedHeader.maxHeight = AndroidCamObs::kMaxHeight;
        Require(AndroidCamObs::ValidHeader(&sharedHeader), "OBS shared-memory protocol header");
        Require(AndroidCamVirtualCamera::kSlotCount == 3 &&
                    AndroidCamVirtualCamera::kMappingBytes > 37'000'000,
                "virtual-camera shared-file triple buffer capacity");
        AndroidCamVirtualCamera::SharedHeader virtualHeader{};
        std::memcpy(virtualHeader.magic, AndroidCamVirtualCamera::kMagic,
                    sizeof(AndroidCamVirtualCamera::kMagic));
        virtualHeader.version = AndroidCamVirtualCamera::kProtocolVersion;
        virtualHeader.headerBytes = static_cast<uint32_t>(AndroidCamVirtualCamera::kHeaderBytes);
        virtualHeader.slotCount = AndroidCamVirtualCamera::kSlotCount;
        virtualHeader.slotStride = static_cast<uint32_t>(AndroidCamVirtualCamera::kSlotStride);
        virtualHeader.maxWidth = AndroidCamVirtualCamera::kMaxWidth;
        virtualHeader.maxHeight = AndroidCamVirtualCamera::kMaxHeight;
        Require(AndroidCamVirtualCamera::ValidHeader(&virtualHeader),
                "virtual-camera shared-file protocol header");
        Require(ResolvePreviewRotation(PreviewOrientationMode::Auto, 90, 0, 1920, 1080) == 90,
                "portrait auto rotation");
        Require(ResolvePreviewRotation(PreviewOrientationMode::Auto, 90, 90, 1920, 1080) == 0,
                "landscape auto rotation");
        Require(ResolvePreviewRotation(PreviewOrientationMode::Portrait, 90, 90, 1920, 1080) == 90,
                "forced portrait rotation");
        Require(ResolvePreviewRotation(PreviewOrientationMode::Rotate270, 90, 0, 1920, 1080) == 270,
                "manual 270 rotation");
        for (const auto [clientWidth, clientHeight] : {
                 std::pair{640, 360}, std::pair{720, 480}, std::pair{800, 600},
                 std::pair{900, 600}, std::pair{900, 480}, std::pair{1080, 800},
                 std::pair{1460, 920}, std::pair{1920, 1080},
                 std::pair{2560, 1080}, std::pair{1200, 1440}}) {
            for (const int rotation : {0, 90, 180, 270}) {
                const auto layout = CalculateWindowPreviewLayout(
                    clientWidth, clientHeight, 1920, 1080, rotation);
                Require(layout.previewX >= 20 && layout.previewY >= 84,
                        "preview container stays below header and inside left margin");
                Require(layout.previewX + layout.previewWidth <= layout.panelX - 20,
                        "preview container never crosses controls panel");
                Require(layout.previewY + layout.previewHeight <= layout.toolbarY - 10,
                        "preview container never crosses toolbar");
                Require(layout.toolbarY + 48 <= layout.metricsY,
                        "toolbar and metrics remain ordered");
                Require(layout.metricsY + layout.metricsHeight <= clientHeight - 20,
                        "responsive metrics fit below the preview on short displays");
                Require(layout.panelX + layout.panelWidth <= clientWidth - 20,
                        "controls panel fits inside the client area");
            }
        }
        for (const auto [viewWidth, viewHeight] : {
                 std::pair{320.0, 180.0}, std::pair{960.0, 720.0},
                 std::pair{1080.0, 1600.0}, std::pair{2200.0, 900.0}}) {
            for (const auto [sourceWidth, sourceHeight] : {
                     std::pair{1920, 1080}, std::pair{3840, 2160}}) {
                for (const int rotation : {0, 90, 180, 270}) {
                    for (const auto mode : {PreviewScaleMode::Fit, PreviewScaleMode::Fill,
                                            PreviewScaleMode::Actual}) {
                        const auto geometry = CalculatePreviewDisplayGeometry(
                            viewWidth, viewHeight, sourceWidth, sourceHeight, rotation, mode);
                        Require(geometry.valid, "preview display geometry is valid");
                        Require(geometry.imageLeft >= -0.001 && geometry.imageTop >= -0.001 &&
                                    geometry.imageLeft + geometry.imageWidth <= viewWidth + 0.001 &&
                                    geometry.imageTop + geometry.imageHeight <= viewHeight + 0.001,
                                "D3D viewport remains inside explicit preview destination");
                        Require(geometry.sampleScaleX > 0.0 && geometry.sampleScaleX <= 1.0 &&
                                    geometry.sampleScaleY > 0.0 && geometry.sampleScaleY <= 1.0,
                                "fill sampling crop remains normalized");
                    }
                }
            }
        }
        const SensorActiveArray active{0, 0, 4000, 3000};
        const auto center = MapPreviewPoint(540, 960, 1080, 1920, 1920, 1080, 90,
                                            PreviewScaleMode::Fit, active, 1.0);
        Require(center.inside && std::abs(center.sensorX - 2000) <= 1 && std::abs(center.sensorY - 1500) <= 1,
                "rotated center maps to sensor center");
        const auto letterbox = MapPreviewPoint(10, 500, 1600, 900, 1920, 1080, 90,
                                               PreviewScaleMode::Fit, active, 1.0);
        Require(!letterbox.inside, "fit letterbox click ignored");
        const auto topLeft90 = MapPreviewPoint(0, 0, 1080, 1920, 1920, 1080, 90,
                                              PreviewScaleMode::Fill, active, 1.0);
        Require(topLeft90.inside && topLeft90.sensorX == 0 && topLeft90.sensorY == 2625,
                "90 degree corner mapping");
        const auto topLeft0 = MapPreviewPoint(0, 0, 1920, 1080, 1920, 1080, 0,
                                             PreviewScaleMode::Fit, active, 1.0);
        const auto topLeft180 = MapPreviewPoint(0, 0, 1920, 1080, 1920, 1080, 180,
                                               PreviewScaleMode::Fit, active, 1.0);
        const auto topLeft270 = MapPreviewPoint(0, 0, 1080, 1920, 1920, 1080, 270,
                                               PreviewScaleMode::Fit, active, 1.0);
        Require(topLeft0.inside && topLeft0.sensorX == 0 && topLeft0.sensorY == 375,
                "0 degree corner mapping");
        Require(topLeft180.inside && topLeft180.sensorX == 3999 && topLeft180.sensorY == 2625,
                "180 degree corner mapping");
        Require(topLeft270.inside && topLeft270.sensorX == 3999 && topLeft270.sensorY == 375,
                "270 degree corner mapping");
        const auto fillCrop = MapPreviewPoint(0, 500, 1000, 1000, 1920, 1080, 0,
                                              PreviewScaleMode::Fill, active, 1.0);
        Require(fillCrop.inside && std::abs(fillCrop.sensorX - 875) <= 1 &&
                    std::abs(fillCrop.sensorY - 1500) <= 1,
                "fill crop maps only the visible source region");
        const auto actualBar = MapPreviewPoint(10, 10, 2560, 1440, 1920, 1080, 0,
                                               PreviewScaleMode::Actual, active, 1.0);
        Require(!actualBar.inside, "actual-size bar click ignored");
        const auto zoomed = MapPreviewPoint(0, 0, 1920, 1080, 1920, 1080, 0,
                                           PreviewScaleMode::Fit, active, 2.0);
        Require(zoomed.inside && zoomed.sensorX == 0 && zoomed.sensorY == 375,
                "CONTROL_ZOOM_RATIO uses post-zoom AF coordinates");
        const auto legacyZoom = MapPreviewPoint(0, 0, 1920, 1080, 1920, 1080, 0,
                                                PreviewScaleMode::Fit, active, 2.0, false);
        Require(legacyZoom.inside && legacyZoom.sensorX == 1000 && legacyZoom.sensorY == 938,
                "legacy crop-region zoom maps into active array");
        std::cout << "All native tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
