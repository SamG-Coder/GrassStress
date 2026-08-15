# DLSS 4 / 4.5, Streamline, and RTX Kit for this stress test

What shipped after the 5080 launch, what buffers we must emit, and how it
makes the voiceover honest *and* watchable.

## The problem this stack solves

Internal path-traced lighting of 10M blades is ~4 FPS at 1080p native
(bench: 3.97 FPS, 100% GPU, 136 W). That is the *breaking point*. DLSS
does not reduce the 10M-blade AS or the bounce rays. It lets us:

- Path-trace at 720p / 1080p **internal**
- Reconstruct noisy 1-spp bounces into a clean 1440p / 4K image
- Generate 2–6 display frames per rendered frame on a 5080
- Keep Reflex so generated frames do not feel like slideshow input lag

Shipped path-traced games on 5080 (Indiana Jones, Cyberpunk Overdrive)
use exactly this split: **expensive PT at low internal res + RR + MFG**.

---

## 1. DLSS 4 (CES 2025) — official paper

NVIDIA ADLR, 2025.
*Technical Insights into DLSS 4: Multi Frame Generation and Transformer-Based Architectures*
https://research.nvidia.com/labs/adlr/DLSS4/

### Multi Frame Generation (50-series exclusive)

- DLSS 3: one generated frame between two rendered (~3.25 ms / 4K on 4090).
- DLSS 4 MFG: **up to 3 generated frames per rendered frame** (4×).
- Split network: a large half runs once per *input pair*; a tiny half
  runs once per *output frame* (~1 ms / generated frame on 5090).
- Optical-flow OFA is **not** the primary interpolator anymore; an extra
  AI net predicts transitions. Engine motion vectors + depth still matter.
- Blackwell **hardware flip metering** paces the extra frames so 240/360 Hz
  does not hitch.
- UI, particles, specular: MVs are often *wrong*. The net learns when
  to ignore them. HUD must be tagged separately (`Hudless` + `UI Color`).

### Transformer Ray Reconstruction

- Replaces NRD / hand-tuned denoisers **and** does super-res in one net.
- Old pipeline: denoise at low res → upsample. Shading stayed blurry.
- RR: noisy path-traced buffer goes in at the *output* mindset; the
  transformer attends across space and time to missing samples.
- CNN RR hit a wall (ghosting, painterly). Custom transformer:
  **4× compute, 2× params**, same frame budget, FP8 on Ada/Blackwell
  tensor cores.
- Trained across Halton / white / blue / QMC **and ReSTIR** correlations.
- Better: surface detail, disocclusion, hair/skin, temporal stability.

### Transformer Super Resolution

Same backbone as RR. Sharper foliage in motion, less wobble on edges,
less over-sharpen. Important for thin grass silhouettes.

### Reflex Frame Warp

Late-stage warp of the *already displayed* frame using the newest camera
input. Claims up to **75%** added latency cut on top of Reflex.
Not a substitute for Reflex; FG *requires* Reflex.

---

## 2. DLSS 4.5 (CES 2026 / spring 2026)

https://www.nvidia.com/en-us/geforce/news/dlss-4-5-dynamic-multi-frame-gen-6x-2nd-gen-transformer-super-res/

| Piece | What it is | 5080? |
|---|---|---|
| 2nd-gen transformer SR | 5× compute of 1st transformer, better lighting/edges/motion | Yes (all RTX) |
| **6× Multi Frame Generation** | 5 generated frames per rendered | **50-series only** |
| **Dynamic MFG** | Auto-picks 2×–6× to hit refresh / a target FPS | **50-series only** |
| Updated RR | Same 4.5 transformer family | Yes |

Dynamic MFG is the one that matches the voiceover: when the PT pass
drops to 4–15 FPS in the grass, the 5080 **raises the generate count**
so the display still looks like a “fluid masterpiece” while the HUD
shows the *real* rendered frame time (must label that).

SDK: Streamline **2.12+ / DLSS 4.5 SDK** (April 2026).
https://wccftech.com/nvidia-dlss-4-5-sdk-now-available-enabling-devs-to-integrate-dynamic-frame-gen-in-games/

---

## 3. How you actually hook it: Streamline

https://developer.nvidia.com/rtx/streamline
https://developer.nvidia.com/blog/how-to-integrate-nvidia-dlss-4-into-your-game-with-nvidia-streamline/
https://github.com/NVIDIAGameWorks/Streamline

DX12 and DX11. One interposer (`sl.interposer.dll`, signature-checked)
plus feature DLLs (`nvngx_dlss.dll`, `nvngx_dlssd.dll`, FG plugin).

### Buffers this project must start emitting

| Buffer | SR | RR | MFG | Notes for GrassStress |
|---|---|---|---|---|
| Color (HDR, pre-UI) | yes | yes | yes | Path-traced + grass, **before HUD** |
| Depth | yes | — | yes | Hardware depth we already have |
| **Linear depth** | — | **required** | — | `kBufferTypeLinearDepth`, not HW Z |
| Motion vectors | yes | yes | yes | Per-pixel 2D. **We do not have these yet** |
| Specular MVs | — | yes | — | Optional but RR wants them for wet grass |
| Exposure | yes | yes | — | Auto-exposure flag or a scalar each frame |
| Jitter | yes | yes | — | Halton/R2 in RayGen. Camera reset on cut |
| Hudless color | — | — | **required** | Same res as backbuffer, no HUD |
| UI color/alpha | — | — | **required** | HUD/title only, same extent as backbuffer |
| Inverted-Z bit | if used | if used | if used | We use standard Z |

Without motion vectors, SR/RR/MFG will ghost every wind-blown blade.
That is the #1 integration hole in the current renderer.

### Pipeline order (NVIDIA checklist)

1. Simulate / cull
2. Path trace + grass (jittered, low internal res)
3. **No NRD** if RR is on (RR replaces denoisers)
4. DLSS RR+SR as the *first* post (or RR unified)
5. Remaining post (we have almost none)
6. HUD / title **after** RR, into UI buffer for FG
7. Reflex + DLSS FG / Dynamic MFG at Present
8. Disable FG in menus, resize, pause

Mip bias must be set when RR/SR is on or ground/grass textures smear.

Reflex is **mandatory** with FG. Halve motion-blur magnitude if we add any.

`numFramesToGenerate` = multiplier − 1  
(2× → 1, 4× → 3, 6× → 5).

---

## 4. Rest of RTX Kit (not DLSS, still 5080-relevant)

https://developer.nvidia.com/rtx-kit

| Piece | Role next to 10M blades |
|---|---|
| **NRD** | Hand-tuned RT denoiser. Use **only if RR is off**. Do not stack with RR. |
| **RTXDI / ReSTIR** | Many-light / GI resampling. Pair with RR (paper trained on ReSTIR noise). |
| **SHARC / NRC** | World-space radiance cache after bounce 1. Cuts far-blade GI cost. |
| **RTX Mega Geometry** | Cluster CLAS, wind-refit. Vulkan NV ext today. |
| **LSS (RTX Hair)** | Hardware capsules. NVAPI R570. Best *primitive* for a blade. |
| **OMM** | Hardware alpha. DXR 1.2 / NVAPI. |
| **SER** | `MaybeReorderThread` after grass hits. DXR 1.2. |
| **Path Tracing SDK** | Reference glue for the above. |
| **Reflex / Frame Warp** | Latency under MFG. Frame Warp is late reprojection + inpaint. |

---

## 5. Mapping to the voiceover

> Ten million volumetric grass blades, each receiving individual
> path-traced light bounces … thermals and frame-time … choke or
> fluid masterpiece.

| Claim | Who does the work |
|---|---|
| 10M volumetric blades | Our AS + LSS/clusters (not DLSS) |
| Individual PT bounces | Our TraceRay / RayQuery (not DLSS) |
| Looks finished at 1 spp | **Ray Reconstruction transformer** |
| Display feels fluid on a 5080 | **Dynamic MFG 2×–6×** on top of a 4–15 Hz PT |
| Thermals / 1% low | Our NVML HUD must show **rendered** ms, not FG FPS |
| Not a cheat | HUD labels `PT 4.1 FPS` vs `DISPLAY 144 FPS (DLSS 4.5 6×)` |

If the HUD only shows the Present rate, the video is lying. Dual meters
are how Digital Foundry and NVIDIA’s own Indy 5080 numbers are reported
(4K out, 1080p path-trace).

---

## 6. Implementation order if we wire this next

1. Emit **jittered** camera, **linear depth**, and **motion vectors**
   (blade tip/base this frame vs last, including wind).
2. Split HUD off the PT target (`Hudless`).
3. Integrate Streamline: Reflex → SR → RR → FG.
4. Path-trace at 720p or 1080p Quality/Performance; present 1440p/4K.
5. Turn Dynamic MFG on for `--record` / 5080.
6. HUD: `RENDER fps/ms/1%` + `DISPLAY fps` + temp/power.
7. Only then consider LSS/SER/OMM (geometry), which DLSS cannot replace.

DLSS 4.5 SDK + Streamline 2.12 is the integration surface.
Do **not** ship NGX watermarked DLLs. Do **not** leave NRD on under RR.
