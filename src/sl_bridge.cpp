#define SL_BRIDGE_EXPORTS
#include "sl_bridge.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <sl.h>
#include <sl_dlss.h>
#include <sl_dlss_d.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

namespace {

const sl::ViewportHandle kViewport{0};

sl::DLSSMode toDlssMode(uint32_t quality) {
    switch (quality) {
    case 1: return sl::DLSSMode::eMaxQuality;
    case 2: return sl::DLSSMode::eBalanced;
    case 3: return sl::DLSSMode::eMaxPerformance;
    case 4: return sl::DLSSMode::eUltraPerformance;
    case 5: return sl::DLSSMode::eDLAA;
    default: return sl::DLSSMode::eOff;
    }
}

const char* qualityLabel(uint32_t quality) {
    switch (quality) {
    case 1: return "QUALITY";
    case 2: return "BALANCED";
    case 3: return "PERF";
    case 4: return "ULTRA";
    case 5: return "DLAA";
    default: return "OFF";
    }
}

struct Mat4 { float m[4][4]{}; };

Mat4 identity() {
    Mat4 r{};
    r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
    return r;
}

Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] +
                        a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}

bool invert(const Mat4& in, Mat4& out) {
    float a[4][8]{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) a[i][j] = in.m[i][j];
        a[i][i + 4] = 1.0f;
    }
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        float best = fabsf(a[col][col]);
        for (int row = col + 1; row < 4; ++row) {
            const float v = fabsf(a[row][col]);
            if (v > best) { best = v; pivot = row; }
        }
        if (best < 1e-12f) return false;
        if (pivot != col)
            for (int j = 0; j < 8; ++j) std::swap(a[col][j], a[pivot][j]);
        const float inv = 1.0f / a[col][col];
        for (int j = 0; j < 8; ++j) a[col][j] *= inv;
        for (int row = 0; row < 4; ++row) {
            if (row == col) continue;
            const float f = a[row][col];
            for (int j = 0; j < 8; ++j) a[row][j] -= f * a[col][j];
        }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out.m[i][j] = a[i][j + 4];
    return true;
}

void toSl(const Mat4& in, sl::float4x4& out) {
    for (int i = 0; i < 4; ++i)
        out.setRow(static_cast<uint32_t>(i),
                   sl::float4(in.m[i][0], in.m[i][1], in.m[i][2], in.m[i][3]));
}

Mat4 worldToView(const SlbCamera& c) {
    Mat4 m = identity();
    m.m[0][0] = c.right[0]; m.m[1][0] = c.right[1]; m.m[2][0] = c.right[2];
    m.m[0][1] = c.up[0];    m.m[1][1] = c.up[1];    m.m[2][1] = c.up[2];
    m.m[0][2] = c.forward[0]; m.m[1][2] = c.forward[1]; m.m[2][2] = c.forward[2];
    m.m[3][0] = -(c.right[0] * c.eye[0] + c.right[1] * c.eye[1] + c.right[2] * c.eye[2]);
    m.m[3][1] = -(c.up[0] * c.eye[0] + c.up[1] * c.eye[1] + c.up[2] * c.eye[2]);
    m.m[3][2] = -(c.forward[0] * c.eye[0] + c.forward[1] * c.eye[1] + c.forward[2] * c.eye[2]);
    return m;
}

Mat4 viewToClip(const SlbCamera& c) {
    constexpr float kNear = 0.02f;
    constexpr float kFar = 2200.0f;
    const float tanH = c.tanHalf > 1e-4f ? c.tanHalf : 1e-4f;
    const float aspect = c.aspect > 1e-4f ? c.aspect : 1e-4f;
    Mat4 m{};
    m.m[0][0] = 1.0f / (aspect * tanH);
    m.m[1][1] = 1.0f / tanH;
    m.m[2][2] = kFar / (kFar - kNear);
    m.m[2][3] = 1.0f;
    m.m[3][2] = -kNear * kFar / (kFar - kNear);
    return m;
}

sl::Resource makeTex(void* resource, uint32_t state) {
    sl::Resource out(sl::ResourceType::eTex2d, resource, state);
    if (resource) {
        const D3D12_RESOURCE_DESC desc = static_cast<ID3D12Resource*>(resource)->GetDesc();
        out.width = static_cast<uint32_t>(desc.Width);
        out.height = desc.Height;
        out.nativeFormat = static_cast<uint32_t>(desc.Format);
        out.mipLevels = desc.MipLevels;
        out.arrayLayers = desc.DepthOrArraySize;
    }
    return out;
}

void writeLabel(char* dest, size_t bytes, const char* text) {
    if (!dest || !bytes) return;
    size_t n = 0;
    if (text)
        while (text[n] && n + 1 < bytes) { dest[n] = text[n]; ++n; }
    dest[n] = 0;
}

struct Bridge {
    HMODULE module{};
    sl::FrameToken* frame{};
    PFun_slInit* slInit{};
    PFun_slShutdown* slShutdown{};
    PFun_slSetD3DDevice* slSetD3DDevice{};
    PFun_slUpgradeInterface* slUpgradeInterface{};
    PFun_slIsFeatureSupported* slIsFeatureSupported{};
    PFun_slGetFeatureFunction* slGetFeatureFunction{};
    PFun_slGetNewFrameToken* slGetNewFrameToken{};
    PFun_slSetConstants* slSetConstants{};
    PFun_slSetTagForFrame* slSetTagForFrame{};
    PFun_slEvaluateFeature* slEvaluateFeature{};
    PFun_slDLSSSetOptions* slDLSSSetOptions{};
    PFun_slDLSSGetOptimalSettings* slDLSSGetOptimalSettings{};
    PFun_slDLSSDSetOptions* slDLSSDSetOptions{};
    PFun_slDLSSDGetOptimalSettings* slDLSSDGetOptimalSettings{};
    PFun_slDLSSGSetOptions* slDLSSGSetOptions{};
    PFun_slDLSSGGetState* slDLSSGGetState{};
    PFun_slReflexSetOptions* slReflexSetOptions{};
    PFun_slReflexSleep* slReflexSleep{};
    PFun_slPCLSetMarker* slPCLSetMarker{};
    bool inited{};
    bool deviceSet{};
    wchar_t pluginDir[MAX_PATH]{};
    SlbStatus status{};
    uint32_t quality = 1;
    uint32_t frameGen = 4;

    bool feature(sl::Feature id, const char* name, void*& fn) {
        if (!slGetFeatureFunction) return false;
        return slGetFeatureFunction(id, name, fn) == sl::Result::eOk && fn;
    }
    bool supported(sl::Feature id) const {
        if (!slIsFeatureSupported) return false;
        sl::AdapterInfo info{};
        return slIsFeatureSupported(id, info) == sl::Result::eOk;
    }
};

Bridge g;

} // namespace

SLB_API int slb_startup(const wchar_t* pluginDirectory) {
    memset(&g, 0, sizeof(g));
    g.quality = 1;
    g.frameGen = 4;
    writeLabel(g.status.label, sizeof(g.status.label), "NATIVE");
    if (!pluginDirectory || !pluginDirectory[0]) return 0;
    wcsncpy_s(g.pluginDir, pluginDirectory, _TRUNCATE);
    wchar_t path[MAX_PATH]{};
    swprintf_s(path, L"%s\\sl.interposer.dll", g.pluginDir);
    g.module = LoadLibraryW(path);
    if (!g.module) return 0;
    auto grab = [](const char* name) -> void* {
        return reinterpret_cast<void*>(GetProcAddress(g.module, name));
    };
    g.slInit = reinterpret_cast<PFun_slInit*>(grab("slInit"));
    g.slShutdown = reinterpret_cast<PFun_slShutdown*>(grab("slShutdown"));
    g.slSetD3DDevice = reinterpret_cast<PFun_slSetD3DDevice*>(grab("slSetD3DDevice"));
    g.slUpgradeInterface = reinterpret_cast<PFun_slUpgradeInterface*>(grab("slUpgradeInterface"));
    g.slIsFeatureSupported = reinterpret_cast<PFun_slIsFeatureSupported*>(grab("slIsFeatureSupported"));
    g.slGetFeatureFunction = reinterpret_cast<PFun_slGetFeatureFunction*>(grab("slGetFeatureFunction"));
    g.slGetNewFrameToken = reinterpret_cast<PFun_slGetNewFrameToken*>(grab("slGetNewFrameToken"));
    g.slSetConstants = reinterpret_cast<PFun_slSetConstants*>(grab("slSetConstants"));
    g.slSetTagForFrame = reinterpret_cast<PFun_slSetTagForFrame*>(grab("slSetTagForFrame"));
    g.slEvaluateFeature = reinterpret_cast<PFun_slEvaluateFeature*>(grab("slEvaluateFeature"));
    if (!g.slInit || !g.slSetD3DDevice || !g.slGetNewFrameToken || !g.slSetConstants ||
        !g.slSetTagForFrame || !g.slEvaluateFeature || !g.slGetFeatureFunction) {
        FreeLibrary(g.module);
        g.module = nullptr;
        return 0;
    }
    const sl::Feature features[] = {
        sl::kFeatureDLSS, sl::kFeatureDLSS_RR, sl::kFeatureDLSS_G,
        sl::kFeatureReflex, sl::kFeaturePCL
    };
    const wchar_t* paths[] = {g.pluginDir};
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = sl::LogLevel::eDefault;
    pref.pathsToPlugins = paths;
    pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = g.pluginDir;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                 sl::PreferenceFlags::eUseManualHooking |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = static_cast<uint32_t>(sizeof(features) / sizeof(features[0]));
    pref.applicationId = 0x47525353;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "0.1.0";
    pref.projectId = "7c3e1d2a-5080-4b1e-9c77-10mgrasspt01";
    pref.renderAPI = sl::RenderAPI::eD3D12;
    if (g.slInit(pref, sl::kSDKVersion) != sl::Result::eOk) {
        FreeLibrary(g.module);
        g.module = nullptr;
        return 0;
    }
    g.inited = true;
    g.status.loaded = 1;
    return 1;
}

SLB_API int slb_set_device(void* device) {
    if (!g.inited || !device || !g.slSetD3DDevice) return 0;
    if (g.slSetD3DDevice(device) != sl::Result::eOk) return 0;
    g.deviceSet = true;
    void* fn = nullptr;
    if (g.supported(sl::kFeatureDLSS) && g.feature(sl::kFeatureDLSS, "slDLSSSetOptions", fn)) {
        g.slDLSSSetOptions = reinterpret_cast<PFun_slDLSSSetOptions*>(fn);
        fn = nullptr;
        g.feature(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", fn);
        g.slDLSSGetOptimalSettings = reinterpret_cast<PFun_slDLSSGetOptimalSettings*>(fn);
        g.status.dlss = g.slDLSSSetOptions && g.slDLSSGetOptimalSettings;
    }
    fn = nullptr;
    if (g.supported(sl::kFeatureDLSS_RR) && g.feature(sl::kFeatureDLSS_RR, "slDLSSDSetOptions", fn)) {
        g.slDLSSDSetOptions = reinterpret_cast<PFun_slDLSSDSetOptions*>(fn);
        fn = nullptr;
        g.feature(sl::kFeatureDLSS_RR, "slDLSSDGetOptimalSettings", fn);
        g.slDLSSDGetOptimalSettings = reinterpret_cast<PFun_slDLSSDGetOptimalSettings*>(fn);
        g.status.rayReconstruction = g.slDLSSDSetOptions && g.slDLSSDGetOptimalSettings;
    }
    fn = nullptr;
    if (g.supported(sl::kFeatureDLSS_G) && g.feature(sl::kFeatureDLSS_G, "slDLSSGSetOptions", fn)) {
        g.slDLSSGSetOptions = reinterpret_cast<PFun_slDLSSGSetOptions*>(fn);
        fn = nullptr;
        g.feature(sl::kFeatureDLSS_G, "slDLSSGGetState", fn);
        g.slDLSSGGetState = reinterpret_cast<PFun_slDLSSGGetState*>(fn);
        g.status.frameGeneration = g.slDLSSGSetOptions != nullptr;
    }
    fn = nullptr;
    if (g.supported(sl::kFeatureReflex) && g.feature(sl::kFeatureReflex, "slReflexSetOptions", fn)) {
        g.slReflexSetOptions = reinterpret_cast<PFun_slReflexSetOptions*>(fn);
        fn = nullptr;
        g.feature(sl::kFeatureReflex, "slReflexSleep", fn);
        g.slReflexSleep = reinterpret_cast<PFun_slReflexSleep*>(fn);
        g.status.reflex = g.slReflexSetOptions != nullptr;
    }
    fn = nullptr;
    if (g.supported(sl::kFeaturePCL) && g.feature(sl::kFeaturePCL, "slPCLSetMarker", fn))
        g.slPCLSetMarker = reinterpret_cast<PFun_slPCLSetMarker*>(fn);
    if (g.status.reflex && g.slReflexSetOptions) {
        sl::ReflexOptions options{};
        options.mode = sl::ReflexMode::eLowLatency;
        g.slReflexSetOptions(options);
    }
    return 1;
}

SLB_API int slb_upgrade_swap(void** swap) {
    if (!g.inited || !g.slUpgradeInterface || !swap || !*swap) return 0;
    if (g.slUpgradeInterface(swap) != sl::Result::eOk) return 0;
    return 1;
}

SLB_API int slb_configure(uint32_t displayWidth, uint32_t displayHeight,
                          uint32_t quality, uint32_t frameGen, SlbStatus* out) {
    g.quality = quality;
    g.frameGen = frameGen;
    g.status.displayWidth = displayWidth;
    g.status.displayHeight = displayHeight;
    g.status.renderWidth = displayWidth;
    g.status.renderHeight = displayHeight;
    g.status.quality = 0;
    g.status.frameGen = 0;
    g.status.mfgMultiplier = 1;
    g.status.presentedFrames = 1;
    writeLabel(g.status.label, sizeof(g.status.label), "NATIVE");
    if (!g.deviceSet) {
        if (out) *out = g.status;
        return 0;
    }
    const sl::DLSSMode mode = toDlssMode(quality);
    if (mode != sl::DLSSMode::eOff && g.status.rayReconstruction &&
        g.slDLSSDGetOptimalSettings && g.slDLSSDSetOptions) {
        sl::DLSSDOptions options{};
        options.mode = mode;
        options.outputWidth = displayWidth;
        options.outputHeight = displayHeight;
        options.colorBuffersHDR = sl::Boolean::eTrue;
        options.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
        options.qualityPreset = sl::DLSSDPreset::ePresetE;
        options.balancedPreset = sl::DLSSDPreset::ePresetE;
        options.performancePreset = sl::DLSSDPreset::ePresetE;
        sl::DLSSDOptimalSettings settings{};
        if (g.slDLSSDGetOptimalSettings(options, settings) == sl::Result::eOk &&
            settings.optimalRenderWidth && settings.optimalRenderHeight) {
            g.status.renderWidth = settings.optimalRenderWidth;
            g.status.renderHeight = settings.optimalRenderHeight;
        }
        if (g.slDLSSDSetOptions(kViewport, options) == sl::Result::eOk)
            g.status.quality = quality;
        else
            g.status.rayReconstruction = 0;
    }
    if (mode != sl::DLSSMode::eOff && g.status.quality == 0 &&
        g.status.dlss && g.slDLSSGetOptimalSettings && g.slDLSSSetOptions) {
        sl::DLSSOptions options{};
        options.mode = mode;
        options.outputWidth = displayWidth;
        options.outputHeight = displayHeight;
        options.colorBuffersHDR = sl::Boolean::eTrue;
        options.useAutoExposure = sl::Boolean::eTrue;
        options.qualityPreset = sl::DLSSPreset::ePresetK;
        sl::DLSSOptimalSettings settings{};
        if (g.slDLSSGetOptimalSettings(options, settings) == sl::Result::eOk &&
            settings.optimalRenderWidth && settings.optimalRenderHeight) {
            g.status.renderWidth = settings.optimalRenderWidth;
            g.status.renderHeight = settings.optimalRenderHeight;
        }
        if (g.slDLSSSetOptions(kViewport, options) == sl::Result::eOk)
            g.status.quality = quality;
    }
    if (g.status.frameGeneration && g.slDLSSGSetOptions) {
        sl::DLSSGOptions options{};
        options.numBackBuffers = 2;
        options.mvecDepthWidth = g.status.renderWidth;
        options.mvecDepthHeight = g.status.renderHeight;
        options.colorWidth = displayWidth;
        options.colorHeight = displayHeight;
        options.colorBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        options.mvecBufferFormat = DXGI_FORMAT_R16G16_FLOAT;
        options.depthBufferFormat = DXGI_FORMAT_R32_FLOAT;
        options.hudLessBufferFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        options.uiBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        options.enableUserInterfaceRecomposition = sl::Boolean::eTrue;
        sl::DLSSGState state{};
        if (g.slDLSSGGetState) g.slDLSSGGetState(kViewport, state, nullptr);
        g.status.dynamicMfg = state.bIsDynamicMFGSupported == sl::Boolean::eTrue;
        const uint32_t maxGen = state.numFramesToGenerateMax ? state.numFramesToGenerateMax : 1u;
        options.mode = sl::DLSSGMode::eOff;
        options.numFramesToGenerate = 1;
        if (frameGen == 4 && g.status.dynamicMfg) {
            options.mode = sl::DLSSGMode::eDynamic;
            options.numFramesToGenerate = maxGen;
            g.status.frameGen = 4;
            g.status.mfgMultiplier = maxGen + 1u;
        } else if (frameGen >= 1 && frameGen <= 3) {
            uint32_t gen = frameGen == 1 ? 1u : (frameGen == 2 ? 3u : 5u);
            if (gen > maxGen) gen = maxGen;
            options.mode = sl::DLSSGMode::eOn;
            options.numFramesToGenerate = gen;
            g.status.frameGen = frameGen;
            g.status.mfgMultiplier = gen + 1u;
        }
        if (options.mode != sl::DLSSGMode::eOff && !g.status.reflex)
            options.mode = sl::DLSSGMode::eOff;
        if (g.slDLSSGSetOptions(kViewport, options) != sl::Result::eOk) {
            g.status.frameGen = 0;
            g.status.mfgMultiplier = 1;
        }
    }
    if (g.status.quality == 0)
        writeLabel(g.status.label, sizeof(g.status.label), "NATIVE");
    else {
        char label[48]{};
        const char* kind = g.status.rayReconstruction ? "RR" : "SR";
        sprintf_s(label, "DLSS %s %s", kind, qualityLabel(g.status.quality));
        if (g.status.frameGen) {
            char extra[20]{};
            if (g.status.frameGen == 4) sprintf_s(extra, " DYN MFG");
            else sprintf_s(extra, " %ux MFG", g.status.mfgMultiplier);
            strcat_s(label, extra);
        }
        writeLabel(g.status.label, sizeof(g.status.label), label);
    }
    if (out) *out = g.status;
    return 1;
}

SLB_API int slb_begin_frame(uint32_t frameIndex) {
    if (!g.inited || !g.slGetNewFrameToken) return 0;
    if (g.slGetNewFrameToken(g.frame, &frameIndex) != sl::Result::eOk) return 0;
    if (g.status.reflex && g.slReflexSleep && g.frame) g.slReflexSleep(*g.frame);
    return g.frame != nullptr;
}

SLB_API void slb_marker(int which) {
    if (!g.slPCLSetMarker || !g.frame) return;
    sl::PCLMarker marker = sl::PCLMarker::eSimulationStart;
    switch (which) {
    case 1: marker = sl::PCLMarker::eSimulationEnd; break;
    case 2: marker = sl::PCLMarker::eRenderSubmitStart; break;
    case 3: marker = sl::PCLMarker::eRenderSubmitEnd; break;
    case 4: marker = sl::PCLMarker::ePresentStart; break;
    case 5: marker = sl::PCLMarker::ePresentEnd; break;
    default: break;
    }
    g.slPCLSetMarker(marker, *g.frame);
}

SLB_API void slb_set_camera(const SlbCamera* current, const SlbCamera* previous, int reset) {
    if (!g.slSetConstants || !g.frame || !current || !previous) return;
    const Mat4 view = worldToView(*current);
    const Mat4 proj = viewToClip(*current);
    const Mat4 viewProj = mul(view, proj);
    const Mat4 prevViewProj = mul(worldToView(*previous), viewToClip(*previous));
    Mat4 invProj{}, invViewProj{}, clipToPrev{}, prevToClip{};
    invert(proj, invProj);
    invert(viewProj, invViewProj);
    clipToPrev = mul(invViewProj, prevViewProj);
    invert(clipToPrev, prevToClip);
    sl::Constants constants{};
    toSl(proj, constants.cameraViewToClip);
    toSl(invProj, constants.clipToCameraView);
    toSl(identity(), constants.clipToLensClip);
    toSl(clipToPrev, constants.clipToPrevClip);
    toSl(prevToClip, constants.prevClipToClip);
    constants.jitterOffset = sl::float2(current->jitter[0], current->jitter[1]);
    constants.mvecScale = sl::float2(1.0f, 1.0f);
    constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    constants.cameraPos = sl::float3(current->eye[0], current->eye[1], current->eye[2]);
    constants.cameraUp = sl::float3(current->up[0], current->up[1], current->up[2]);
    constants.cameraRight = sl::float3(current->right[0], current->right[1], current->right[2]);
    constants.cameraFwd = sl::float3(current->forward[0], current->forward[1], current->forward[2]);
    constants.cameraNear = 0.02f;
    constants.cameraFar = 2200.0f;
    constants.cameraFOV = 2.0f * atanf(current->tanHalf > 1e-4f ? current->tanHalf : 1e-4f);
    constants.cameraAspectRatio = current->aspect;
    constants.depthInverted = sl::Boolean::eFalse;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.orthographicProjection = sl::Boolean::eFalse;
    constants.motionVectorsDilated = sl::Boolean::eFalse;
    constants.motionVectorsJittered = sl::Boolean::eFalse;
    g.slSetConstants(constants, *g.frame, kViewport);
    if (g.status.rayReconstruction && g.slDLSSDSetOptions && g.status.quality) {
        sl::DLSSDOptions options{};
        options.mode = toDlssMode(g.status.quality);
        options.outputWidth = g.status.displayWidth;
        options.outputHeight = g.status.displayHeight;
        options.colorBuffersHDR = sl::Boolean::eTrue;
        options.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
        Mat4 invView{};
        invert(view, invView);
        toSl(view, options.worldToCameraView);
        toSl(invView, options.cameraViewToWorld);
        g.slDLSSDSetOptions(kViewport, options);
    }
}

SLB_API int slb_evaluate(void* commandList,
                         void* hdrColor, void* linearDepth, void* motionVectors,
                         void* normalRough, void* diffuseAlbedo, void* specularAlbedo,
                         void* outputColor,
                         uint32_t colorState, uint32_t depthState, uint32_t motionState,
                         uint32_t extraState, uint32_t outputState,
                         uint32_t renderWidth, uint32_t renderHeight,
                         uint32_t displayWidth, uint32_t displayHeight) {
    if (!g.slEvaluateFeature || !g.frame || !commandList) return 0;
    sl::Extent input{}; input.width = renderWidth; input.height = renderHeight;
    sl::Extent output{}; output.width = displayWidth; output.height = displayHeight;
    sl::Resource colorIn = makeTex(hdrColor, colorState);
    sl::Resource colorOut = makeTex(outputColor, outputState);
    sl::Resource depth = makeTex(linearDepth, depthState);
    sl::Resource mvec = makeTex(motionVectors, motionState);
    sl::Resource normals = makeTex(normalRough, extraState);
    sl::Resource diffuse = makeTex(diffuseAlbedo, extraState);
    sl::Resource specular = makeTex(specularAlbedo, extraState);
    sl::ResourceTag tags[7] = {
        sl::ResourceTag{&colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &input},
        sl::ResourceTag{&colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &output},
        sl::ResourceTag{&depth, sl::kBufferTypeLinearDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &input},
        sl::ResourceTag{&mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &input},
        sl::ResourceTag{&normals, sl::kBufferTypeNormalRoughness, sl::ResourceLifecycle::eValidUntilEvaluate, &input},
        sl::ResourceTag{&diffuse, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eValidUntilEvaluate, &input},
        sl::ResourceTag{&specular, sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eValidUntilEvaluate, &input}
    };
    if (g.slSetTagForFrame(*g.frame, kViewport, tags, 7, commandList) != sl::Result::eOk)
        return 0;
    const sl::BaseStructure* inputs[] = {&kViewport};
    const sl::Feature feature = (g.status.rayReconstruction && g.status.quality)
        ? sl::kFeatureDLSS_RR : sl::kFeatureDLSS;
    return g.slEvaluateFeature(feature, *g.frame, inputs, 1, commandList) == sl::Result::eOk;
}

SLB_API void slb_tag_fg(void* hudless, void* uiColor,
                        uint32_t hudlessState, uint32_t uiState,
                        uint32_t displayWidth, uint32_t displayHeight) {
    if (!g.status.frameGen || !g.slSetTagForFrame || !g.frame) return;
    sl::Extent display{}; display.width = displayWidth; display.height = displayHeight;
    sl::Resource hud = makeTex(hudless, hudlessState);
    sl::Resource ui = makeTex(uiColor, uiState);
    sl::ResourceTag tags[2] = {
        sl::ResourceTag{&hud, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &display},
        sl::ResourceTag{&ui, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent, &display}
    };
    g.slSetTagForFrame(*g.frame, kViewport, tags, 2, nullptr);
}

SLB_API void slb_query(SlbStatus* out) {
    if (g.status.frameGen && g.slDLSSGGetState) {
        sl::DLSSGState state{};
        if (g.slDLSSGGetState(kViewport, state, nullptr) == sl::Result::eOk &&
            state.numFramesActuallyPresented)
            g.status.presentedFrames = state.numFramesActuallyPresented;
        else
            g.status.presentedFrames = g.status.mfgMultiplier;
    } else {
        g.status.presentedFrames = 1;
    }
    if (out) *out = g.status;
}

SLB_API void slb_shutdown(void) {
    if (g.inited && g.slShutdown) g.slShutdown();
    if (g.module) FreeLibrary(g.module);
    memset(&g, 0, sizeof(g));
}
