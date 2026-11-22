# MoonSR shader conventions

## Layout

```
shaders/
  CMakeLists.txt            build list (one moonsr_add_shader line per shader)
  common/
    scale_common.hlsli      shared contract: constant buffer, bindings, helpers (owned by the lead)
    fullscreen_vs.hlsl      fullscreen vertex shader producing VsOut
  scale/                    scaling and filter pixel shaders (*_ps.hlsl)
```

Shaders include the contract with `#include "common/scale_common.hlsli"` (the build passes `/I shaders`).
Each pixel shader has entry point `main`, signature `float4 main(VsOut i) : SV_Target`, and returns alpha 1.

## The ScaleConstants contract

`cbuffer ScaleConstants : register(b0)` is defined in `common/scale_common.hlsli`. Its C++ mirror is the
`ScaleConstants` struct in `src/upscale/scale_pass.cpp`. Never change one without the other, and state any
change in your report. Fields: `srcTexSize`, `srcTexelSize`, `srcRectOrigin`, `srcRectSize`, `dstSize`,
`dstInvSize`, `sharpness` (0..1), `param1..param3` (shader specific).

Bindings: `srcTex` t0, `linearClamp` s0, `pointClamp` s1.

## Coordinates

- `i.uv` is 0..1 across the destination rectangle (the viewport).
- `SrcPosTexels(uv)` = `srcRectOrigin + uv * srcRectSize`: a continuous position in source texels.
  Texel centres are at integer + 0.5, so texel index `t` has its centre at `t + 0.5`.
  To get the texel index grid use `pos - 0.5` and `floor`.
- `ClampToSrcRect`, `SampleSrcLinear` (bilinear, position clamped to the region's texel centres) and
  `LoadSrc` (integer load, clamped to the region) keep reads inside the source region. Always use them
  instead of raw `srcTex` access so sub-rect sources never bleed in neighbouring pixels.

## Two-stage flow: scale, then sharpen

The scaler renders either directly to the destination rect or, when sharpening is enabled, into an
intermediate at destination-rect resolution. The sharpen shader then runs 1:1 over that intermediate with
`srcRect` = the whole intermediate and `srcRectSize == dstSize`. Scalers therefore must not sharpen
themselves (except for the small fixed lobes inherent to their kernel); `sharpness` is consumed by the
sharpen shader.

## Registering a shader

Add one line in your own section of `shaders/CMakeLists.txt`:

```
moonsr_add_shader(scale/foo_ps.hlsl ps_5_0 g_ps_foo)
```

The header `g_ps_foo.h` is generated into `${CMAKE_BINARY_DIR}/shaders_gen` and defines `const BYTE g_ps_foo[]`.
Optional arguments: `ENTRY <name>`, `DEFINES A=1 B`.

## Compiling by hand (exactly as the build does)

```
fxc /nologo /T ps_5_0 /E main /O3 /WX /I shaders /Vn g_ps_foo /Fh %TEMP%\g_ps_foo.h shaders\scale\foo_ps.hlsl
```

Add `/Fc %TEMP%\foo.asm` to see "Approximately N instruction slots used". Warnings are errors (`/WX`).

## Shader Model 5.0 rules

- Target `ps_5_0` / `vs_5_0` / `cs_5_0` only; no SM 6 features without an explicit request.
- Use `SampleLevel`/`Load` (no implicit derivatives) in code with non-uniform control flow.
- Use `[unroll]` on fixed small loops. Keep constant buffer changes in sync with C++.
- Original code only; cite a general technique in a header comment when one is used.

## Gamma space

The captured image is gamma-encoded SDR BGRA8 and is **not linearised**. All filters operate on the encoded
values directly. Anti-ringing clamps and `Luma()` assume gamma-encoded input. Results may differ slightly from
linear-light filtering; this is intentional (matches common game-scaler behaviour and costs nothing).

## Shader table

| Shader | Variable | Role | Fetches | Notes |
|---|---|---|---|---|
| `common/fullscreen_vs.hlsl` | `g_vs_fullscreen` | vertex shader | 0 | fullscreen triangle |
| `scale/nearest_ps.hlsl` | `g_ps_nearest` | scaler | 1 point | |
| `scale/bilinear_ps.hlsl` | `g_ps_bilinear` | scaler | 1 bilinear | baseline |
| `scale/bicubic_ps.hlsl` | `g_ps_bicubic` | scaler | 9 bilinear + 4 point loads | Catmull-Rom, 16-tap kernel via 9 fetches, 75% anti-ringing |
| `scale/lanczos_ps.hlsl` | `g_ps_lanczos` | scaler | 36 point loads | Lanczos-3, 80% anti-ringing; no kernel widening when downscaling |
| `scale/sharpen_ps.hlsl` | `g_ps_sharpen` | sharpen (1:1) | 5 point loads | contrast-adaptive, `sharpness` 0 = exact pass-through |
