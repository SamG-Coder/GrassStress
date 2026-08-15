// SM experiments. BASE is A-trous (Dammertz 2010). Other modes are the
// papers in docs/OBSCURE_PAPERS.md — hash GI, ReSTIR, path-space filter,
// Split Radiance Cascades. Runs after DXR so leftover CUDA cores work.
#include "experiment_math.hlsli"

Texture2D<float4> Color : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D<float4> NormalRough : register(t2);
RWTexture2D<float4> Output : register(u0);

cbuffer DenoiseCB : register(b0)
{
    uint2 g_Size;
    uint g_Step;
    uint g_Experiment;
    float3 g_Eye;
    float g_TanHalf;
    float3 g_Forward;
    float g_Aspect;
    float3 g_Right;
    uint g_Frame;
    float3 g_Up;
    float g_Pad;
};

float3 firefly(float3 c) {
    float lum = dot(max(c, 0), float3(.2126, .7152, .0722));
    return c * (lum > 8.0 ? (8.0 / lum) : 1.0);
}

float3 atrous(int2 pixel) {
    float4 center = Color.Load(int3(pixel, 0));
    float depth = Depth.Load(int3(pixel, 0));
    float3 normal = NormalRough.Load(int3(pixel, 0)).xyz;
    const float kernel[3] = {0.25, 0.5, 0.25};
    float3 sum = 0;
    float weightSum = 0;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            int2 s = clamp(pixel + int2(x, y) * int(g_Step), int2(0, 0), int2(g_Size) - 1);
            float4 sampleColor = Color.Load(int3(s, 0));
            float sampleDepth = Depth.Load(int3(s, 0));
            float3 sampleNormal = NormalRough.Load(int3(s, 0)).xyz;
            float3 sc = firefly(sampleColor.rgb);
            float w = kernel[x + 1] * kernel[y + 1];
            w *= exp(-abs(sampleDepth - depth) * 2.4);
            w *= pow(saturate(dot(normalize(normal + 1e-5), normalize(sampleNormal + 1e-5))), 16.0);
            sum += sc * w;
            weightSum += w;
        }
    }
    return sum / max(weightSum, 1e-5);
}

// HASHGI / PSF: Instant-NGP hash of reconstructed world cells.
// Neighbors in the same cell vote their irradiance (Binder/Keller PSF).
float3 hashGi(int2 pixel) {
    float depth = Depth.Load(int3(pixel, 0));
    float3 normal = NormalRough.Load(int3(pixel, 0)).xyz;
    float3 world = reconstructWorld(pixel, max(depth, 1e-3), g_Eye, g_Forward,
                                    g_Right, g_Up, g_TanHalf, g_Aspect, g_Size);
    uint cell = spatialHash3(hashCell(world, .42));
    float3 sum = firefly(Color.Load(int3(pixel, 0)).rgb);
    float wsum = 1;
    const int2 offs[8] = {
        int2(-3, 0), int2(3, 0), int2(0, -3), int2(0, 3),
        int2(-5, -2), int2(5, 2), int2(-2, 5), int2(2, -5)
    };
    [unroll] for (int i = 0; i < 8; ++i) {
        int2 s = clamp(pixel + offs[i], int2(0, 0), int2(g_Size) - 1);
        float sd = Depth.Load(int3(s, 0));
        float3 sw = reconstructWorld(s, max(sd, 1e-3), g_Eye, g_Forward,
                                     g_Right, g_Up, g_TanHalf, g_Aspect, g_Size);
        uint sc = spatialHash3(hashCell(sw, .42));
        float3 sn = NormalRough.Load(int3(s, 0)).xyz;
        float w = (sc == cell) ? 1.0 : 0.15;
        w *= exp(-abs(sd - depth) * 1.8);
        w *= pow(saturate(dot(normalize(normal + 1e-5), normalize(sn + 1e-5))), 8.0);
        sum += firefly(Color.Load(int3(s, 0)).rgb) * w;
        wsum += w;
    }
    return sum / max(wsum, 1e-5);
}

// ReSTIR GI (Ouyang 2021) / GRIS (Lin 2022): one RIS pick from a 3x3
// of neighbor "paths" (their shaded color + depth as the sample).
float3 restirGi(int2 pixel) {
    float4 center = Color.Load(int3(pixel, 0));
    float depth = Depth.Load(int3(pixel, 0));
    float3 normal = NormalRough.Load(int3(pixel, 0)).xyz;
    float3 best = firefly(center.rgb);
    float bestW = max(dot(best, float3(.2126, .7152, .0722)), 1e-4);
    float M = 1;
    uint seed = pcgHash((uint)pixel.x + ((uint)pixel.y << 16) + g_Frame * 17u);
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            if (x == 0 && y == 0) continue;
            int2 s = clamp(pixel + int2(x, y) * 3, int2(0, 0), int2(g_Size) - 1);
            float sd = Depth.Load(int3(s, 0));
            float3 sn = NormalRough.Load(int3(s, 0)).xyz;
            if (abs(sd - depth) > .35) continue;
            if (dot(normalize(normal + 1e-5), normalize(sn + 1e-5)) < .35) continue;
            float3 c = firefly(Color.Load(int3(s, 0)).rgb);
            float p = max(dot(c, float3(.2126, .7152, .0722)), 1e-4);
            M += 1;
            float w = p;
            seed = pcgHash(seed);
            float u = float(seed & 0x00ffffffu) * (1.0 / 16777216.0);
            if (u < w / (bestW + w)) {
                best = c;
                bestW = w;
            }
        }
    }
    // Unbiased-ish RIS: selected sample * (sum weights / M) / p
    float3 blended = lerp(firefly(center.rgb), best, saturate(.55 * log2(M + 1)));
    return blended;
}

// Split Radiance Cascades (Freeman & Sannikov, arXiv:2607.20384, Jul 2026).
// Screen-space: 4 cascades, interval chosen from hit distance (the split).
float3 radianceCascades(int2 pixel) {
    float depth = max(Depth.Load(int3(pixel, 0)), 1e-3);
    float3 normal = NormalRough.Load(int3(pixel, 0)).xyz;
    float3 base = firefly(Color.Load(int3(pixel, 0)).rgb);
    float3 sum = base;
    float wsum = 1;
    // Near cascade: short rays, high spatial. Far: long, coarse.
    const float intervals[4] = {8.0, 18.0, 40.0, 80.0};
    const float radii[4] = {2.0, 5.0, 11.0, 22.0};
    const uint dirs[4] = {4, 4, 4, 4};
    [unroll] for (uint cascade = 0; cascade < 4; ++cascade) {
        // Split: only this cascade owns the interval around `depth`.
        float lo = cascade == 0 ? 0.0 : intervals[cascade - 1];
        float hi = intervals[cascade];
        float gate = saturate(1.0 - abs(depth - .5 * (lo + hi)) / max(hi - lo, 1.0));
        if (gate < 1e-3) continue;
        uint n = dirs[cascade];
        float radius = radii[cascade];
        [unroll] for (uint d = 0; d < 4; ++d) {
            float ang = (float(d) + .25 * float(cascade)) * 1.5707963;
            float2 dir = float2(cos(ang), sin(ang));
            int2 s = clamp(pixel + int2(dir * radius), int2(0, 0), int2(g_Size) - 1);
            float sd = Depth.Load(int3(s, 0));
            // Occlusion: a closer neighbor blocks the interval.
            float vis = sd + .08 < depth ? 0.15 : 1.0;
            float3 sn = NormalRough.Load(int3(s, 0)).xyz;
            float w = gate * vis;
            w *= pow(saturate(dot(normalize(normal + 1e-5), normalize(sn + 1e-5))), 4.0);
            sum += firefly(Color.Load(int3(s, 0)).rgb) * w;
            wsum += w;
        }
    }
    return sum / max(wsum, 1e-5);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_Size.x || id.y >= g_Size.y) return;
    int2 pixel = int2(id.xy);
    float4 center = Color.Load(int3(pixel, 0));
    uint exp = g_Experiment;
    float3 result = firefly(center.rgb);

    if (exp == EXP_RESTIR)
        result = restirGi(pixel);
    else if (exp == EXP_CASCADES)
        result = radianceCascades(pixel);
    else if (exp == EXP_HASHGI || exp == EXP_PSF)
        result = hashGi(pixel);
    else if (exp == EXP_STACK) {
        float3 a = atrous(pixel);
        float3 h = hashGi(pixel);
        float3 r = restirGi(pixel);
        float3 c = radianceCascades(pixel);
        result = a * .35 + h * .22 + r * .23 + c * .20;
    } else
        result = atrous(pixel);

    Output[pixel] = float4(result, center.a);
}
