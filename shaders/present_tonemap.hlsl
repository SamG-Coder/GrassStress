Texture2D<float4> HdrColor : register(t0);
SamplerState LinearClamp : register(s0);

cbuffer PresentCB : register(b0)
{
    float2 g_Resolution;
    float g_GrainSeed;
    float g_Pad;
};

float3 tonemap(float3 x) {
    return saturate((x * (2.51 * x + .03)) / (x * (2.43 * x + .59) + .14));
}

float3 colorGrade(float3 c) {
    float luminance = dot(c, float3(.2126, .7152, .0722));
    c = lerp(luminance.xxx, c, 1.14);
    c = pow(saturate(c), float3(.94, 1.0, 1.06));
    c *= float3(1.06, .98, .88);
    c = (c - .08) * 1.06 + .10;
    c = lerp(c, luminance.xxx, -0.04);
    return saturate(c);
}

float3 linearToSrgb(float3 c) {
    c = max(c, 0);
    return lerp(12.92 * c, 1.055 * pow(c, 1.0 / 2.4) - .055, step(.0031308, c));
}

float hash(float2 p) {
    float3 p3 = frac(float3(p.xyx) * .1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

struct VSOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
    VSOut output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.position = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}

float3 sampleCatmull(float2 uv) {
    float2 size = max(g_Resolution, 1.xx);
    float2 texel = 1.0 / size;
    float2 coord = uv * size - 0.5;
    float2 f = frac(coord);
    float2 base = (floor(coord) + 0.5) * texel;
    float3 result = 0;
    float weightSum = 0;
    [unroll] for (int y = -1; y <= 2; ++y) {
        float wy = (y == 0 || y == 1) ? (1.5 * abs(f.y - y) * abs(f.y - y) * abs(f.y - y)
            - 2.5 * (f.y - y) * (f.y - y) + 1.0)
            : (-0.5 * abs(f.y - y) * abs(f.y - y) * abs(f.y - y)
            + 2.5 * (f.y - y) * (f.y - y) - 4.0 * abs(f.y - y) + 2.0);
        [unroll] for (int x = -1; x <= 2; ++x) {
            float wx = (x == 0 || x == 1) ? (1.5 * abs(f.x - x) * abs(f.x - x) * abs(f.x - x)
                - 2.5 * (f.x - x) * (f.x - x) + 1.0)
                : (-0.5 * abs(f.x - x) * abs(f.x - x) * abs(f.x - x)
                + 2.5 * (f.x - x) * (f.x - x) - 4.0 * abs(f.x - x) + 2.0);
            float w = wx * wy;
            result += HdrColor.SampleLevel(LinearClamp, base + float2(x, y) * texel, 0).rgb * w;
            weightSum += w;
        }
    }
    return result / max(weightSum, 1e-5);
}

float4 PSMain(VSOut input) : SV_Target {
    float3 hdr = sampleCatmull(input.uv);
    float3 graded = colorGrade(tonemap(hdr * 1.28));
    float2 nv = input.uv * 2 - 1;
    float vignette = saturate(1.22 - dot(nv, nv) * .20);
    graded *= vignette;
    float grain = hash(input.uv * g_Resolution + g_GrainSeed) * .035 - .012;
    graded = saturate(graded + grain);
    return float4(linearToSrgb(graded), 1);
}
