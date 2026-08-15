#include "streamline_host.hpp"

#include <d3d12.h>
#include <windows.h>

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include <nvsdk_ngx.h>
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <csetjmp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace dense {
namespace {

void writeLabel(char* dest, std::size_t bytes, const char* text) {
    if (!dest || bytes == 0) return;
    std::size_t n = 0;
    if (text)
        while (text[n] && n + 1 < bytes) {
            dest[n] = text[n];
            ++n;
        }
    dest[n] = 0;
}

void appendLog(const char* line) {
    CreateDirectoryW(L"C:\\StressTest\\video", nullptr);
    if (FILE* f = fopen("C:\\StressTest\\video\\ngx.log", "a")) {
        fputs(line, f);
        fputc('\n', f);
        fclose(f);
    }
}

thread_local jmp_buf g_ngxJmp;
thread_local bool g_ngxGuard = false;

LONG CALLBACK ngxVeh(EXCEPTION_POINTERS* info) {
    if (!g_ngxGuard || !info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION) {
        appendLog("trapped NGX access violation");
        longjmp(g_ngxJmp, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

template<class Fn>
bool ngxTrap(Fn&& fn) {
    static PVOID handle = AddVectoredExceptionHandler(1, ngxVeh);
    (void)handle;
    g_ngxGuard = true;
    if (setjmp(g_ngxJmp) != 0) {
        g_ngxGuard = false;
        return false;
    }
    fn();
    g_ngxGuard = false;
    return true;
}

void logf(const char* fmt, int a = 0, int b = 0) {
    char buf[256]{};
    std::snprintf(buf, sizeof(buf), fmt, a, b);
    appendLog(buf);
}

enum NgxResult : unsigned int {
    kNgxSuccess = 0x1,
    kNgxFail = 0xBAD00000
};

inline bool ngxOk(unsigned int r) { return (r & 0xFFF00000u) != kNgxFail; }

enum NgxFeature : unsigned int {
    kNgxSuperSampling = 1,
    kNgxFrameGeneration = 11,
    kNgxRayReconstruction = 13
};

enum NgxPerf : int {
    kNgxMaxPerf = 0,
    kNgxBalanced = 1,
    kNgxMaxQuality = 2,
    kNgxUltraPerf = 3,
    kNgxUltraQuality = 4,
    kNgxDlaa = 5
};

enum NgxEngine : int { kNgxEngineCustom = 0 };

struct NgxPathList {
    const wchar_t* const* Path;
    unsigned int Length;
};

struct NgxLoggingInfo {
    void* LoggingCallback;
    int MinimumLoggingLevel;
    bool DisableOtherLoggingSinks;
};

struct NgxFeatureCommonInfo {
    NgxPathList PathListInfo;
    void* InternalData;
    NgxLoggingInfo LoggingInfo;
};

struct NgxHandle;
struct NgxParameter;

typedef unsigned int (*P_InitProject)(const char*, int, const char*, const wchar_t*,
                                      ID3D12Device*, const NgxFeatureCommonInfo*, unsigned int);
typedef unsigned int (*P_Shutdown1)(ID3D12Device*);
typedef unsigned int (*P_GetCaps)(NgxParameter**);
typedef unsigned int (*P_AllocParams)(NgxParameter**);
typedef unsigned int (*P_DestroyParams)(NgxParameter*);
typedef unsigned int (*P_CreateFeature)(ID3D12GraphicsCommandList*, unsigned int,
                                        NgxParameter*, NgxHandle**);
typedef unsigned int (*P_ReleaseFeature)(NgxHandle*);
typedef unsigned int (*P_Evaluate)(ID3D12GraphicsCommandList*, const NgxHandle*,
                                   const NgxParameter*, void*);
typedef unsigned int (*P_Scratch)(unsigned int, const NgxParameter*, size_t*);
typedef void (*P_SetUI)(NgxParameter*, const char*, unsigned int);
typedef void (*P_SetI)(NgxParameter*, const char*, int);
typedef void (*P_SetF)(NgxParameter*, const char*, float);
typedef void (*P_SetRes)(NgxParameter*, const char*, ID3D12Resource*);
typedef void (*P_SetPtr)(NgxParameter*, const char*, void*);
typedef unsigned int (*P_GetUI)(NgxParameter*, const char*, unsigned int*);
typedef unsigned int (*P_GetI)(NgxParameter*, const char*, int*);
typedef unsigned int (*P_GetF)(NgxParameter*, const char*, float*);
typedef unsigned int (*P_GetPtr)(NgxParameter*, const char*, void**);
typedef unsigned int (*P_OptimalCb)(NgxParameter*);

NgxPerf toPerf(DlssQuality q) {
    switch (q) {
    case DlssQuality::Balanced: return kNgxBalanced;
    case DlssQuality::Performance: return kNgxMaxPerf;
    case DlssQuality::UltraPerformance: return kNgxUltraPerf;
    case DlssQuality::Dlaa: return kNgxDlaa;
    default: return kNgxMaxQuality;
    }
}

const char* qualityName(DlssQuality q) {
    switch (q) {
    case DlssQuality::Balanced: return "BALANCED";
    case DlssQuality::Performance: return "PERF";
    case DlssQuality::UltraPerformance: return "ULTRA";
    case DlssQuality::Dlaa: return "DLAA";
    case DlssQuality::Quality: return "QUALITY";
    default: return "OFF";
    }
}

float qualityScale(DlssQuality q) {
    switch (q) {
    case DlssQuality::Balanced: return 0.58f;
    case DlssQuality::Performance: return 0.50f;
    case DlssQuality::UltraPerformance: return 0.33f;
    case DlssQuality::Dlaa: return 1.00f;
    case DlssQuality::Quality: return 0.67f;
    default: return 1.00f;
    }
}

std::wstring findNvngx() {
    wchar_t root[] = L"C:\\Windows\\System32\\DriverStore\\FileRepository";
    WIN32_FIND_DATAW dir{};
    const std::wstring query = std::wstring(root) + L"\\*";
    HANDLE h = FindFirstFileW(query.c_str(), &dir);
    if (h == INVALID_HANDLE_VALUE) return {};
    do {
        if ((dir.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        if (dir.cFileName[0] == L'.') continue;
        const std::wstring candidate =
            std::wstring(root) + L"\\" + dir.cFileName + L"\\_nvngx.dll";
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            FindClose(h);
            return candidate;
        }
    } while (FindNextFileW(h, &dir));
    FindClose(h);
    return {};
}

} // namespace

void copyHaltonJitter(std::uint32_t index, float* outX, float* outY) {
    auto halton = [](std::uint32_t i, std::uint32_t base) {
        float result = 0.0f, f = 1.0f / static_cast<float>(base);
        while (i > 0) {
            result += f * static_cast<float>(i % base);
            i /= base;
            f /= static_cast<float>(base);
        }
        return result;
    };
    const std::uint32_t sample = index + 1u;
    if (outX) *outX = halton(sample, 2u) - 0.5f;
    if (outY) *outY = halton(sample, 3u) - 0.5f;
}

struct StreamlineHost::Impl {
    HMODULE module{};
    ID3D12Device* device{};
    NgxParameter* caps{};
    NgxParameter* eval{};
    NgxHandle* feature{};
    bool inited{};
    bool wantRr{};
    bool created{};
    bool resetNext{};
    bool evalDead{};
    int evalFails{};
    ID3D12Resource* scratch{};
    wchar_t pluginDir[MAX_PATH]{};
    wchar_t logDir[MAX_PATH]{};
    const wchar_t* pathList[1]{};
    NgxFeatureCommonInfo common{};
    float jitter[2]{};
    P_InitProject initProject{};
    typedef unsigned int (*P_InitSimple)(unsigned long long, const wchar_t*, ID3D12Device*, unsigned int);
    P_InitSimple initSimple{};
    P_Shutdown1 shutdown1{};
    P_GetCaps getCaps{};
    P_AllocParams allocParams{};
    P_DestroyParams destroyParams{};
    P_CreateFeature createFeature{};
    P_ReleaseFeature releaseFeature{};
    P_Evaluate evaluate{};
    P_SetUI setUI{};
    P_SetI setI{};
    P_SetF setF{};
    P_SetRes setRes{};
    P_SetPtr setPtr{};
    P_GetUI getUI{};
    P_GetI getI{};
    P_GetPtr getPtr{};
    P_Scratch getScratch{};
    typedef unsigned int (*P_GetParams)(NgxParameter**);
    P_GetParams getParams{};

    // Real NGX Parameter vtable (from nvsdk_ngx_parameters_lib.obj), NOT the
    // C++ header order. Slot 0 = Set(void*), 1 = Set(ID3D12Resource*).
    void** vt(NgxParameter* p) {
        return p ? *reinterpret_cast<void***>(p) : nullptr;
    }
    void pSetULL(NgxParameter* p, const char* n, unsigned long long v) {
        if (!p) return;
        using Fn = void (*)(void*, const char*, unsigned long long);
        reinterpret_cast<Fn>(vt(p)[7])(p, n, v);
    }
    void pSetF(NgxParameter* p, const char* n, float v) {
        if (!p) return;
        using Fn = void (*)(void*, const char*, float);
        reinterpret_cast<Fn>(vt(p)[6])(p, n, v);
    }
    void pSetU(NgxParameter* p, const char* n, unsigned int v) {
        if (!p) return;
        using Fn = void (*)(void*, const char*, unsigned int);
        reinterpret_cast<Fn>(vt(p)[4])(p, n, v);
    }
    void pSetI(NgxParameter* p, const char* n, int v) {
        if (!p) return;
        using Fn = void (*)(void*, const char*, int);
        reinterpret_cast<Fn>(vt(p)[3])(p, n, v);
    }
    void pSetR(NgxParameter* p, const char* n, ID3D12Resource* v) {
        if (!p) return;
        using Fn12 = void (*)(void*, const char*, ID3D12Resource*);
        using FnPtr = void (*)(void*, const char*, void*);
        reinterpret_cast<Fn12>(vt(p)[1])(p, n, v);
        reinterpret_cast<FnPtr>(vt(p)[0])(p, n, static_cast<void*>(v));
    }
    bool pGetU(NgxParameter* p, const char* n, unsigned int* v) {
        if (!p || !v) return false;
        if (getUI && ngxOk(getUI(p, n, v))) return true;
        using Fn = unsigned int (*)(void*, const char*, unsigned int*);
        return ngxOk(reinterpret_cast<Fn>(vt(p)[12])(p, n, v));
    }
    bool pGetP(NgxParameter* p, const char* n, void** v) {
        if (!p || !v) return false;
        if (getPtr && ngxOk(getPtr(p, n, v))) return true;
        using Fn = unsigned int (*)(void*, const char*, void**);
        return ngxOk(reinterpret_cast<Fn>(vt(p)[8])(p, n, v));
    }

    bool loadFunctions() {
        auto grab = [&](const char* name) -> void* {
            return reinterpret_cast<void*>(GetProcAddress(module, name));
        };
        initProject = reinterpret_cast<P_InitProject>(grab("NVSDK_NGX_D3D12_Init_ProjectID"));
        if (!initProject)
            initProject = reinterpret_cast<P_InitProject>(grab("NVSDK_NGX_D3D12_Init_with_ProjectID"));
        shutdown1 = reinterpret_cast<P_Shutdown1>(grab("NVSDK_NGX_D3D12_Shutdown1"));
        getCaps = reinterpret_cast<P_GetCaps>(grab("NVSDK_NGX_D3D12_GetCapabilityParameters"));
        allocParams = reinterpret_cast<P_AllocParams>(grab("NVSDK_NGX_D3D12_AllocateParameters"));
        destroyParams = reinterpret_cast<P_DestroyParams>(grab("NVSDK_NGX_D3D12_DestroyParameters"));
        createFeature = reinterpret_cast<P_CreateFeature>(grab("NVSDK_NGX_D3D12_CreateFeature"));
        releaseFeature = reinterpret_cast<P_ReleaseFeature>(grab("NVSDK_NGX_D3D12_ReleaseFeature"));
        evaluate = reinterpret_cast<P_Evaluate>(grab("NVSDK_NGX_D3D12_EvaluateFeature"));
        getScratch = reinterpret_cast<P_Scratch>(grab("NVSDK_NGX_D3D12_GetScratchBufferSize"));
        getParams = reinterpret_cast<P_GetParams>(grab("NVSDK_NGX_D3D12_GetParameters"));
        setUI = reinterpret_cast<P_SetUI>(grab("NVSDK_NGX_Parameter_SetUI"));
        setI = reinterpret_cast<P_SetI>(grab("NVSDK_NGX_Parameter_SetI"));
        setF = reinterpret_cast<P_SetF>(grab("NVSDK_NGX_Parameter_SetF"));
        setRes = reinterpret_cast<P_SetRes>(grab("NVSDK_NGX_Parameter_SetD3d12Resource"));
        setPtr = reinterpret_cast<P_SetPtr>(grab("NVSDK_NGX_Parameter_SetVoidPointer"));
        getUI = reinterpret_cast<P_GetUI>(grab("NVSDK_NGX_Parameter_GetUI"));
        getI = reinterpret_cast<P_GetI>(grab("NVSDK_NGX_Parameter_GetI"));
        getPtr = reinterpret_cast<P_GetPtr>(grab("NVSDK_NGX_Parameter_GetVoidPointer"));
        initSimple = reinterpret_cast<P_InitSimple>(grab("NVSDK_NGX_D3D12_Init"));
        return (initProject || initSimple) && getCaps && createFeature && evaluate;
    }

    void applyFallback(StreamlineStatus& status, std::uint32_t dw, std::uint32_t dh,
                       DlssQuality quality) {
        status.taau = quality != DlssQuality::Off;
        status.dlss = false;
        status.rayReconstruction = false;
        status.quality = quality == DlssQuality::Off ? DlssQuality::Off : quality;
        status.displayWidth = dw;
        status.displayHeight = dh;
        if (!status.taau) {
            status.renderWidth = dw;
            status.renderHeight = dh;
            writeLabel(status.label, sizeof(status.label), "NATIVE");
            return;
        }
        const float s = qualityScale(quality);
        auto align8 = [](std::uint32_t v) {
            v = std::max(8u, v);
            return v - (v % 8u);
        };
        status.renderWidth = align8(static_cast<std::uint32_t>(std::lround(dw * s)));
        status.renderHeight = align8(static_cast<std::uint32_t>(std::lround(dh * s)));
        char label[48]{};
        std::snprintf(label, sizeof(label), "TAAU %s", qualityName(quality));
        writeLabel(status.label, sizeof(status.label), label);
    }
};

StreamlineHost::StreamlineHost(): impl_(new Impl) {}
StreamlineHost::~StreamlineHost() {
    shutdown();
    delete impl_;
    impl_ = nullptr;
}

bool StreamlineHost::startup(const wchar_t* pluginDirectory) {
    status_ = {};
    if (FILE* f = fopen("C:\\StressTest\\video\\ngx.log", "w")) {
        fputs("ngx host start\n", f);
        fclose(f);
    }
    wchar_t dir[MAX_PATH]{};
    if (pluginDirectory) wcsncpy(dir, pluginDirectory, MAX_PATH - 1);
    wchar_t sl[MAX_PATH]{};
    wcsncpy(sl, dir, MAX_PATH - 16);
    wcscat(sl, L"\\streamline");
    wchar_t exeDlss[MAX_PATH]{};
    wcsncpy(exeDlss, dir, MAX_PATH - 20);
    wcscat(exeDlss, L"\\nvngx_dlss.dll");
    if (GetFileAttributesW(exeDlss) != INVALID_FILE_ATTRIBUTES)
        wcsncpy(impl_->pluginDir, dir, MAX_PATH - 1);
    else if (GetFileAttributesW(sl) != INVALID_FILE_ATTRIBUTES)
        wcsncpy(impl_->pluginDir, sl, MAX_PATH - 1);
    else
        wcsncpy(impl_->pluginDir, dir, MAX_PATH - 1);
    wcsncpy(impl_->logDir, L"C:\\StressTest\\video", MAX_PATH - 1);

    const std::wstring ngx = findNvngx();
    if (ngx.empty()) {
        appendLog("no _nvngx.dll in DriverStore");
        return false;
    }
    char narrow[MAX_PATH]{};
    WideCharToMultiByte(CP_UTF8, 0, ngx.c_str(), -1, narrow, MAX_PATH, nullptr, nullptr);
    appendLog(narrow);
    bool loaded = false;
    if (!ngxTrap([&] {
            impl_->module = LoadLibraryW(ngx.c_str());
            loaded = impl_->module != nullptr;
        })) {
        appendLog("LoadLibrary _nvngx access violation");
        impl_->module = nullptr;
        return false;
    }
    if (!loaded) {
        logf("LoadLibrary _nvngx failed %d", static_cast<int>(GetLastError()));
        return false;
    }
    if (!impl_->loadFunctions()) {
        appendLog("missing NGX C exports");
        FreeLibrary(impl_->module);
        impl_->module = nullptr;
        return false;
    }
    appendLog("NGX C API loaded");
    status_.loaded = true;
    return true;
}

bool StreamlineHost::setDevice(ID3D12Device* device) {
    if (impl_->inited) return status_.dlss || status_.rayReconstruction;
    if (!impl_->module || !device || (!impl_->initSimple && !impl_->initProject))
        return false;
    impl_->device = device;
    impl_->pathList[0] = impl_->pluginDir;
    impl_->common = {};
    impl_->common.PathListInfo.Path = impl_->pathList;
    impl_->common.PathListInfo.Length = 1;
    impl_->common.LoggingInfo.MinimumLoggingLevel = 1;
    wchar_t dlssDll[MAX_PATH]{};
    wcsncpy(dlssDll, impl_->pluginDir, MAX_PATH - 20);
    wcscat(dlssDll, L"\\nvngx_dlss.dll");
    char pluginNarrow[MAX_PATH]{};
    WideCharToMultiByte(CP_UTF8, 0, impl_->pluginDir, -1, pluginNarrow, MAX_PATH, nullptr, nullptr);
    appendLog(pluginNarrow);
    appendLog(GetFileAttributesW(dlssDll) != INVALID_FILE_ATTRIBUTES
                  ? "nvngx_dlss.dll present in plugin dir"
                  : "nvngx_dlss.dll MISSING from plugin dir");
    const unsigned int ver = 0x0000015;
    unsigned int r = kNgxFail;
    // Event Viewer: GrassStress 0xC0000005 in _nvngx.dll during Init.
    // Trap it so the live path can keep running without DLSS.
    if (impl_->initSimple) {
        if (!ngxTrap([&] {
                r = impl_->initSimple(0x47525353ull, impl_->logDir, device, ver);
            })) {
            appendLog("D3D12_Init access violation");
            impl_->evalDead = true;
            return false;
        }
        logf("D3D12_Init 0x%08x", static_cast<int>(r));
    } else if (impl_->initProject) {
        if (!ngxTrap([&] {
                r = impl_->initProject("7c3e1d2a-5080-4b1e-9c77-a10c6fa55b01",
                                       kNgxEngineCustom, "0.1.0", impl_->logDir,
                                       device, &impl_->common, ver);
            })) {
            appendLog("Init_ProjectID access violation");
            impl_->evalDead = true;
            return false;
        }
        logf("Init_ProjectID 0x%08x", static_cast<int>(r));
    }
    if (!ngxOk(r)) return false;
    impl_->inited = true;
    if (impl_->getCaps(&impl_->caps) != kNgxSuccess || !impl_->caps) {
        appendLog("GetCapabilityParameters failed");
        return false;
    }
    unsigned int available = 0;
    impl_->pGetU(impl_->caps, "SuperSampling.Available", &available);
    if (!available) impl_->pGetU(impl_->caps, "#\x01", &available);
    logf("SuperSampling.Available %d", static_cast<int>(available));
    unsigned int needDriver = 0, initResult = 0;
    impl_->pGetU(impl_->caps, "SuperSampling.NeedsUpdatedDriver", &needDriver);
    impl_->pGetU(impl_->caps, "SuperSampling.FeatureInitResult", &initResult);
    logf("SS NeedDriver %d InitResult 0x%08x", static_cast<int>(needDriver),
         static_cast<int>(initResult));
    status_.dlss = available != 0;
    unsigned int rr = 0;
    impl_->pGetU(impl_->caps, "RayReconstruction.Available", &rr);
    if (!rr) impl_->pGetU(impl_->caps, "SuperSamplingDenoising.Available", &rr);
    status_.rayReconstruction = rr != 0;
    logf("RayReconstruction.Available %d", static_cast<int>(rr));
    if (impl_->getParams)
        impl_->getParams(&impl_->eval);
    if (!impl_->eval && impl_->allocParams)
        impl_->allocParams(&impl_->eval);
    appendLog(impl_->getParams && impl_->eval ? "using GetParameters" : "using AllocateParameters");
    return status_.dlss || status_.rayReconstruction;
}

bool StreamlineHost::upgradeFactory(IDXGIFactory**) { return false; }
bool StreamlineHost::upgradeSwapChain(IDXGISwapChain**) { return false; }

bool StreamlineHost::configure(std::uint32_t displayWidth, std::uint32_t displayHeight,
                               DlssQuality quality, FrameGenMode frameGen) {
    requestedQuality_ = quality;
    // Frame generation needs sl.interposer.dll beside the exe. On this MinGW
    // host that DLL faults before wWinMain, so FG stays Off.
    requestedFrameGen_ = FrameGenMode::Off;
    (void)frameGen;
    status_.displayWidth = displayWidth;
    status_.displayHeight = displayHeight;
    status_.frameGen = FrameGenMode::Off;
    status_.frameGeneration = false;
    status_.mfgMultiplier = 1;
    status_.presentedFrames = 1;
    if (impl_->feature && impl_->releaseFeature) {
        impl_->releaseFeature(impl_->feature);
        impl_->feature = nullptr;
    }
    impl_->created = false;
    impl_->resetNext = true;
    impl_->evalDead = false;

    if (quality == DlssQuality::Off) {
        impl_->applyFallback(status_, displayWidth, displayHeight, DlssQuality::Off);
        return true;
    }

    unsigned int optW = 0, optH = 0;
    if (impl_->inited && (impl_->eval || impl_->caps)) {
        NgxParameter* src = impl_->eval ? impl_->eval : impl_->caps;
        void* cb = nullptr;
        impl_->pGetP(src, "DLSSOptimalSettingsCallback", &cb);
        if (!cb)
            impl_->pGetP(src, "DLSSDOptimalSettingsCallback", &cb);
        if (!cb)
            impl_->pGetP(src, "#\x3e", &cb);
        if (cb) {
            impl_->pSetU(src, "Width", displayWidth);
            impl_->pSetU(src, "Height", displayHeight);
            impl_->pSetI(src, "PerfQualityValue", toPerf(quality));
            impl_->pSetI(src, "RTXValue", 0);
            auto* pfn = reinterpret_cast<P_OptimalCb>(cb);
            if (ngxOk(pfn(src))) {
                impl_->pGetU(src, "OutWidth", &optW);
                impl_->pGetU(src, "OutHeight", &optH);
            }
            appendLog("optimal callback present");
        } else {
            appendLog("no optimal callback");
        }
    }

    if (optW && optH && (status_.dlss || status_.rayReconstruction)) {
        status_.renderWidth = optW;
        status_.renderHeight = optH;
        status_.quality = quality;
        status_.taau = false;
        impl_->wantRr = status_.rayReconstruction;
        char label[48]{};
        std::snprintf(label, sizeof(label), "DLSS %s %s",
                      impl_->wantRr ? "RR" : "SR", qualityName(quality));
        writeLabel(status_.label, sizeof(status_.label), label);
        logf("optimal %dx%d", static_cast<int>(optW), static_cast<int>(optH));
        return true;
    }

    impl_->applyFallback(status_, displayWidth, displayHeight, quality);
    appendLog(status_.label);
    return true;
}

void StreamlineHost::shutdown() {
    if (impl_->feature && impl_->releaseFeature) {
        impl_->releaseFeature(impl_->feature);
        impl_->feature = nullptr;
    }
    if (impl_->scratch) {
        impl_->scratch->Release();
        impl_->scratch = nullptr;
    }
    // DestroyParameters/FreeLibrary of the driver object has crashed MinGW
    // after a failed evaluate. Shutdown1 is enough to drop the NGX instance.
    impl_->eval = nullptr;
    impl_->caps = nullptr;
    impl_->inited = false;
    impl_->created = false;
    impl_->device = nullptr;
    impl_->module = nullptr;
    status_ = {};
}

bool StreamlineHost::beginFrame(std::uint32_t) { return true; }
void StreamlineHost::markerSimulationStart() {}
void StreamlineHost::markerSimulationEnd() {}
void StreamlineHost::markerRenderStart() {}
void StreamlineHost::markerRenderEnd() {}
void StreamlineHost::markerPresentStart() {}
void StreamlineHost::markerPresentEnd() {}

void StreamlineHost::setCamera(const CameraBasis& current, const CameraBasis&, bool reset) {
    impl_->jitter[0] = current.jitter[0];
    impl_->jitter[1] = current.jitter[1];
    if (reset) impl_->resetNext = true;
}

bool StreamlineHost::ensureFeature(ID3D12GraphicsCommandList* commands) {
    if (!commands || impl_->evalDead) return false;
    if (!impl_->inited || !impl_->createFeature || !impl_->eval) return false;
    if (status_.quality == DlssQuality::Off) return false;
    if (impl_->created && impl_->feature) return true;

    impl_->pSetU(impl_->eval, "CreationNodeMask", 1);
    impl_->pSetU(impl_->eval, "VisibilityNodeMask", 1);
    impl_->pSetU(impl_->eval, "Width", status_.renderWidth);
    impl_->pSetU(impl_->eval, "Height", status_.renderHeight);
    impl_->pSetU(impl_->eval, "OutWidth", status_.displayWidth);
    impl_->pSetU(impl_->eval, "OutHeight", status_.displayHeight);
    impl_->pSetI(impl_->eval, "PerfQualityValue", toPerf(status_.quality));
    const int flags = 1 /*HDR*/ | 2 /*MVLowRes*/ | 64 /*AutoExposure*/;
    impl_->pSetI(impl_->eval, "DLSS.Feature.Create.Flags", flags);
    impl_->pSetI(impl_->eval, "DLSS.Enable.Output.Subrects", 0);
    impl_->pSetI(impl_->eval, "DLSS.Hint.Render.Preset.Quality", 11);
    impl_->pSetI(impl_->eval, "DLSS.Hint.Render.Preset.Balanced", 11);
    impl_->pSetI(impl_->eval, "DLSS.Hint.Render.Preset.Performance", 13);
    if (impl_->getScratch) {
        size_t bytes = 0;
        impl_->getScratch(kNgxSuperSampling, impl_->eval, &bytes);
        logf("scratch %d", static_cast<int>(bytes));
        if (bytes && impl_->device && !impl_->scratch) {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = std::max<UINT64>(bytes, 256);
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            impl_->device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                __uuidof(ID3D12Resource),
                reinterpret_cast<void**>(&impl_->scratch));
        }
        if (impl_->scratch) {
            impl_->pSetR(impl_->eval, "Scratch", impl_->scratch);
            impl_->pSetULL(impl_->eval, "Scratch.SizeInBytes", bytes);
        }
    }
    unsigned int id = kNgxSuperSampling;
    unsigned int cr = kNgxFail;
    if (!ngxTrap([&] {
            cr = impl_->createFeature(commands, id, impl_->eval, &impl_->feature);
        })) {
        appendLog("CreateFeature access violation");
        impl_->applyFallback(status_, status_.displayWidth, status_.displayHeight,
                             requestedQuality_);
        impl_->evalDead = true;
        return false;
    }
    logf("CreateFeature 0x%08x", static_cast<int>(cr));
    if (!ngxOk(cr) || !impl_->feature) {
        impl_->applyFallback(status_, status_.displayWidth, status_.displayHeight,
                             requestedQuality_);
        impl_->evalDead = true;
        return false;
    }
    impl_->created = true;
    impl_->resetNext = true;
    impl_->wantRr = false;
    status_.taau = false;
    status_.dlss = true;
    status_.rayReconstruction = false;
    char label[48]{};
    std::snprintf(label, sizeof(label), "DLSS SR %s", qualityName(status_.quality));
    writeLabel(status_.label, sizeof(status_.label), label);
    appendLog(status_.label);
    return true;
}

bool StreamlineHost::evaluateUpscale(ID3D12GraphicsCommandList* commands,
                                     ID3D12Resource* hdrColor,
                                     ID3D12Resource* linearDepth,
                                     ID3D12Resource* motionVectors,
                                     ID3D12Resource* normalRough,
                                     ID3D12Resource* diffuseAlbedo,
                                     ID3D12Resource* specularAlbedo,
                                     ID3D12Resource* outputColor,
                                     std::uint32_t, std::uint32_t, std::uint32_t,
                                     std::uint32_t, std::uint32_t) {
    if (!commands || !hdrColor || !outputColor) return false;
    if (impl_->evalDead) return false;
    if (!impl_->inited || !impl_->createFeature || !impl_->evaluate || !impl_->eval)
        return false;
    if (status_.quality == DlssQuality::Off) return false;
    if (!impl_->created || !impl_->feature) return false;

    impl_->pSetR(impl_->eval, "Color", hdrColor);
    impl_->pSetR(impl_->eval, "Output", outputColor);
    impl_->pSetR(impl_->eval, "Depth", linearDepth);
    impl_->pSetR(impl_->eval, "MotionVectors", motionVectors);
    impl_->pSetR(impl_->eval, "TransparencyMask", nullptr);
    impl_->pSetR(impl_->eval, "ExposureTexture", nullptr);
    impl_->pSetR(impl_->eval, "DLSS.Input.Bias.Current.Color.Mask", nullptr);
    impl_->pSetF(impl_->eval, "Jitter.Offset.X", impl_->jitter[0]);
    impl_->pSetF(impl_->eval, "Jitter.Offset.Y", impl_->jitter[1]);
    impl_->pSetF(impl_->eval, "Sharpness", 0.0f);
    impl_->pSetI(impl_->eval, "Reset", impl_->resetNext ? 1 : 0);
    impl_->pSetF(impl_->eval, "MV.Scale.X", static_cast<float>(status_.renderWidth));
    impl_->pSetF(impl_->eval, "MV.Scale.Y", static_cast<float>(status_.renderHeight));
    impl_->pSetU(impl_->eval, "DLSS.Render.Subrect.Dimensions.Width", status_.renderWidth);
    impl_->pSetU(impl_->eval, "DLSS.Render.Subrect.Dimensions.Height", status_.renderHeight);
    impl_->pSetU(impl_->eval, "DLSS.Input.Color.Subrect.Base.X", 0);
    impl_->pSetU(impl_->eval, "DLSS.Input.Color.Subrect.Base.Y", 0);
    impl_->pSetU(impl_->eval, "DLSS.Input.Depth.Subrect.Base.X", 0);
    impl_->pSetU(impl_->eval, "DLSS.Input.Depth.Subrect.Base.Y", 0);
    impl_->pSetU(impl_->eval, "DLSS.Input.MV.Subrect.Base.X", 0);
    impl_->pSetU(impl_->eval, "DLSS.Input.MV.Subrect.Base.Y", 0);
    impl_->pSetU(impl_->eval, "DLSS.Output.Subrect.Base.X", 0);
    impl_->pSetU(impl_->eval, "DLSS.Output.Subrect.Base.Y", 0);
    impl_->pSetF(impl_->eval, "DLSS.Pre.Exposure", 1.0f);
    impl_->pSetF(impl_->eval, "DLSS.Exposure.Scale", 1.0f);
    impl_->pSetF(impl_->eval, "FrameTimeDeltaInMsec", 16.0f);
    if (impl_->scratch) {
        impl_->pSetR(impl_->eval, "Scratch", impl_->scratch);
    }
    if (impl_->wantRr) {
        impl_->pSetR(impl_->eval, "DLSS.Input.DiffuseAlbedo", diffuseAlbedo);
        impl_->pSetR(impl_->eval, "DLSS.Input.SpecularAlbedo", specularAlbedo);
        impl_->pSetR(impl_->eval, "GBuffer.Normals", normalRough);
        impl_->pSetR(impl_->eval, "GBuffer.Roughness", normalRough);
    }
    unsigned int er = kNgxFail;
    if (!ngxTrap([&] {
            er = impl_->evaluate(commands, impl_->feature, impl_->eval, nullptr);
        })) {
        appendLog("EvaluateFeature access violation");
        impl_->evalDead = true;
        impl_->applyFallback(status_, status_.displayWidth, status_.displayHeight,
                             requestedQuality_);
        return false;
    }
    if (!ngxOk(er)) {
        logf("EvaluateFeature 0x%08x", static_cast<int>(er));
        if (++impl_->evalFails >= 2) {
            appendLog("DLSS evaluate disabled; using TAAU");
            impl_->evalDead = true;
            impl_->applyFallback(status_, status_.displayWidth, status_.displayHeight,
                                 requestedQuality_);
        }
        return false;
    }
    impl_->evalFails = 0;
    impl_->resetNext = false;
    return true;
}

void StreamlineHost::tagFrameGeneration(ID3D12Resource*, ID3D12Resource*,
                                        std::uint32_t, std::uint32_t) {}

void StreamlineHost::queryPresentedFrames() {
    status_.presentedFrames = 1;
}

void StreamlineHost::setQuality(DlssQuality quality) { requestedQuality_ = quality; }
void StreamlineHost::setFrameGen(FrameGenMode mode) { requestedFrameGen_ = mode; }

} // namespace dense
