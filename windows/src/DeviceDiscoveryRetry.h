#pragma once

#include <cstdint>

// Only failed ADB discovery arms a retry. Successful preparation, Stop, and
// failures later in the video pipeline do not restart an intentionally stopped stream.
inline bool DeviceDiscoveryRetryDue(bool requested, bool connecting, bool running,
                                    uint64_t nowMs, uint64_t retryAtMs) {
    return requested && !connecting && !running && retryAtMs != 0 && nowMs >= retryAtMs;
}
