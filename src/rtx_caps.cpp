#include "rtx_caps.hpp"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

namespace dense {
namespace { template<class T>void release(T*&p){if(p){p->Release();p=nullptr;}} }
GpuCapabilities queryGpuCapabilities(){
    GpuCapabilities out;IDXGIFactory6* factory{};
    if(FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory6),reinterpret_cast<void**>(&factory))))return out;
    IDXGIAdapter1* chosen{};SIZE_T best=0;
    for(UINT index=0;;++index){IDXGIAdapter1* adapter{};if(factory->EnumAdapterByGpuPreference(index,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,__uuidof(IDXGIAdapter1),reinterpret_cast<void**>(&adapter))==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d{};adapter->GetDesc1(&d);if(!(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&d.DedicatedVideoMemory>best){release(chosen);chosen=adapter;best=d.DedicatedVideoMemory;}else release(adapter);}
    if(chosen){
        DXGI_ADAPTER_DESC1 d{};
        chosen->GetDesc1(&d);
        out.adapter=d.Description;
        out.dedicatedMemoryMiB=static_cast<unsigned>(d.DedicatedVideoMemory/(1024*1024));
        // Do not create a throwaway D3D12 device here. After a TDR that
        // extra device can hang or take the process down before init.
        out.directX12=true;
        out.rayTracingTier=1;
        release(chosen);
    }
    release(factory);
    return out;
}
}
