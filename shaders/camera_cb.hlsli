// Shared camera constant buffer.  First 176 bytes stay ABI-stable with the
// original DenseTrees layout; the trailing 80 bytes feed DLSS jitter / MVs.
struct Camera {
    float3 eye; float tanHalfFov;
    float3 forward; float aspect;
    float3 right; uint frameIndex;
    float3 up; uint maxFrames;
    float exposure; float localLightIntensity; float localLightRange;
    float localLightInnerCos;
    uint2 resolution; uint environmentIndexOffset; float localLightOuterCos;
    float4 grassSettings;
    float4 groundSettings;
    float4 grassInteraction;
    float4 waterState;
    float4 sceneSettings;
    float2 jitter;
    float2 prevJitter;
    float3 prevEye;
    float prevTanHalfFov;
    float3 prevForward;
    float prevAspect;
    float3 prevRight;
    float prevTime;
    float3 prevUp;
    float gbufferWrite;
};

float2 worldToUvUnjittered(float3 world, float3 eye, float3 forward, float3 right,
                           float3 up, float tanHalf, float aspect) {
    float3 delta = world - eye;
    float viewZ = max(dot(delta, forward), 1e-4);
    float ndcX = dot(delta, right) / (viewZ * max(aspect, 1e-4) * max(tanHalf, 1e-4));
    float ndcY = dot(delta, up) / (viewZ * max(tanHalf, 1e-4));
    return float2(ndcX * .5 + .5, -ndcY * .5 + .5);
}

float2 cameraMotionUv(float3 world, float3 prevWorld, Camera cam) {
    float2 curr = worldToUvUnjittered(world, cam.eye, cam.forward, cam.right, cam.up,
                                      cam.tanHalfFov, cam.aspect);
    float2 prev = worldToUvUnjittered(prevWorld, cam.prevEye, cam.prevForward, cam.prevRight,
                                      cam.prevUp, cam.prevTanHalfFov, cam.prevAspect);
    return prev - curr;
}

float4 applyPixelJitter(float4 clip, Camera cam) {
    float2 pixelToNdc = float2(2.0 / max((float)cam.resolution.x, 1.0),
                               -2.0 / max((float)cam.resolution.y, 1.0));
    clip.xy += cam.jitter * pixelToNdc * max(clip.w, 1e-4);
    return clip;
}
