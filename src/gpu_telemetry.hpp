#pragma once

namespace dense {

struct GpuTelemetry {
    bool valid = false;
    float temperatureC = 0.0f;
    float utilizationPercent = 0.0f;
    float powerWatts = 0.0f;
    float clockMHz = 0.0f;
    float vramUsedGiB = 0.0f;
    float vramTotalGiB = 0.0f;
};

bool initializeGpuTelemetry();
void shutdownGpuTelemetry();
GpuTelemetry sampleGpuTelemetry();

} // namespace dense
