#pragma once
#include <cstdint>

namespace dense {

struct TensorDenoiseStatus {
    bool cuda = false;
    bool optix = false;
    char label[80]{};
};

bool initTensorDenoiser();
void shutdownTensorDenoiser();
const TensorDenoiseStatus& tensorDenoiseStatus();
// Beauty RGBA32F in/out. Hits tensor cores through the OptiX denoiser.
bool tensorDenoiseRgba(const float* input, float* output,
                       std::uint32_t width, std::uint32_t height);

}
