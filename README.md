# GrassStress

A native Windows DirectX 12 / DXR meadow that puts **60,000,000** wind-animated grass blades into a real acceleration structure and path-traces them. Built as an RTX 5080 stress test: every blade exists, every visible fragment traces a sun shadow and a bounce, and the leftover silicon (SMs, tensor cores, NVENC, copy engine) is kept busy on purpose.

![Daylight meadow, 60 million path-traced blades](docs/screenshots/meadow.jpg)

*Live capture on an RTX 5080: 60,000,000 blades, path-traced bounces, DLSS Super Resolution, ~25 path-traced FPS at 1080p.*

![Night meadow with the same 60M blade field](docs/screenshots/night.jpg)

*Same field at night. Keys `1`–`4` jump sunrise / noon / golden hour / night; arrows nudge the clock.*

## What you are looking at

The meadow is not a tiled texture or a few thousand cards. It is a spatial field of individual blades:

| Piece | Count |
|---|---|
| Meadow patches | 750 × 500 = **375,000** |
| Blades per patch | **160** (a mix of short turf and taller stems) |
| Blade AABBs in the grass BLAS | **60,000,000** |
| What a blade is | A wind-bent two-plane ribbon, tested by the DXR intersection shader |

The ground is a heightfield. Wind, sun, and moon come from a CPU environment simulation that the shaders read every frame. Walking through the field (`F2`) pushes a local grass wake — blades lean away from your feet without a CPU geometry rewrite.

The HUD reports blade count, path-traced FPS, display FPS (after DLSS / frame gen), frame time, 1% low, GPU temperature, utilization, watts, VRAM, and whether the machine is `STABLE` or `CHOKING`.

## How it works

GrassStress is a **hybrid raster + ray-tracing** path, not a pure offline tracer.

```text
CPU field (60M blade AABBs + 375k patches)
        │
        ▼
  DXR grass BLAS  ─────────────┐
        │                      │
        ▼                      ▼
  raytracing.hlsl         grass_overlay.hlsl
  (ground + sky +         (instanced ribbons
   bounce into the         so the meadow
   60M-blade AS)           actually draws)
        │                      │
        └──────────┬───────────┘
                   ▼
           sm_denoise.hlsl     (A-trous / experiment filters on SMs)
                   ▼
           Streamline DLSS     (SR in realtime; RR optional)
                   ▼
           present_tonemap     → swap chain
                   │
                   └── --offline → OptiX HDR denoise → PNG → NVENC
```

1. **Field build.** `grass::build()` stamps a 750×500 patch grid and expands every patch into per-blade AABBs. Those AABBs are the DXR procedural primitives — the intersection shader reconstructs the curved ribbon from the patch seed, so the BLAS stays compact.
2. **Primary lighting.** `raytracing.hlsl` path-traces the ground and sky. Primary rays, sun shadows, and the bounce path all hit grass (instance mask `0x3`). The ground therefore receives contact shadows and bounced green from the canopy, not a fake AO term.
3. **Visible meadow.** `grass_overlay.hlsl` rasterizes the ribbons so you can see the sward. Each fragment fires `RayQuery` for a sun/moon shadow **and** a bounce into that same 60M-blade AS. The overlay also writes depth, motion vectors, and G-buffer so DLSS has something honest to reconstruct.
4. **Denoise / upscale.** An A-trous compute pass (`sm_denoise.hlsl`) eats SM time. NVIDIA Streamline then runs DLSS Super Resolution (and optionally Ray Reconstruction / Multi Frame Generation) on the tensor cores.
5. **Present.** `present_tonemap.hlsl` tone-maps and grades the HDR buffer to the swap chain. VSync is off by default so the HUD shows real frame time.

`--offline` freezes the cinematic, accumulates samples per camera, runs OptiX HDR denoise on the tensor cores, dumps PNGs on a dedicated D3D12 COPY queue, and encodes with `ffmpeg -c:v h264_nvenc`.

### What uses which unit

| Unit | What uses it |
|---|---|
| RT cores | DXR path trace + per-blade sun/bounce `RayQuery` |
| CUDA / SM cores | A-trous denoise and the paper-experiment reconstructors |
| Tensor cores | DLSS SR (live); OptiX HDR denoise on `--offline` dumps |
| Copy engine | Dedicated D3D12 COPY queue for PNG readback |
| NVENC | `ffmpeg -c:v h264_nvenc` after an offline sequence |

Press **E** to cycle paper experiments (R2 QMC, fiber BSDF, hashed GI, ReSTIR, path-space filtering, radiance cascades, or all of them stacked). Those are documented in [`docs/OBSCURE_PAPERS.md`](docs/OBSCURE_PAPERS.md). Blackwell / DLSS 4 notes live in [`docs/RTX5080_PAPERS.md`](docs/RTX5080_PAPERS.md) and [`docs/DLSS4_AND_RTX_KIT.md`](docs/DLSS4_AND_RTX_KIT.md).

## Build

Windows only. Same toolchain as [DenseTrees](https://github.com/SamG-Coder/DenseTrees):

- MSYS2 UCRT64 GCC, CMake, Ninja (`C:\msys64\ucrt64\bin`)
- [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) (`winget install Microsoft.DirectX.ShaderCompiler`)
- An NVIDIA GPU with DXR 1.1 (RTX 20-series or newer). The 60M-blade field is sized for a 16 GB RTX 5080.

```text
build.cmd
run.cmd
```

`build.cmd` compiles the HLSL with DXC, configures CMake/Ninja, links `GrassStress.exe`, and runs `grass_field_tests` (the field must emit exactly 60,000,000 blades).

Optional Streamline / DLSS needs the NVIDIA Streamline SDK unpacked under `third_party/streamline` and, for the native G-buffer hook, MSVC to build `sl_bridge.dll`. Without that, the native path still runs — you just lose DLSS.

## Run

```text
run.cmd
```

The window opens at 1920×1080 with an auto-playing cinematic. OBS Game Capture the window. VSync is off so the HUD is honest.

| Command | What it does |
|---|---|
| `run.cmd` | Live cinematic |
| `run.cmd --bench 120` | Run 120 frames, write `video/bench.txt`, quit |
| `run.cmd --offline` | Frozen cameras, 16 spp, 48 shots, NVENC encode |
| `run.cmd --offline 12 36` | 12 spp, 36 shots |

Offline frames land in `video/offline/`. The encode is `video/RTX5080-10M-Grass-OFFLINE.mp4`.

## Controls

| Key | Action |
|---|---|
| `C` | Toggle cinematic / free orbit |
| `F2` | First-person walk through the field |
| WASD + mouse | Walk (first person). Shift sprint, Space jump |
| Left-drag / wheel | Orbit and zoom (orbit mode) |
| `E` | Cycle paper experiments |
| `D` | Cycle DLSS quality (Off → Quality → Balanced → Performance → Ultra → DLAA) |
| `F` | Cycle frame generation (Off → 2× → 4× → 6× → Dynamic) |
| `H` / `F1` | HUD on/off |
| `V` | VSync on/off |
| `F11` | Borderless fullscreen |
| `R` | Restart cinematic + title card |
| `1`–`4` | Sunrise / noon / golden hour / night |
| Left / Right | Nudge time of day |
| `W` | Toggle wind (orbit / cinematic) |
| Escape | Release mouse, or quit |

## Layout

| Path | Role |
|---|---|
| `src/grass_field.cpp` | Deterministic 60M-blade meadow |
| `src/environment_simulation.cpp` | Sun, moon, wind, fog, time of day |
| `src/dxr_renderer.cpp` | DX12 device, BLAS/TLAS, frame graph |
| `src/streamline_host.cpp` | DLSS / Reflex / frame-gen via Streamline |
| `src/optix_denoise.cpp` | Offline OptiX HDR denoise |
| `src/gpu_telemetry.cpp` | Temperature, power, VRAM, clocks for the HUD |
| `shaders/raytracing.hlsl` | Path tracer + grass intersection |
| `shaders/grass_overlay.hlsl` | Raster ribbons + inline `RayQuery` |
| `shaders/sm_denoise.hlsl` | SM reconstructors |
| `shaders/present_tonemap.hlsl` | Display map |
| `obs/` | OBS scene + YouTube letterbox overlays |

## License

Application source is published as-is for the stress-test writeup. NVIDIA Streamline, NGX, and OptiX remain under their own NVIDIA licenses in `third_party/` and are not redistributed as part of this description.
