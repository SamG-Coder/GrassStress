// Edge-aware A-trous denoise only. Experiment filters used to steal this
// pass when the CB experiment field was non-zero.
Texture2D<float4> Color : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D<float4> NormalRough : register(t2);
RWTexture2D<float4> Output : register(u0);

cbuffer DenoiseCB : register(b0)
{
    uint2 g_Size;
    uint g_Step;
    uint g_Pad;
};

float3 firefly(float3 c) {
    float lum = dot(max(c, 0), float3(.2126, .7152, .0722));
    return c * (lum > 8.0 ? (8.0 / lum) : 1.0);
}

float3 atrous(int2 pixel, int step) {
    float4 center = Color.Load(int3(pixel, 0));
    float depth = Depth.Load(int3(pixel, 0));
    float3 normal = NormalRough.Load(int3(pixel, 0)).xyz;
    const float kernel[3] = {0.25, 0.5, 0.25};
    float3 sum = 0;
    float weightSum = 0;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            int2 s = clamp(pixel + int2(x, y) * step, int2(0, 0), int2(g_Size) - 1);
            float4 sampleColor = Color.Load(int3(s, 0));
            float sampleDepth = Depth.Load(int3(s, 0));
            float3 sampleNormal = NormalRough.Load(int3(s, 0)).xyz;
            float w = kernel[x + 1] * kernel[y + 1];
            w *= exp(-abs(sampleDepth - depth) * 2.4);
            w *= pow(saturate(dot(normalize(normal + 1e-5), normalize(sampleNormal + 1e-5))), 16.0);
            sum += firefly(sampleColor.rgb) * w;
            weightSum += w;
        }
    }
    return sum / max(weightSum, 1e-5);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_Size.x || id.y >= g_Size.y) return;
    int2 pixel = int2(id.xy);
    float4 center = Color.Load(int3(pixel, 0));
    int step = max((int)g_Step, 1);
    float3 fine = atrous(pixel, step);
    float3 coarse = atrous(pixel, step * 2);
    Output[pixel] = float4(fine * .65 + coarse * .35, center.a);
}
