# RTX 5080 vs 10,000,000 Volumetric Grass Blades

YouTube stress test aligned to the voiceover:

> Ten million volumetric grass blades, each receiving individual
> path-traced light bounces, with live thermals and frame-time monitoring
> on an RTX 5080.

What that means in this build:

- 10,000,000 blade AABBs in a spatial DXR BLAS (every blade exists)
- Raster ribbons so the meadow actually draws
- Every grass fragment traces a sun shadow **and** a bounce into that 10M AS
- Ground path-tracing also sees the blades (mask 0x3)
- HUD: blade count, FPS, ms, 1% low, °C, GPU %, watts, VRAM, STABLE/CHOKING

`--bench N` writes `video/bench.txt` after N frames.

`--offline [spp] [shots]` freezes the cinematic, accumulates path-traced samples
per camera, then dumps PNGs and encodes them with NVENC. Default is 16 spp and
48 shots.

The live path tracer only keeps the RT cores busy. This build also occupies
the leftover silicon:

| Unit | What uses it |
|---|---|
| RT cores | DXR path trace + per-blade sun/bounce `RayQuery` |
| CUDA / SM cores | A-trous firefly denoise compute (`sm_denoise.hlsl`) |
| Tensor cores | DLSS SR in realtime; OptiX HDR denoise on `--offline` dumps |
| Copy engine | Dedicated D3D12 COPY queue for PNG readback |
| NVENC | `ffmpeg -c:v h264_nvenc` after the offline sequence |

## Record

```text
build.cmd
run.cmd
```

The window opens at 1920×1080 with an auto-playing cinematic. OBS Game Capture
the window. VSync is off so the HUD shows the real frame time.

High-spp offline B-roll (uses SMs + tensor denoise + NVENC):

```text
run.cmd --offline 12 36
```

Frames land in `video/offline/` and the encode is
`video/RTX5080-10M-Grass-OFFLINE.mp4`.

## Controls

| Key | Action |
|---|---|
| `E` | Cycle paper experiments (R2 / fiber / hash GI / ReSTIR / PSF / cascades / stack) |
| `C` | Toggle cinematic / free orbit |
| `F2` | First-person walk through the field |
| `H` / `F1` | HUD on/off |
| `V` | VSync on/off |
| `F11` | Borderless fullscreen |
| `R` | Restart cinematic + title card |
| `1`–`4` | Sunrise / noon / golden hour / night |
| Left/Right | Nudge time of day |
| `W` | Toggle wind (orbit/cinematic) |
| WASD + mouse | Walk in first person |
| Escape | Release mouse / quit |

## What the number means

- 400 × 250 meadow patches
- 100 volumetric blades per patch
- **10,000,000** blades in the DXR grass BLAS
- Each blade is a wind-animated, two-plane ribbon tested by the intersection shader
- Primary rays, sun shadows, and the existing bounce path all hit grass

Paper experiments (`E`) are documented in `docs/OBSCURE_PAPERS.md`.

## Toolchain

Same as DenseTrees: MSYS2 UCRT64 GCC/CMake/Ninja and `Microsoft.DirectX.ShaderCompiler`.
