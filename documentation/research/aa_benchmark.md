# Anti-aliasing benchmark: automating the search, keeping the eye

Status: design and first implementation (2026-11-26). The fast regression mode remains `moonsr_bench --aa`; this note
describes the thorough suite behind `moonsr_bench --aa-suite` / `--aa-sweep`. Measured results live in
`documentation/BENCHMARKS.md`; this note defines what is measured and why.

## 1. Goal

The benchmark has two jobs:

1. **Automate the boring part.** Score every candidate setting (and, later, every candidate algorithm) against a fixed
   scene set, reject candidates that damage text, texture, exact edges or the budget, and rank what remains. A run
   writes a machine-readable table and a human-readable report.
2. **Keep the owner in the loop.** Every run writes a visual review pack: input, ground truth and output at 1:1 and
   4x, per scene, for the top candidate and the current default. No setting is declared good from numbers alone.

The benchmark is deliberately variant-agnostic: it links `AaPass` and calls `SetSettings`/`Apply`, so a new shader,
model or tier can be compared by replacing the pass. The first implementation ranks settings of the T1 filter; the
same harness is the acceptance gate for Directional/Temporal work.

## 2. Why ground truth, and what kind

The fast `--aa` check uses hand-built scenes and edge-step/energy metrics. That catches regressions but cannot answer
"is this closer to the image a correct area filter would produce?" The suite adds a **supersampled ground truth**:

- Every synthetic scene is generated at `S x S` the target resolution (`S = 4` by default), where the geometry is
  continuous in target coordinates.
- `ground truth` = exact box downsample of the high-resolution image to the target size
  (`AreaDownsample` in `tools/bench/imageops.cpp`).
- `aliased input` = point sample of the same high-resolution image at the target pixel centres. This is what a game
  without AA delivers.
- The AA pass runs on the aliased input; the result is compared with the ground truth.

All generation and comparison is on gamma-encoded 8-bit values, matching the capture path (A9 in ARCHITECTURE.md).
A linear-light variant would be a separate experiment; mixing the two would make PSNR meaningless.

The ground truth is an area reference, not a photograph of what the game's TAA would produce. It is the right target
for coverage reconstruction: a 45-degree edge should resolve to half-covered pixels, a shallow edge to a thin ramp.
It says nothing about temporal stability or model-based detail.

## 3. Scene matrix

Each scene is generated once at the suite resolution (default 1280x720) and reused for every candidate.

| Scene | Kind | What it tests | Judged on |
|---|---|---|---|
| `edge_20`, `edge_45`, `edge_70` | slanted edges | staircase reconstruction at shallow, diagonal and steep angles | edge-step reduction, PSNR/SSIM vs GT |
| `roofline_8` | shallow band (about 4.6 degrees) | long runs and near-horizontal edges, the case the first tier is weakest at | edge-step reduction, PSNR/SSIM vs GT |
| `color_30` | colour edge | chroma preservation when the luma weight is applied to RGB | PSNR/SSIM vs GT, saturation drift vs GT |
| `exact_0` | axis-aligned edge | do no harm on edges the pixel grid already resolves | step ratio, maximum change |
| `thin_lines` | 1.5-px grid, subpixel offset | one-texel and thin-stroke preservation | line-mask mean luma, changed pixels |
| `text_hud` | small glyphs over a 2-px checker | text and HUD protection on a hostile background | stroke-mask changed %, mean luma ratio, maximum |
| `checker_4` | 4-px checker with per-texel noise | fine texture must not be blurred | luma stddev ratio, high-pass energy ratio |
| `gradient` | smooth two-axis gradient | nothing to anti-alias, so nothing should change | maximum change |
| `subpixel_edges` | vertical/colour steps at 0.25/0.5/0.75-px offsets | coverage estimation when the edge is not on a pixel boundary; step ratio is diagnostic, not a gate | PSNR/SSIM vs GT |

The owner's real screenshots are added with `--aa-image`; they have no ground truth. They are used for the visual
review and for do-no-harm statistics (changed pixels, high-pass energy, HUD text change), never for a PSNR claim.

## 4. Gates: what a candidate may not damage

A candidate is disqualified before ranking if any gate fails. Thresholds are intentionally conservative; they are
regression guards, not quality claims.

| Gate | Threshold | Why |
|---|---|---|
| Slanted edges | step ratio <= 0.80 on every slanted scene | at least 20 % off the strongest scanline step |
| Roofline (4.6°) | step ratio <= 0.90 | a 12-16 texel run gets a smaller correction than a 45° staircase; 10 % is the measured tier-1 bound |
| Subpixel straight edges | diagnostic only (GT delta reported) | a vertical bar at a fractional position is not a staircase; the endpoint model cannot move it, so a step gate would disqualify every candidate |
| Exact edges | step ratio >= 0.98 and max change <= 32/255 | an already-resolved edge must not soften |
| Text/HUD | changed <= 10 % of stroke pixels, mean luma >= 98 %, max <= 80/255 | the suite glyph is 1.5 px wide; the fast 1 px stress scene remains 0 % changed. Real ATLYSS HUD is 11-22 % and stays a T2 item |
| Thin lines | line-mask mean luma >= 0.90 | a 1.5 px grid; the fast 1 px grid remains 100 % |
| Texture | luma stddev >= 0.95 and high-pass energy >= 0.90 of input | fine texture must not become a blur |
| Gradient | maximum change <= 4/255 | no banding introduced |
| Colour | saturation drift vs GT no worse than the input + 0.01 | luma-weighted blending must not grey out colour edges |
| Budget | <= 0.3 ms at 1080p and <= 0.6 ms at 1440p (RTX 5060) | the project budget |

A candidate that fails a gate is printed with the failing scene and metric. It can still be useful for diagnosis, but
it is not eligible for "best".

## 5. Quality ranking

For slanted, roofline, colour and subpixel scenes the suite records:

- `dPSNR = PSNR(out, GT) - PSNR(input, GT)` in dB (RGB, peak 255).
- `dSSIM = SSIM(out, GT) - SSIM(input, GT)` on luma.

A positive value means the filter moved the image toward the area reference. Negative dPSNR is possible: a filter can
look smoother than the input while over-blurring a shallow edge. The report shows both, per scene and averaged.

The automatic ranking score is:

```
score = 100 * mean(1 - stepRatio) + 10 * mean(dPSNR) + 1000 * mean(dSSIM)
```

It is a search heuristic, not a quality score. `dSSIM` has a small absolute range and is weighted highest; the edge
term rewards visible staircase removal; `dPSNR` breaks ties. The report prints every component, so a candidate with a
good score but a bad per-scene row is visible immediately. The owner's visual review has the final word.

## 6. The sweep

`--aa-sweep` evaluates a grid of `AaSettings`:

- quality: Performance / Balanced / Quality (search length 8 / 12 / 16)
- strength: 0.60 / 0.70 / 0.85 / 1.0
- relative edge threshold: 0.08 / 0.10 / 0.12
- absolute floor: 0.03 (configurable)
- text/HUD protection: on for eligible candidates; off only as a diagnostic

The default grid is 36 candidates. Each candidate runs the scene set at 1280x720/SS=4, then a cost pass at 1080p and
1440p on a busy 2-px checker/text scene. The run prints a ranked table and writes `candidates.csv` plus a per-scene
`scenes.csv`.

The current default (`Balanced`, 1.0, 0.10, 0.03, protections on) is included in every sweep. A no-AA baseline
(strength 0) is measured for reference but fails the edge gate; it shows how much of the score comes from doing
nothing.

## 7. Visual review pack

Every suite run writes `aa_suite/index.html` and its images:

- candidate ranking table with gate status and per-metric columns;
- for the top eligible candidate and the current default, each synthetic scene as input / ground truth / output,
  full-frame and a 4x nearest crop of the region that matters (edge centre, text glyphs, checker, roofline);
- for each `--aa-image` real capture, input / output / amplified difference and the same 4x crop;
- a short checklist for the owner.

The pack is deliberately not embedded in the console output. The owner opens the HTML, compares the crops, and says
whether the effect is visible and whether text/HUD/fine detail survive. A benchmark result without that review is
"not rejected by the gates", not "approved".

## 8. Running it

```
# fast regression (unchanged)
moonsr_bench --aa

# thorough suite at the current default
moonsr_bench --aa-suite --aa-image <capture>.png --out-dir build\aa_suite

# sweep and review pack
moonsr_bench --aa-sweep --aa-image <capture>.png --out-dir build\aa_suite
moonsr_bench --aa-sweep --aa-qualities balanced,quality --aa-strengths 0.85,1.0 --aa-thresholds 0.08,0.10
```

Useful options: `--aa-size WxH` (suite resolution, default 1280x720), `--aa-supersample N` (default 4),
`--aa-qualities`, `--aa-strengths`, `--aa-thresholds`, `--aa-floors`, `--aa-no-protect` (diagnostic only).
The cost pass always measures 1080p and 1440p; it does not change with `--aa-size`.

## 9. Limitations

- **Synthetic GT is area coverage, not a game reference.** A candidate can win PSNR while looking worse in a game
  with TAA, film grain or alpha-tested foliage. The real captures and the owner's eye are the counterweight.
- **Gamma space only.** The pipeline is gamma-encoded; a linear-light AA would need a separate reference set.
- **No temporal GT yet.** T1 is stateless and deterministic, so a static scene repeated is bit-identical. A temporal
  tier would need a moving ground-truth sequence and a ghosting/flicker metric; the suite does not claim to cover
  that.
- **The sweep can overfit the scene set.** The scenes are deliberately varied, but they are still synthetic. Do not
  adopt a setting that only wins on one scene; the per-scene table exists to catch that.
- **Cost is data-dependent.** The busy checker is a worst-case edge density; a real game may cost less. The budget
  uses the measured worst case.
- **27 candidates is not a global optimum.** It is a reproducible search step. Widen the grid only when a candidate
  family looks promising; keep the default and the baseline in every run.
- **One GPU is the budget target.** The suite can run on the A400/iGPU for correctness, but their timings are not the
  budget.

## 10. Adding a candidate algorithm

The suite only knows `AaPass::Create`, `SetSettings`, `Apply`, `Reset`, `Stats`. To compare a new filter:

1. Implement it behind `AaPass` (or add a mode inside it), keeping the same input/output contract.
2. Extend the candidate grid with its mode/quality values.
3. Run the suite; the gates and the visual pack are the same.
4. Do not change the scene set and the thresholds in the same commit as the algorithm. A benchmark that moves with
   the candidate is not a benchmark.

## 11. Owner review checklist

- Does the 4x crop show the staircase gone on the slanted edges/roofline?
- Do the text and HUD crops look unchanged at 1:1 and 4x?
- Is the 4-px checker still crisp, not smeared?
- On the real captures, is the effect visible in motion? Is it worth the cost?
- Does the game's own AA look better, worse, or different? That is allowed; record it.

The answer is a go/no-go on the candidate, not a PSNR number.
