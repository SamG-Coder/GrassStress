#pragma once

#include <cstdint>
#include <string>

struct ID3D12CommandQueue;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct IDXGIFactory;
struct IDXGISwapChain;

namespace dense {

enum class DlssQuality : std::uint32_t {
    Off = 0,
    Quality,
    Balanced,
    Performance,
    UltraPerformance,
    Dlaa
};

enum class FrameGenMode : std::uint32_t {
    Off = 0,
    X2,
    X4,
    X6,
    Dynamic
};

struct CameraBasis {
    float eye[3]{};
    float forward[3]{0, 0, 1};
    float right[3]{1, 0, 0};
    float up[3]{0, 1, 0};
    float tanHalf = 0.488f;
    float aspect = 1.777f;
    float jitter[2]{};
    float time = 0.0f;
};

struct StreamlineStatus {
    bool loaded = false;
    bool reflex = false;
    bool dlss = false;
    bool rayReconstruction = false;
    bool frameGeneration = false;
    bool dynamicMfg = false;
    bool taau = false;
    DlssQuality quality = DlssQuality::Off;
    FrameGenMode frameGen = FrameGenMode::Off;
    std::uint32_t renderWidth = 0;
    std::uint32_t renderHeight = 0;
    std::uint32_t displayWidth = 0;
    std::uint32_t displayHeight = 0;
    std::uint32_t mfgMultiplier = 1;
    std::uint32_t presentedFrames = 1;
    char label[48]{};
};

class StreamlineHost {
public:
    StreamlineHost();
    ~StreamlineHost();
    StreamlineHost(const StreamlineHost&) = delete;
    StreamlineHost& operator=(const StreamlineHost&) = delete;

    bool startup(const wchar_t* pluginDirectory);
    bool setDevice(ID3D12Device* device);
    bool upgradeFactory(IDXGIFactory** factory);
    bool upgradeSwapChain(IDXGISwapChain** swap);
    bool configure(std::uint32_t displayWidth, std::uint32_t displayHeight,
                   DlssQuality quality, FrameGenMode frameGen);
    void shutdown();

    bool beginFrame(std::uint32_t frameIndex);
    void markerSimulationStart();
    void markerSimulationEnd();
    void markerRenderStart();
    void markerRenderEnd();
    void markerPresentStart();
    void markerPresentEnd();

    void setCamera(const CameraBasis& current, const CameraBasis& previous, bool reset);
    bool evaluateUpscale(ID3D12GraphicsCommandList* commands,
                         ID3D12Resource* hdrColor,
                         ID3D12Resource* linearDepth,
                         ID3D12Resource* motionVectors,
                         ID3D12Resource* normalRough,
                         ID3D12Resource* diffuseAlbedo,
                         ID3D12Resource* specularAlbedo,
                         ID3D12Resource* outputColor,
                         std::uint32_t colorState,
                         std::uint32_t depthState,
                         std::uint32_t motionState,
                         std::uint32_t extraState,
                         std::uint32_t outputState);
    void tagFrameGeneration(ID3D12Resource* hudless,
                            ID3D12Resource* uiColor,
                            std::uint32_t hudlessState,
                            std::uint32_t uiState);
    void queryPresentedFrames();

    const StreamlineStatus& status() const { return status_; }
    bool upscaleActive() const {
        return status_.rayReconstruction || status_.dlss || status_.taau;
    }

    void setQuality(DlssQuality quality);
    void setFrameGen(FrameGenMode mode);
    DlssQuality quality() const { return requestedQuality_; }
    FrameGenMode frameGen() const { return requestedFrameGen_; }

private:
    struct Impl;
    Impl* impl_{};
    StreamlineStatus status_{};
    DlssQuality requestedQuality_ = DlssQuality::Quality;
    FrameGenMode requestedFrameGen_ = FrameGenMode::Dynamic;
};

void copyHaltonJitter(std::uint32_t index, float* outX, float* outY);

} // namespace dense
