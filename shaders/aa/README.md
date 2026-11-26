# Anti-aliasing shaders

Spatial tier T1 (experimental): a pattern-based morphological filter, written from the published descriptions of MLAA
(Reshetov, Intel, 2009) and SMAA (Jimenez et al., 2011). No third-party shader code is used.

| File | Pass | Output |
|---|---|---|
| `aa_edge_ps.hlsl` | luma edge detection, local-contrast adaptation, thin-stroke mask | RGBA8: R vertical edge, G horizontal edge, B thin stroke |
| `aa_weights_ps.hlsl` | run-length search along each edge, coverage weight at the two ends | RGBA8: R north, G south, B west, A east |
| `aa_blend_ps.hlsl` | neighbourhood blend, edge/weight/split debug views | final image |
| `aa_stats_cs.hlsl` | hard-edge density on a 128x128 sample grid | two counters |

`aa_common.hlsli` holds the 64-byte `AaConstants` block (mirrored in `src/aa/aa_pass.cpp`) and the content clamp. The
filter runs at the resolution it receives: after upscaling (default) at output resolution, or before it at game
resolution.

Reference implementations kept for comparison must stay outside this directory, under `tools/aa_lab/_reference/`
(git-ignored), and are never compiled into MoonSR.
