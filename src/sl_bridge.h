#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef SL_BRIDGE_EXPORTS
#define SLB_API __declspec(dllexport)
#else
#define SLB_API __declspec(dllimport)
#endif

typedef struct SlbCamera {
    float eye[3];
    float forward[3];
    float right[3];
    float up[3];
    float tanHalf;
    float aspect;
    float jitter[2];
    float time;
} SlbCamera;

typedef struct SlbStatus {
    int loaded;
    int reflex;
    int dlss;
    int rayReconstruction;
    int frameGeneration;
    int dynamicMfg;
    uint32_t quality;
    uint32_t frameGen;
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t displayWidth;
    uint32_t displayHeight;
    uint32_t mfgMultiplier;
    uint32_t presentedFrames;
    char label[48];
} SlbStatus;

SLB_API int slb_startup(const wchar_t* pluginDirectory);
SLB_API int slb_set_device(void* device);
SLB_API int slb_upgrade_swap(void** swap);
SLB_API int slb_configure(uint32_t displayWidth, uint32_t displayHeight,
                          uint32_t quality, uint32_t frameGen, SlbStatus* out);
SLB_API int slb_begin_frame(uint32_t frameIndex);
SLB_API void slb_marker(int which);
SLB_API void slb_set_camera(const SlbCamera* current, const SlbCamera* previous, int reset);
SLB_API int slb_evaluate(void* commandList,
                         void* hdrColor, void* linearDepth, void* motionVectors,
                         void* normalRough, void* diffuseAlbedo, void* specularAlbedo,
                         void* outputColor,
                         uint32_t colorState, uint32_t depthState, uint32_t motionState,
                         uint32_t extraState, uint32_t outputState,
                         uint32_t renderWidth, uint32_t renderHeight,
                         uint32_t displayWidth, uint32_t displayHeight);
SLB_API void slb_tag_fg(void* hudless, void* uiColor,
                        uint32_t hudlessState, uint32_t uiState,
                        uint32_t displayWidth, uint32_t displayHeight);
SLB_API void slb_query(SlbStatus* out);
SLB_API void slb_shutdown(void);

#ifdef __cplusplus
}
#endif
