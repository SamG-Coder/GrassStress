#define OPTIX_DONT_INCLUDE_CUDA
#include "optix_denoise.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

typedef struct CUctx_st* CUcontext;
typedef struct CUstream_st* CUstream;
typedef int CUresult;
typedef int CUdevice;

#include <optix.h>
#include <optix_function_table_definition.h>
#include <optix_stubs.h>

namespace dense {
namespace {

constexpr CUresult kCudaSuccess = 0;

struct CudaDriver {
    HMODULE module{};
    CUresult (*cuInit)(unsigned int){};
    CUresult (*cuDeviceGet)(CUdevice*, int){};
    CUresult (*cuCtxCreate)(CUcontext*, unsigned int, CUdevice){};
    CUresult (*cuCtxDestroy)(CUcontext){};
    CUresult (*cuCtxSetCurrent)(CUcontext){};
    CUresult (*cuMemAlloc)(CUdeviceptr*, size_t){};
    CUresult (*cuMemFree)(CUdeviceptr){};
    CUresult (*cuMemcpyHtoD)(CUdeviceptr, const void*, size_t){};
    CUresult (*cuMemcpyDtoH)(void*, CUdeviceptr, size_t){};
    CUresult (*cuCtxSynchronize)(){};
};

struct TensorState {
    CudaDriver cuda;
    CUcontext context{};
    OptixDeviceContext optixContext{};
    OptixDenoiser denoiser{};
    CUdeviceptr state{};
    CUdeviceptr scratch{};
    CUdeviceptr input{};
    CUdeviceptr output{};
    size_t stateBytes{};
    size_t scratchBytes{};
    size_t imageBytes{};
    unsigned width{};
    unsigned height{};
    TensorDenoiseStatus status{};
};

TensorState g;

template<class T>
bool loadFn(HMODULE module, T& fn, const char* name) {
    fn = reinterpret_cast<T>(GetProcAddress(module, name));
    return fn != nullptr;
}

bool loadCuda() {
    g.cuda.module = LoadLibraryW(L"nvcuda.dll");
    if(!g.cuda.module) return false;
    bool ok = true;
    ok = loadFn(g.cuda.module, g.cuda.cuInit, "cuInit") && ok;
    ok = loadFn(g.cuda.module, g.cuda.cuDeviceGet, "cuDeviceGet") && ok;
    ok = loadFn(g.cuda.module, g.cuda.cuCtxCreate, "cuCtxCreate_v2") && ok;
    if(!g.cuda.cuCtxCreate)
        ok = loadFn(g.cuda.module, g.cuda.cuCtxCreate, "cuCtxCreate") && ok;
    ok = loadFn(g.cuda.module, g.cuda.cuCtxDestroy, "cuCtxDestroy_v2") && ok;
    if(!g.cuda.cuCtxDestroy)
        loadFn(g.cuda.module, g.cuda.cuCtxDestroy, "cuCtxDestroy");
    ok = loadFn(g.cuda.module, g.cuda.cuCtxSetCurrent, "cuCtxSetCurrent") && ok;
    ok = loadFn(g.cuda.module, g.cuda.cuMemAlloc, "cuMemAlloc_v2") && ok;
    if(!g.cuda.cuMemAlloc)
        loadFn(g.cuda.module, g.cuda.cuMemAlloc, "cuMemAlloc");
    ok = loadFn(g.cuda.module, g.cuda.cuMemFree, "cuMemFree_v2") && ok;
    if(!g.cuda.cuMemFree)
        loadFn(g.cuda.module, g.cuda.cuMemFree, "cuMemFree");
    ok = loadFn(g.cuda.module, g.cuda.cuMemcpyHtoD, "cuMemcpyHtoD_v2") && ok;
    if(!g.cuda.cuMemcpyHtoD)
        loadFn(g.cuda.module, g.cuda.cuMemcpyHtoD, "cuMemcpyHtoD");
    ok = loadFn(g.cuda.module, g.cuda.cuMemcpyDtoH, "cuMemcpyDtoH_v2") && ok;
    if(!g.cuda.cuMemcpyDtoH)
        loadFn(g.cuda.module, g.cuda.cuMemcpyDtoH, "cuMemcpyDtoH");
    loadFn(g.cuda.module, g.cuda.cuCtxSynchronize, "cuCtxSynchronize");
    return ok && g.cuda.cuInit && g.cuda.cuDeviceGet && g.cuda.cuCtxCreate &&
           g.cuda.cuMemAlloc && g.cuda.cuMemcpyHtoD && g.cuda.cuMemcpyDtoH;
}

void freeDevice(CUdeviceptr& ptr) {
    if(ptr && g.cuda.cuMemFree) g.cuda.cuMemFree(ptr);
    ptr = 0;
}

void destroyDenoiser() {
    if(g.denoiser) {
        optixDenoiserDestroy(g.denoiser);
        g.denoiser = nullptr;
    }
    freeDevice(g.state);
    freeDevice(g.scratch);
    freeDevice(g.input);
    freeDevice(g.output);
    g.stateBytes = g.scratchBytes = g.imageBytes = 0;
    g.width = g.height = 0;
}

bool ensureDenoiser(unsigned width, unsigned height) {
    if(g.denoiser && g.width == width && g.height == height && g.input && g.output)
        return true;
    destroyDenoiser();
    OptixDenoiserOptions options{};
    options.guideAlbedo = 0;
    options.guideNormal = 0;
    options.denoiseAlpha = OPTIX_DENOISER_ALPHA_MODE_COPY;
    if(optixDenoiserCreate(g.optixContext, OPTIX_DENOISER_MODEL_KIND_HDR,
                           &options, &g.denoiser) != OPTIX_SUCCESS)
        return false;
    OptixDenoiserSizes sizes{};
    if(optixDenoiserComputeMemoryResources(g.denoiser, width, height, &sizes) != OPTIX_SUCCESS)
        return false;
    g.stateBytes = sizes.stateSizeInBytes;
    g.scratchBytes = sizes.withoutOverlapScratchSizeInBytes;
    g.imageBytes = static_cast<size_t>(width) * height * 4 * sizeof(float);
    if(g.cuda.cuMemAlloc(&g.state, g.stateBytes) != kCudaSuccess ||
       g.cuda.cuMemAlloc(&g.scratch, g.scratchBytes) != kCudaSuccess ||
       g.cuda.cuMemAlloc(&g.input, g.imageBytes) != kCudaSuccess ||
       g.cuda.cuMemAlloc(&g.output, g.imageBytes) != kCudaSuccess)
        return false;
    if(optixDenoiserSetup(g.denoiser, nullptr, width, height,
                          g.state, g.stateBytes, g.scratch, g.scratchBytes) != OPTIX_SUCCESS)
        return false;
    g.width = width;
    g.height = height;
    return true;
}

} // namespace

bool initTensorDenoiser() {
    if(g.status.optix) return true;
    std::snprintf(g.status.label, sizeof(g.status.label), "tensor off");
    if(!loadCuda()) return false;
    if(g.cuda.cuInit(0) != kCudaSuccess) return false;
    CUdevice device = 0;
    if(g.cuda.cuDeviceGet(&device, 0) != kCudaSuccess) return false;
    if(g.cuda.cuCtxCreate(&g.context, 0, device) != kCudaSuccess) return false;
    g.status.cuda = true;
    if(optixInit() != OPTIX_SUCCESS) {
        std::snprintf(g.status.label, sizeof(g.status.label), "CUDA ready, OptiX missing");
        return false;
    }
    OptixDeviceContextOptions options{};
    if(optixDeviceContextCreate(g.context, &options, &g.optixContext) != OPTIX_SUCCESS) {
        std::snprintf(g.status.label, sizeof(g.status.label), "CUDA ready, OptiX context failed");
        return false;
    }
    g.status.optix = true;
    std::snprintf(g.status.label, sizeof(g.status.label), "OptiX HDR tensor denoise");
    return true;
}

void shutdownTensorDenoiser() {
    if(g.context && g.cuda.cuCtxSetCurrent)
        g.cuda.cuCtxSetCurrent(g.context);
    destroyDenoiser();
    if(g.optixContext) {
        optixDeviceContextDestroy(g.optixContext);
        g.optixContext = nullptr;
    }
    if(g.context && g.cuda.cuCtxDestroy)
        g.cuda.cuCtxDestroy(g.context);
    g.context = nullptr;
    if(g.cuda.module) {
        FreeLibrary(g.cuda.module);
        g.cuda = {};
    }
    g.status = {};
}

const TensorDenoiseStatus& tensorDenoiseStatus() {
    return g.status;
}

bool tensorDenoiseRgba(const float* input, float* output,
                       std::uint32_t width, std::uint32_t height) {
    if(!input || !output || !width || !height) return false;
    if(!g.status.optix && !initTensorDenoiser()) return false;
    if(!g.status.optix || !g.context) return false;
    if(g.cuda.cuCtxSetCurrent && g.cuda.cuCtxSetCurrent(g.context) != kCudaSuccess)
        return false;
    if(!ensureDenoiser(width, height)) {
        destroyDenoiser();
        return false;
    }
    if(g.cuda.cuMemcpyHtoD(g.input, input, g.imageBytes) != kCudaSuccess)
        return false;
    OptixImage2D in{};
    in.data = g.input;
    in.width = width;
    in.height = height;
    in.rowStrideInBytes = width * 16;
    in.pixelStrideInBytes = 16;
    in.format = OPTIX_PIXEL_FORMAT_FLOAT4;
    OptixImage2D out = in;
    out.data = g.output;
    OptixDenoiserGuideLayer guide{};
    OptixDenoiserLayer layer{};
    layer.input = in;
    layer.output = out;
    OptixDenoiserParams params{};
    params.blendFactor = 0.0f;
    if(optixDenoiserInvoke(g.denoiser, nullptr, &params,
                           g.state, g.stateBytes, &guide, &layer, 1,
                           0, 0, g.scratch, g.scratchBytes) != OPTIX_SUCCESS)
        return false;
    if(g.cuda.cuCtxSynchronize && g.cuda.cuCtxSynchronize() != kCudaSuccess)
        return false;
    return g.cuda.cuMemcpyDtoH(output, g.output, g.imageBytes) == kCudaSuccess;
}

}
