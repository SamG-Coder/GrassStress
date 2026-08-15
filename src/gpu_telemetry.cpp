#include "gpu_telemetry.hpp"

#include <windows.h>

#include <cstdint>

namespace dense {
namespace {

using nvmlReturn_t = int;
using nvmlDevice_t = void*;
struct nvmlUtilization_t { unsigned gpu, memory; };
struct nvmlMemory_t { unsigned long long total, free, used; };

constexpr int NVML_SUCCESS = 0;
constexpr int NVML_TEMPERATURE_GPU = 0;
constexpr int NVML_CLOCK_GRAPHICS = 0;

HMODULE nvml{};
nvmlDevice_t device{};
using NvmlInit_t = nvmlReturn_t(*)();
using NvmlShutdown_t = nvmlReturn_t(*)();
using NvmlDeviceGetHandleByIndex_t = nvmlReturn_t(*)(unsigned, nvmlDevice_t*);
using NvmlDeviceGetTemperature_t = nvmlReturn_t(*)(nvmlDevice_t, int, unsigned*);
using NvmlDeviceGetUtilizationRates_t = nvmlReturn_t(*)(nvmlDevice_t, nvmlUtilization_t*);
using NvmlDeviceGetPowerUsage_t = nvmlReturn_t(*)(nvmlDevice_t, unsigned*);
using NvmlDeviceGetClockInfo_t = nvmlReturn_t(*)(nvmlDevice_t, int, unsigned*);
using NvmlDeviceGetMemoryInfo_t = nvmlReturn_t(*)(nvmlDevice_t, nvmlMemory_t*);

NvmlInit_t nvmlInit{};
NvmlShutdown_t nvmlShutdown{};
NvmlDeviceGetHandleByIndex_t nvmlDeviceGetHandleByIndex{};
NvmlDeviceGetTemperature_t nvmlDeviceGetTemperature{};
NvmlDeviceGetUtilizationRates_t nvmlDeviceGetUtilizationRates{};
NvmlDeviceGetPowerUsage_t nvmlDeviceGetPowerUsage{};
NvmlDeviceGetClockInfo_t nvmlDeviceGetClockInfo{};
NvmlDeviceGetMemoryInfo_t nvmlDeviceGetMemoryInfo{};
bool ready = false;

template<class T>
T loadFn(const char* name) {
    return reinterpret_cast<T>(GetProcAddress(nvml, name));
}

} // namespace

bool initializeGpuTelemetry() {
    if(ready)return true;
    nvml = LoadLibraryW(L"nvml.dll");
    if(!nvml)return false;
    nvmlInit = loadFn<NvmlInit_t>("nvmlInit_v2");
    if(!nvmlInit)nvmlInit = loadFn<NvmlInit_t>("nvmlInit");
    nvmlShutdown = loadFn<NvmlShutdown_t>("nvmlShutdown");
    nvmlDeviceGetHandleByIndex = loadFn<NvmlDeviceGetHandleByIndex_t>("nvmlDeviceGetHandleByIndex_v2");
    if(!nvmlDeviceGetHandleByIndex)
        nvmlDeviceGetHandleByIndex = loadFn<NvmlDeviceGetHandleByIndex_t>("nvmlDeviceGetHandleByIndex");
    nvmlDeviceGetTemperature = loadFn<NvmlDeviceGetTemperature_t>("nvmlDeviceGetTemperature");
    nvmlDeviceGetUtilizationRates = loadFn<NvmlDeviceGetUtilizationRates_t>("nvmlDeviceGetUtilizationRates");
    nvmlDeviceGetPowerUsage = loadFn<NvmlDeviceGetPowerUsage_t>("nvmlDeviceGetPowerUsage");
    nvmlDeviceGetClockInfo = loadFn<NvmlDeviceGetClockInfo_t>("nvmlDeviceGetClockInfo");
    nvmlDeviceGetMemoryInfo = loadFn<NvmlDeviceGetMemoryInfo_t>("nvmlDeviceGetMemoryInfo");
    if(!nvmlInit||!nvmlDeviceGetHandleByIndex||nvmlInit()!=NVML_SUCCESS)return false;
    if(nvmlDeviceGetHandleByIndex(0,&device)!=NVML_SUCCESS)return false;
    ready=true;
    return true;
}

void shutdownGpuTelemetry() {
    if(ready&&nvmlShutdown)nvmlShutdown();
    if(nvml)FreeLibrary(nvml);
    nvml=nullptr;device=nullptr;ready=false;
}

GpuTelemetry sampleGpuTelemetry() {
    GpuTelemetry out;
    if(!ready&&!initializeGpuTelemetry())return out;
    unsigned temp=0,power=0,clock=0;
    nvmlUtilization_t util{};
    nvmlMemory_t memory{};
    if(nvmlDeviceGetTemperature&&nvmlDeviceGetTemperature(device,NVML_TEMPERATURE_GPU,&temp)==NVML_SUCCESS)
        out.temperatureC=static_cast<float>(temp);
    if(nvmlDeviceGetUtilizationRates&&nvmlDeviceGetUtilizationRates(device,&util)==NVML_SUCCESS)
        out.utilizationPercent=static_cast<float>(util.gpu);
    if(nvmlDeviceGetPowerUsage&&nvmlDeviceGetPowerUsage(device,&power)==NVML_SUCCESS)
        out.powerWatts=static_cast<float>(power)*0.001f;
    if(nvmlDeviceGetClockInfo&&nvmlDeviceGetClockInfo(device,NVML_CLOCK_GRAPHICS,&clock)==NVML_SUCCESS)
        out.clockMHz=static_cast<float>(clock);
    if(nvmlDeviceGetMemoryInfo&&nvmlDeviceGetMemoryInfo(device,&memory)==NVML_SUCCESS){
        out.vramUsedGiB=static_cast<float>(memory.used)/(1024.0f*1024.0f*1024.0f);
        out.vramTotalGiB=static_cast<float>(memory.total)/(1024.0f*1024.0f*1024.0f);
    }
    out.valid=true;
    return out;
}

} // namespace dense
