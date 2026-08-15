// Drop-in math from papers this repo did not implement until now.
// Experiment ids live in Camera.sceneSettings.y / DenoiseCB.experiment.

#ifndef EXPERIMENT_MATH_HLSLI
#define EXPERIMENT_MATH_HLSLI

// 0 BASE     current hybrid path
// 1 R2QMC    Roberts 2018 plastic sequence + Cranley-Patterson
// 2 FIBER    Kajiya-Kay 1989 + Marschner 2003 R lobe on blades
// 3 HASHGI   Instant-NGP hash + SHARC-style world cache (Müller 2021 / NVIDIA)
// 4 RESTIR   Bitterli 2020 / Ouyang ReSTIR GI 2021 screen reservoirs
// 5 PSF      Binder, Fricke, Keller 2019 hashed path-space filtering
// 6 CASCADES Sannikov RC + Freeman/Sannikov Split RC, arXiv:2607.20384 (Jul 2026)
// 7 STACK    all of the above on the same frame

static const uint EXP_BASE = 0;
static const uint EXP_R2QMC = 1;
static const uint EXP_FIBER = 2;
static const uint EXP_HASHGI = 3;
static const uint EXP_RESTIR = 4;
static const uint EXP_PSF = 5;
static const uint EXP_CASCADES = 6;
static const uint EXP_STACK = 7;
static const uint EXP_COUNT = 8;

uint experimentId(float packed) {
    return min(EXP_COUNT - 1u, (uint)max(packed, 0));
}

bool expUsesR2(uint e) { return e == EXP_R2QMC || e == EXP_STACK; }
bool expUsesFiber(uint e) { return e == EXP_FIBER || e == EXP_STACK; }
bool expUsesHash(uint e) { return e == EXP_HASHGI || e == EXP_STACK; }
bool expUsesRestir(uint e) { return e == EXP_RESTIR || e == EXP_STACK; }
bool expUsesPsf(uint e) { return e == EXP_PSF || e == EXP_STACK; }
bool expUsesCascades(uint e) { return e == EXP_CASCADES || e == EXP_STACK; }

// Roberts, "The Unreasonable Effectiveness of Quasirandom Sequences" (2018).
// Plastic constant g = 1.32471795724474602596; R2 is the 2-D Kronecker sequence.
float2 r2Sequence(uint n) {
    const float g = 1.324717957244746;
    const float a1 = 1.0 / g;
    const float a2 = 1.0 / (g * g);
    return frac(float2(.5, .5) + float2((float)n * a1, (float)n * a2));
}

// Cranley-Patterson rotation so neighboring pixels are not the same lattice.
float2 r2Cranley(uint n, float2 rotate) {
    return frac(r2Sequence(n) + rotate);
}

uint pcgHash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Instant-NGP spatial hash (Müller et al., 2022) / SHARC cell key.
uint spatialHash3(int3 p) {
    const uint p1 = 73856093u;
    const uint p2 = 19349663u;
    const uint p3 = 83492791u;
    return pcgHash((uint)p.x * p1 ^ (uint)p.y * p2 ^ (uint)p.z * p3);
}

int3 hashCell(float3 world, float cellSize) {
    return int3(floor(world / max(cellSize, 1e-3)));
}

// Kajiya & Kay, SIGGRAPH 1989: "Rendering fur with three dimensional textures".
// Marschner et al., SIGGRAPH 2003: "Light Scattering from Human Hair Fibers" (R lobe).
// A grass blade is a tapered dielectric cylinder; this is the cheap real-time form.
float3 fiberScatter(float3 tangent, float3 normal, float3 view, float3 light,
                    float3 albedo, float wetness) {
    float3 t = normalize(tangent);
    float3 v = normalize(view);
    float3 l = normalize(light);
    float tl = dot(t, l);
    float tv = dot(t, v);
    float sinTL = sqrt(max(1.0 - tl * tl, 0));
    float sinTV = sqrt(max(1.0 - tv * tv, 0));
    float3 kkDiffuse = albedo * sinTL;
    float specExp = lerp(24.0, 72.0, wetness);
    float kkSpec = pow(saturate(sinTL * sinTV - tl * tv), specExp);
    // Marschner R: first surface reflection, cuticle-shifted along the fiber.
    float3 nR = normalize(normal + t * .12);
    float3 R = reflect(-l, nR);
    float marschnerR = pow(saturate(dot(R, v)), lerp(40.0, 96.0, wetness));
    // Dual-scattering fill (Zinke et al. 2008), a cheap global-multiple term.
    float dual = saturate(.18 + .22 * sinTL) * lerp(.55, .85, wetness);
    return kkDiffuse * (1.0 + dual) +
           (kkSpec * lerp(.10, .28, wetness) + marschnerR * lerp(.08, .22, wetness)) *
           float3(1.04, .98, .86);
}

// Kulla & Fajardo, SIGGRAPH 2012: equiangular sampling for a light along a ray.
// Used as a cheap aerial-perspective / fog sample toward the key light.
float equiangularDistance(float3 origin, float3 dir, float3 light, float tMax, float u) {
    float3 delta = origin - light;
    float tClosest = -dot(dir, delta);
    float D = length(delta + dir * tClosest);
    D = max(D, 1e-3);
    float thetaA = atan((-tClosest) / D);
    float thetaB = atan((tMax - tClosest) / D);
    float t = D * tan(lerp(thetaA, thetaB, u));
    return saturate((tClosest + t) / max(tMax, 1e-3)) * tMax;
}

float3 reconstructWorld(uint2 pixel, float linearDepth, float3 eye, float3 forward,
                        float3 right, float3 up, float tanHalf, float aspect,
                        uint2 size) {
    float2 uv = (float2(pixel) + .5) / float2(max(size, 1));
    float2 ndc = uv * 2 - 1;
    ndc.y = -ndc.y;
    float3 dir = normalize(forward +
                           right * (ndc.x * aspect * tanHalf) +
                           up * (ndc.y * tanHalf));
    return eye + dir * linearDepth;
}

#endif
