# RTX 5080 / Blackwell techniques for 10M path-traced grass

Papers and NVIDIA tech notes that actually apply to this stress test.
Sorted by how much they change the 10M-blade renderer, not by citation count.

## Hardware we are targeting

GeForce RTX 5080 is Blackwell GB203:

- 4th-gen RT cores with a **triangle-cluster intersection engine** (Mega Geometry)
- Hardware **Linear Swept Spheres (LSS)** for strand primitives
- Hardware **Opacity Micro-Maps** (carried from Ada, still the vegetation trick)
- **Shader Execution Reordering** (DXR 1.2 / NVAPI)
- 5th-gen Tensor Cores (DLSS 4, Ray Reconstruction, neural caches)
- GDDR7, ~16 GB

Naive “one AABB, 100 blades, software intersection” does **not** use any of that.
That is why the first path-traced build was 0 FPS.

---

## 1. Treat blades as strands, not triangle soups

### Linear Swept Spheres (Blackwell hardware)

Hart & Kozlowski, NVIDIA (2025).
*Render Path-Traced Hair in Real Time with NVIDIA GeForce RTX 50 Series GPUs.*
https://developer.nvidia.com/blog/render-path-traced-hair-in-real-time-with-nvidia-geforce-rtx-50-series-gpus/

- RT cores intersect a **thick 3D line with rounded ends** in hardware.
- One vertex per segment (vs ≥2 for quads, more for tubes).
- ~2× faster and ~5× less BVH memory than DOTS triangles on animated hair.
- DX12 path: **NVAPI R570+**, not stock D3D12. Vulkan: `VK_NV_ray_tracing_linear_swept_spheres`.
- OptiX uses LSS automatically on 50-series.

**For grass:** a blade *is* a tapered capsule chain. Two LSS segments per blade
(base→mid, mid→tip) is a volumetric primitive the 5080 was built to trace.
This is the single highest-leverage 5080-specific change.

Fallback on older GPUs: DOTS (disjoint orthogonal triangle strips) with
analytic shading normals — same paper.

---

## 2. Do not put 10M independent AABBs in one BLAS

### RTX Mega Geometry / cluster CLAS

Kraemer, Lacewell, Kanani. SIGGRAPH Real-Time Live 2025.
*Real Time Path-Tracing with NVIDIA RTX MegaGeometry.*
ACM 10.1145/3721243.3735983
https://developer.nvidia.com/blog/nvidia-rtx-mega-geometry-now-available-with-new-vulkan-samples/

Also: `VK_NV_cluster_acceleration_structure`,
`nv_cluster_builder`, `nv_lod_cluster_builder`,
https://github.com/NVIDIA-RTX/RTXMG

- Split geometry into **clusters (CLAS)**. BLAS *references* clusters instead
  of copying every triangle.
- Animated / wind-bent geometry: rebuild only dirty clusters (up to 100×
  faster AS updates).
- Continuous LOD: pick cluster groups per camera, seamless across a mesh.
- Blackwell RT cores intersect **clusters**, not just triangles.
- Witcher 4 foliage demo: individual needles, some trees >10M triangles,
  path-traced with Mega Geometry + DLSS.

**For grass:** one cluster = one tuft / one 0.5 m cell (64–128 blades).
Wind updates rebuild that cluster, not 10M primitives. Distant cells swap
to a cheaper cluster LOD instead of testing every blade.

Related: `VK_NV_partitioned_acceleration_structure` — rebuild only the
TLAS tiles that moved (their 100k-physics sample). Same idea as our
spatial 20×13 grid, but in the driver.

---

## 3. Vegetation-specific path tracing (2026)

### Real-time Path Tracing of Massive Dynamic Foliage

van Antwerpen, Gautron, et al. PACMCGIT 9(4), June 2026.
ACM 10.1145/3820021
https://dl.acm.org/doi/10.1145/3820021

End-to-end real-time path tracing of **massive animated foliage beyond
close range**, on modern HW RT. This is the closest peer-reviewed system
to “10 million blades, each getting bounces.”

**Takeaways to steal:** hardware RT + clustered AS + animation that does
not rebuild the world every frame + LOD past close range.

---

## 4. Geometric blades, not cards (the grass classics)

### Jahrmann & Wimmer, i3D 2017

*Responsive Real-Time Grass Rendering for General 3D Scenes.*
ACM 10.1145/3023368.3023380
https://www.cg.tuwien.ac.at/research/publications/2017/JAHRMANN-2017-RRTG/

- Every blade is real geometry (Bezier / tessellation), not a billboard.
- Length-preserving wind (control points, not a squash).
- Arbitrary terrain alignment.

GPUOpen 2024 mesh-shader grass is an implementation of this paper:
https://gpuopen.com/learn/mesh_shaders/mesh_shaders-procedural-grass-rendering/

### Boulanger, Pattanaik, Bouatouch — JCGT 2015

*Real-Time Grass (and Other Procedural Objects) on Terrain.*
https://jcgt.org/published/0004/01/02/

Tessellation-driven per-plant geometry on heightfields.

### Habel, PhD TU Wien 2009

*Real-time Rendering and Animation of Vegetation.*
Ray-traced grass chapter (texels / volumetric blades).

### Perbet & Cani, 2001

*Animating Prairies in Real-Time.*
Procedural trajectories for grass particles. Still the wind model
ancestor.

**For us:** keep Bezier/ribbon blades (already in the overlay +
intersection). Replace software ribbon tests with LSS or clustered
triangles. Keep Jahrmann length-preserving wind when we refit.

---

## 5. Stop paying for any-hit on every blade pixel

### Opacity Micro-Maps + BLAS compaction (measured on RTX 5080)

Bavoil, NVIDIA / MachineGames (2025).
*Path Tracing Optimizations in Indiana Jones: Opacity MicroMaps and
Compaction of Dynamic BLASs.*
https://developer.nvidia.com/blog/path-tracing-optimizations-in-indiana-jones-opacity-micromaps-and-compaction-of-dynamic-blass/

On an **RTX 5080**, Peru jungle, 1080p path trace (4K DLSS-RR):

| Pass         | OMM off | OMM on | Delta |
|--------------|---------|--------|-------|
| TraceMain    | 7.90 ms | 3.58 ms | **−55%** |
| SHARC update | 1.99 ms | 0.89 ms | −55% |
| Sun tracing  | 0.83 ms | 0.71 ms | −14% |

AHS samples in TraceMain: 17% → 3%.

Dynamic vegetation BLAS compaction: **1027 → 606 MB (−41%)**.
Works on *updated* BLASes if you never full-rebuild after compact.
Flags: `PREFER_FAST_BUILD | ALLOW_UPDATE | ALLOW_COMPACTION`.

DXR 1.2 or NVAPI. Vulkan: `VK_EXT_opacity_micromap`.

**For grass:** if we stay on thin ribbons / alpha tips, bake 4-state OMMs
(`OC1_4_State`, subdiv 6). Force 2-state OMM on bounce rays
(`ForceOpacityMicromap2State`) so indirect never runs any-hit.
If we switch to LSS, OMMs matter less (opaque capsules).

---

## 6. Reorder divergent grass hits

### Shader Execution Reordering

Microsoft DXR 1.2 (Agility SDK, GDC 2025).
https://devblogs.microsoft.com/directx/ser/

Bavoil — *Indiana Jones: SER and Live State Reductions* (first post).
TraceMain **−25%** from SER + shrinking live state in the raygen.

Remedy, Alan Wake 2, GDC 2025: SER + OMM, 37M rays/frame,
**16.8 → 10.2 ms (−39%)** on RTX 4090.

HLSL: `MaybeReorderThread` with a coherence hint (material / blade
species / hit distance).

**For grass:** after `ReportHit` / in closest-hit, reorder on
`(bladeSpecies, hitT bucket)` so 10M unique blades do not thrash
warps. This is why 100% util at 98 W still looked “empty” — occupancy
died on divergence, not on RT throughput.

---

## 7. One sample, many reused paths (so 1 spp looks finished)

### ReSTIR DI — Bitterli et al., SIGGRAPH 2020

*Spatiotemporal Reservoir Resampling for Real-time Ray Tracing with
Dynamic Direct Lighting.*
ACM TOG 39(4). https://cs.dartmouth.edu/~wjarosz/publications/bitterli20spatiotemporal.html

6–60× equal-error vs then-SOTA many-light sampling. No extra data
structure beyond per-pixel reservoirs.

### ReSTIR GI — Ouyang et al., 2021

Reuse **indirect** path vertices in screen space. This is how 1 spp
path tracing looks like 16 spp on grass.

### Volumetric ReSTIR — Lin et al., 2021

Same idea inside participating media (fog / wet grass air).

**For the video claim** “each blade receives individual path-traced
bounces”: fire one real bounce per visible fragment, then **resample
neighbor/previous-frame reservoirs** so the image converges while
every sample is still a traced path. Wind forces careful
confidence-kill when the blade moved.

---

## 8. Cut bounce cost with a radiance cache

### Neural Radiance Caching — Müller, Rousselle, Novák, Keller, SIGGRAPH 2021

https://arxiv.org/abs/2106.12372

Tiny fully-fused MLP + hash grid, trained online, queried after the
first bounce. Unbiased if you mix cached and fresh paths.

### SHARC (NVIDIA RTX)

Used in Indiana Jones: up to 4 bounces update the cache, TraceMain
does 2 bounces and reads the cache. Same split we want:
**near grass = full bounce, rest = cache**.

Blackwell Tensor Cores (FP8/FP4) make NRC cheaper than on Ada.

---

## 9. Reconstruct instead of tracing more pixels

DLSS 4 Super Resolution + **Ray Reconstruction** (50-series).
Path-trace at 1080p (or 720p), present 1440p/4K.
Indiana Jones 5080 numbers above are **4K out, 1080p path trace**.

Without DLSS, a 1080p 10M-blade path trace will look noisy *or* die.
The paper-backed production setup is: **internal 1080p PT + RR**.

---

## What we should actually implement (priority)

| Priority | Technique                         | Makes the YouTube line true how                          | API we have      |
|----------|-----------------------------------|----------------------------------------------------------|------------------|
| P0       | Spatial cluster BLASes + tight AABBs | 10M blades exist; rays only walk nearby clusters       | D3D12 DXR 1.1    |
| P0       | SER (`MaybeReorderThread`)        | 5080 RT cores stay coherent on unique blades             | DXR 1.2 / NVAPI  |
| P1       | LSS capsules per blade            | Hardware volumetric primitive, not software ribbons      | NVAPI R570       |
| P1       | OMM on ribbon fallback            | −50% any-hit like Indy on a 5080                         | DXR 1.2 / NVAPI  |
| P1       | ReSTIR GI                         | 1 spp still looks like path-traced bounces               | custom compute   |
| P2       | Mega Geometry CLAS                | Wind refit 100×; true Blackwell cluster intersection     | Vulkan NV ext    |
| P2       | NRC / SHARC                       | Far bounces without 10M closest-hits                     | custom / RTX Kit |
| P2       | DLSS-RR                           | Fluid 1080p-in / 4K-out                                  | Streamline       |
| P3       | Partitioned TLAS                  | Only dirty grass tiles refit                             | Vulkan NV ext    |

Stock D3D12 (what this repo uses today) can do **P0** fully and **P1 SER/OMM**
if we add the Agility SDK. LSS and Mega Geometry CLAS are NVIDIA
extensions — worth it for a 5080-only YouTube card, but they are not
portable DXR 1.1.

---

## One-sentence mapping to the voiceover

> “Ten million volumetric blades” → Mega Geometry / clustered BLAS + LSS
> capsules (Blackwell RT cores).
> “Each receiving individual path-traced light bounces” → one traced bounce
> per fragment + ReSTIR GI / NRC so it converges at 1 spp.
> “Thermals and frame-time stability” → already on the HUD (NVML).
> “Choke or stay fluid” → SER + OMM + internal-1080p PT + DLSS-RR, which is
> exactly how shipped 5080 path-traced games keep vegetation alive.
