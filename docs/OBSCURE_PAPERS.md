# Obscure / unimplemented science now running as experiments

These are **not** the Mega Geometry / DLSS / SER notes already in
`RTX5080_PAPERS.md`. They are newer or rarely-shipped algorithms that
this stress test did not have. Press **E** in the live app to cycle them.
**7 STACK** runs every one of them on the same frame so RT cores, SMs,
and tensor-side OptiX all have extra math to chew.

| Key | Id | Paper | Year | What we actually run |
|---|---|---|---|---|
| BASE | 0 | control | — | existing hybrid PT + A-trous |
| R2QMC | 1 | Roberts, *The Unreasonable Effectiveness of Quasirandom Sequences* | 2018 | R2 plastic Kronecker + Cranley–Patterson instead of LCG hash |
| FIBER | 2 | Kajiya–Kay SIGGRAPH 1989; Marschner et al. SIGGRAPH 2003; Zinke dual-scattering 2008 | 1989–2008 | blade = dielectric cylinder (KK diffuse + Marschner R) |
| HASHGI | 3 | Müller Instant-NGP hash 2022; NVIDIA SHARC / NRC 2021; spatial-hash GI | 2021–22 | world-space hash of bounce irradiance, 8-tap query |
| RESTIR | 4 | Bitterli et al. TOG 2020; Ouyang ReSTIR GI 2021; Lin et al. GRIS SIGGRAPH 2022 | 2020–22 | screen reservoirs, RIS over a 3×3, temporal from MVs |
| PSF | 5 | Binder, Fricke, Keller, *Massively Parallel Path Space Filtering* arXiv:1902.05942 | 2019/21 | quantize path vertices, share radiance across nearby hashes |
| CASCADES | 6 | Sannikov Radiance Cascades 2023; **Freeman & Sannikov, Split Radiance Cascades, arXiv:2607.20384, 22 Jul 2026** | 2023–26 | screen-space cascades; interval by hit distance (the “split” idea) |
| STACK | 7 | all of the above | — | R2 + fiber in the tracer, then hash + ReSTIR + PSF + RC on SMs |

## Why these, not another DLSS blog post

- **Split Radiance Cascades (Jul 2026)** is three weeks old as of this
  build. No shipping path tracer uses the sparse-hash + ray-splitting
  form. We run the screen-space cousin: cascade interval chosen from
  reconstructed hit distance.
- **GRIS / ReSTIR PT** is the math underneath “1 spp looks like 16 spp”.
  The repo only fired one cosine bounce and hoped temporal AA would hide
  it. Reservoirs reuse that bounce across neighbors.
- **Hashed path-space filtering** is Keller’s “share vertices, don’t
  trace more” line. Almost nobody wires it into a grass raster+DXR
  hybrid.
- **R2** is one of the cheapest sampling upgrades that is still
  *wrong* in most game path tracers (they keep `hash(pixel+frame)`).
- **Fiber BSDF** is the correct model for a blade. Games use a Lambertian
  card. Marschner’s R lobe is why wet grass has a grazing sheen.

## Papers we read and did *not* ship (API / time)

| Paper | Why not in this MinGW DXR 1.1 build |
|---|---|
| 3DGRT — Moenne-Loccoz et al., SIGGRAPH Asia 2024 | Needs OptiX particle AS, not DXR 1.1 ribbons |
| Mega Geometry CLAS / LSS | NVAPI / Vulkan `VK_NV_*`, no Windows SDK here |
| Neural Importance Sampling of Many Lights, arXiv 2505.11729 (May 2025) | Needs a trained net + many area lights |
| Holographic Radiance Cascades, May 2026 | 2D walkthrough method; we took Split RC instead |
| Improved Stochastic Texture Filtering, arXiv 2504.05562 (Apr 2025) | Ground textures already mip-filtered; little grass win |
| Cooperative-vector NRC (RTX Kit) | MSVC + Agility SDK |

## How to run

```text
run.cmd
```

`E` cycles experiments. The HUD fourth line names the paper. `--bench`
and `--offline` keep whatever mode you last set; default is BASE.

STACK is the “use every leftover unit” mode: RT cores still bounce,
SMs run four reconstructive algorithms, OptiX tensors still fire on
`--offline` dumps, NVENC still encodes.
