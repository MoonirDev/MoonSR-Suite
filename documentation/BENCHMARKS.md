# MoonSR Suite — Benchmarks

Rules: every number here was produced by a command that was run on the hardware listed, at the commit listed.
No estimates, no third-party figures presented as ours. "Not measured" is a valid and expected entry.

## 1.1.3 notice persistence

UI/settings change only; no new performance measurements. Existing rendering measurements below remain unchanged.

## 1.1.3 capture compatibility check

RTX 5060, driver 32.0.16.2114; source 6426888 plus the capture fix. Default capture inclusion, SDR output, V-Sync on. The GPU smoke test verified the expected presented colour through a desktop BitBlt screenshot and Windows Graphics Capture. No new performance measurements.

## Test machine

| Item | Value |
|---|---|
| CPU | AMD Ryzen 9 7900X (12 cores) |
| RAM | 31.1 GB |
| GPU 0 | NVIDIA GeForce RTX 5060, driver 32.0.16.2114, drives a 1920x1080 @ 143 Hz display |
| GPU 1 | NVIDIA RTX A400, driver 32.0.16.2114, drives a 1920x1200 @ 60 Hz display |
| GPU 2 | AMD Radeon Graphics (Ryzen iGPU, device 0x164E), driver 31.0.24002.92, no display attached |
| OS | Windows 11 Pro, build 26300 |
| Toolchain | Visual Studio 2026 (MSVC 14.52), Windows SDK 10.0.26100 |

Not available on this machine: any Intel GPU, any GPU older than the listed ones, a VRR measurement rig, a latency
photodiode. Compatibility with those is therefore unverified.

The notice dismissal patch changed the app UI. No new performance measurements were taken for that patch.

## Smart parallax verification on 1.1.1 (2026-11-24)

Build `1e3220bf65fd5eb311e448d14a5318702c134596`, source fingerprint `beb377f9e7b45f63`. The RTX 5060 used
driver 32.0.16.2114. Commands were `moonsr_bench --relief --relief-strengths 0.5,1 --adapter 5060` and the same
command with `--relief-parallax`. Both passed the synthetic checks, including zero change on the opaque HUD and
auto-pause on the sky and cel-shaded scenes. The timing used 1920×1080 input and output, default detail, and frame
generation off. These are sums of the GPU timestamp medians for Analyse and Apply.

| Tier and timing strength | Loaded total (ms) | Idle-gap total (ms) |
|---|---:|---:|
| Shading, 50% | 0.2419 | 0.2413 |
| Parallax, 100%, 3 px cap | 0.2812 | 0.3344 |

Shading's idle-gap result varied between runs and was above target on the previous playtest build below.
`--relief-image` also kept the saved Helldivers 2 and ATLYSS crops active in the still, pan, and noisy-still
sequences. These inputs were already cropped before the benchmark applied its own inset.

`moonsr_bench --relief-motion-depth --adapter 5060 --adapter A400 --adapter Radeon` passed the motion mask,
rotation-only, stationary generated-phase, and scene-cut checks on all three adapters. At 640×352, default detail,
100% strength, 2 px cap, and known 8 px cell flow, the RTX 5060 took 0.0832 ms for Analyse plus 0.0226 ms for
Apply. The A400 (driver 32.0.16.2114) took 0.2742 ms plus 0.1679 ms. This excludes the cost of producing the
flow. The Radeon iGPU (driver 31.0.24002.92) was used for correctness. Live 2× and 4× playtests are pending.

### Shading reference comparisons

The expanded benchmark used the 1.1.1 relief code, RTX 5060 and driver 32.0.16.2114. It exports the final 960×540
frame from the procedural brick and rock pans, including the flat input, Shading result, two CPU baselines, and a reference lit
from the known height-map normals. This reference checks lighting fidelity. The displaced-wall reference below
checks geometric displacement. Run `moonsr_bench --relief --relief-strengths 0.5,1 --adapter 5060 --relief-no-time`
with `--save-images`, then compare each image with its `_reference.png` in FLIP 1.7 (LDR defaults). The values are
mean FLIP error at the default 2 px detail.

| Scene and strength | Flat | Shading | Emboss | Clarity |
|---|---:|---:|---:|---:|
| Bricks, 50% | 0.118795 | 0.096228 | 0.111371 | 0.141673 |
| Bricks, 100% | 0.118795 | 0.105855 | 0.112619 | 0.143345 |
| Rock, 50% | 0.087354 | 0.056506 | 0.051421 | 0.108013 |
| Rock, 100% | 0.087354 | 0.059276 | 0.052422 | 0.109196 |

Shading improved on the flat input in both scenes. Emboss had lower error on rock at both strengths. Testing
1.5 px detail at 50% raised the rock Shading error to 0.061956, so this run gives no reason to lower the default.

Two isolated gain trials used the later 1.1.1-based relief candidate (`1e3220b`, benchmark fingerprint
`3e173597fc0e373b`) and the same RTX 5060, driver, 960×540 scenes, and FLIP settings. The Shading suite passed
at both strengths for each trial. Lower gain helped at full strength in the second trial, while its 50% rock
result worsened and emboss still scored lower at both strengths. The playtest keeps the current gains.

| Trial (directional / cavity gain) | Bricks FLIP, 50% / 100% | Rock FLIP, 50% / 100% | Rock emboss FLIP, 50% / 100% |
|---|---:|---:|---:|
| 0.60 / 0.08 | 0.096796 / 0.106812 | 0.056759 / 0.059464 | 0.051419 / 0.052416 |
| 0.38 / 0.08 | 0.096819 / 0.099053 | 0.062940 / 0.055776 | 0.050275 / 0.052017 |

Two further isolated trials used the same benchmark, default gains and estimated height. One projected the
normalized normal onto the light's horizontal direction. The other used a clamped Lambert dot product and
subtracted the flat-surface light contribution. Both passed the Shading checks, and neither beat emboss on rock.

| Lighting trial | Bricks FLIP, 50% / 100% | Rock FLIP, 50% / 100% | Rock emboss FLIP, 50% / 100% |
|---|---:|---:|---:|
| Normalized directional | 0.098188 / 0.104733 | 0.057897 / 0.057985 | 0.050872 / 0.052337 |
| Lambert | 0.098160 / 0.103586 | 0.059422 / 0.058673 | 0.050888 / 0.052118 |

### Measured height on CC0 materials

The expanded `--relief-image --save-images --relief-strengths 0.5 --adapter 5060` fixture ran on 1.1.1-based
relief code, RTX 5060 and driver 32.0.16.2114. The 1K maps came from [Poly Haven's CC0 library](https://polyhaven.com/license).
Plain input used the diffuse map. The AO input multiplied diffuse in linear light by `0.6 + 0.4 × AO`, with
the red channel of the ARM map used where a separate AO map was unavailable. The estimate came from the Height
debug export, matched to the displacement map at the final pan offset. Both measurements exclude the outer
eight analysed pixels. The reference height subtracts a Gaussian blur with sigma 3 px, as in the synthetic test.

To reproduce this offline, save the 1K map files as `<slug>_Diffuse.png`, `<slug>_Displacement.png`, and
`<slug>_AO.png` under `build/relief-materials/maps`. Use each linked asset page's final URL segment for `<slug>`.
If a material has only the ARM map, save that map as `<slug>_AO.png`. Build the benchmark from the current source
to get the Height export. With Python, NumPy and Pillow available, run the following from the repository root.

```powershell
$root = 'build/relief-materials'
python tools/bench/relief_material_report.py "$root/maps" "$root/inputs"
$t = @('--adapter','5060','--relief-strengths','0.5','--save-images','--out-dir',"$root/results")
Get-ChildItem "$root/inputs" -Filter '*.png' | ForEach-Object { $t += @('--relief-image', $_.FullName) }
& build/main/bin/moonsr_bench.exe @t
python tools/bench/relief_material_report.py "$root/maps" "$root/inputs" "$root/results/relief"
```

| Material | Spearman, diffuse / with AO | Sign agreement, diffuse / with AO |
|---|---:|---:|
| [Brick Wall 005](https://polyhaven.com/a/brick_wall_005) | −0.2960 / −0.2522 | 26.02% / 30.50% |
| [Cobblestone 01](https://polyhaven.com/a/cobblestone_01) | 0.0423 / 0.0453 | 56.65% / 56.51% |
| [Rock Ground](https://polyhaven.com/a/rock_ground) | 0.3807 / 0.4634 | 80.41% / 86.12% |
| [Stone Brick Wall 001](https://polyhaven.com/a/stone_brick_wall_001) | 0.0363 / 0.0793 | No qualifying pixels |
| [Wood Planks](https://polyhaven.com/a/wood_planks) | −0.0029 / −0.0052 | 72.31% / 66.76% |

Sign agreement covers pixels with an absolute estimated height above 0.02 and an absolute band-passed reference
height above 0.05.

None reached the M1 target of 0.5. Brick Wall 005 showed the strongest reversed polarity. The synthetic materials
with known lighting remain regression fixtures. The measured materials show where colour-based height estimation
can diverge from the displacement map.

An offline diagnostic checked whether the low scores came from a shifted reference or the choice of colour
channel. It sampled every fourth pixel, excluded another eight pixels at each border and tested horizontal
and vertical shifts from −12 to +12 pixels in four-pixel steps. The strongest absolute brick and rock
correlations occurred at zero shift (−0.300 and +0.383). For the other three materials, even the strongest
shift had an absolute correlation below 0.12.

Using the same grid and the same Gaussian band-pass, the largest absolute diffuse RGB channel correlations
were 0.246, 0.080, 0.434, 0.055 and 0.126 in table order. Directly filtering the separate AO inputs gave
+0.381, −0.035, +0.771, +0.570 and +0.480. The AO scores describe source maps that MoonSR cannot read from
a game capture. These checks narrow the failure to the information available in the captured colour for this
fixture and the tested estimators. The three-of-five target remains unmet.

An offline multiscale trial filtered the diffuse maps' log luminance. It subtracted Gaussian blurs at
1.5, 3, 6 and 12 pixels from a 1 px Gaussian blur. Two additional variants took the mean or median of
the four bands after separately normalizing each band's variance. Scores use the same one-in-four
interior sampling and Gaussian-filtered displacement reference as the alignment diagnostic above.

| Material | 1.5 px | 3 px | 6 px | 12 px | Normalized mean | Normalized median |
|---|---:|---:|---:|---:|---:|---:|
| Brick Wall 005 | −0.133 | −0.231 | −0.318 | −0.311 | −0.277 | −0.280 |
| Cobblestone 01 | 0.057 | 0.092 | 0.110 | 0.103 | 0.100 | 0.102 |
| Rock Ground | 0.407 | 0.429 | 0.414 | 0.392 | 0.431 | 0.427 |
| Stone Brick Wall 001 | 0.049 | 0.064 | 0.047 | 0.027 | 0.050 | 0.053 |
| Wood Planks | 0.087 | 0.148 | 0.183 | 0.137 | 0.169 | 0.167 |

None of the six variants reached 0.5 on any material. The multiscale methods stayed out of the shader.

The rebuilt halo playtest (`1e3220b` plus pending changes, RTX 5060 driver 32.0.16.2114) was
retested with the ten saved inputs at 50% Shading and default detail. All stayed active during the
noisy-still sequence. Spearman diffuse/AO pairs, in original table order, were −0.2837/−0.2435,
0.0363/0.0400, 0.2680/0.3153, 0.0351/0.0777 and −0.0199/−0.0300.
Rock lost the most correlation. The combined halo suppression helps the silhouette fixture but
does not meet the three-material accuracy gate.

A matched comparison of the height and halo playtests split the Rock Ground crop by whether the Height
PNG changed by at least four 8-bit codes. On diffuse input, 48.6% of pixels met that threshold. Their
Spearman correlation fell from 0.4594 to 0.3055. The rest stayed near 0.273. With AO, 54.4% met the
threshold and fell from 0.5400 to 0.3554. The rest stayed near 0.322. This split is defined by the observed
height change and does not identify which pixels correspond to geometry edges.

An isolated comparison on the same GPU and inputs replaced the stored-height ramp's 0.08–0.95 range with
0.30–0.65. Raw silhouette leakage increased from 0.218 to 0.278 at both strengths. Rendered Shading halo
ratios were 0.03/0.05 at 50%/100% and the Shading suite passed. Diffuse/AO rank correlations for rock fell
from 0.2680/0.3153 to 0.2511/0.2896. The narrower ramp stayed out of the playtest.

A later isolated trial kept the 0.08–0.95 ramp and reduced its attenuation as local band-pass variation
rose from 0.035 to 0.095 stops. On the same RTX 5060 and inputs, rock correlation rose to 0.2973/0.3537
on diffuse/AO. The raw silhouette ratio was 0.225 and rendered Shading halo remained 0.04/0.05 at 50%/100%.
The Shading suite passed. The trial still missed the material accuracy target. Its timing and generated-frame
behaviour have yet to be verified.

### Generated-phase timing fixture

The expanded harness uploads a bilinearly resized copy of the textured input outside the GPU timers. The same
RTX 5060 and 1.1.1 relief code processed a settled brick scene at synthetic 2× and 4× display phases with no
motion field. These loaded GPU timestamp medians measure the relief passes. They exclude frame-generation cost
and motion-guided sampling. Shading used 50% strength and Parallax used 100% strength with a 3 px cap.

| Input → output | Phases | Shading Analyse / Apply (ms) | Parallax Analyse / Apply (ms) |
|---|---:|---:|---:|
| 720p → 1080p | 2× | 0.0811 / 0.0550 | 0.0805 / 0.0955 |
| 720p → 1080p | 4× | 0.0819 / 0.0472 | 0.0862 / 0.0389 |
| 1080p → 1440p | 2× | 0.1727 / 0.1013 | 0.1738 / 0.0748 |
| 1080p → 1440p | 4× | 0.1728 / 0.0840 | 0.1748 / 0.0750 |

As in the main pipeline, Analyse runs once per real input and Apply runs per displayed phase. The Apply column is
the median across the mixture of real and generated phases. In the same expanded run, 1080p → 1080p Shading took
0.2423 ms loaded and 0.2953 ms with idle gaps. Parallax took 0.2801 ms loaded and 0.2822 ms with idle gaps.
Both modes stayed active on settled bricks with zero variation across the checked frames. Both paused on the
pixel-art fixture at 50% and 100% strength. The 1.1.1 silhouette fixture's raw height-band energy ratio was 0.866.

An isolated RTX 5060 replay (driver 32.0.16.2114, `1e3220b` with pending relief changes, default detail)
multiplied the Height debug sample by the existing application protection weight. The resulting silhouette
band-to-interior energy ratio was 0.091 at both 50% and 100% Shading. The rendered shading halo ratios
were 0.05 and 0.08, respectively, and the self-checks passed. The masked height is a diagnostic
intermediate. The original pre-protection height still exceeds the M1 limit of 0.25.

Two stored-height experiments followed on the same RTX 5060 and driver. Applying the combined edge and texture
gate to the stored height changed opaque HUD pixels and failed the painted-wall check at 50%. A separate
half-resolution edge texture let the next experiment attenuate the current height estimate and reduce temporal
carry-over near strong edges without applying the texture gate twice. The 0.05–0.85 edge ramp produced a raw
silhouette energy ratio of 0.273. The 0.08–0.95 ramp brought it to 0.218 at both strengths and preserved opaque
HUD pixels. The stronger ramp raised the rendered Shading halo ratios from 0.05 and 0.08 to 0.15 and 0.15.
The ramp meets the raw-height gate in this fixture but remains an isolated variant because it increased the
rendered halo compared with the playtest shader.
Both Shading and Parallax passed their synthetic self-checks at 50% and 100%. At native 1080p, five
50% Shading timing blocks with default detail and frame generation off gave a loaded median of 0.2437 ms.
The high-resolution 2 ms idle-gap totals ranged from 0.2427 to 0.2968 ms.

A subsequent trial kept that raw height map and faded the final relief contribution with the existing surface
protection weight. The raw silhouette ratio remained 0.218. At 50% and 100%, the rendered halo ratios fell
from 0.15/0.15 to 0.08/0.09 for Shading and measured 0.07/0.09 for Parallax. The playtest baseline was
0.05/0.08 for both modes. Shading and Parallax self-checks passed at both strengths, including the opaque HUD
checks. This was an isolated benchmark build.
On the same RTX 5060, the 960×540 Shading exports at default detail were scored against the saved reference
renders with FLIP 1.7 LDR defaults. Brick means at 50%/100% were 0.096228/0.105855 for the playtest and
0.096227/0.105795 for the trial. Rock means were 0.056506/0.059276 and 0.057263/0.060012, respectively.
The rock emboss baseline scored 0.051421/0.052422. The trial was kept out of the playtest because rock fidelity
regressed and the rendered halo remained above the playtest baseline.

A gradient trial used protected neighbours to move each of the four height taps toward the centre height.
Raw height remained at 0.218. At 50%/100% Shading the rendered halo was 0.07/0.09, and all the Shading
self-checks passed. On the RTX 5060 with the same 1080p settings, the five loaded timing blocks had a median
of 0.2600 ms and high-resolution 2 ms idle-gap totals ranged from 0.2595 to 0.3177 ms.
One gather from the centre of the protection texture gave a raw height ratio of 0.218 and rendered halo ratios
of 0.08/0.09. All Shading checks passed, including opaque HUD preservation. Five loaded blocks gave a median
of 0.2471 ms and the high-resolution idle-gap range was 0.2466–0.3021 ms. This trial met the loaded budget
and the raw-height target in the fixture. It is still a benchmark variant. The rendered halo remained above
the playtest baseline of 0.05/0.08.

Combining that gather with a 0.10–0.55 smooth fade on the centre surface weight brought the rendered halo to
0.04/0.05 at 50%/100% for both Shading and Parallax. The raw ratio remained 0.218. Both suites passed,
including opaque HUD protection. On the RTX 5060 with driver 32.0.16.2114, the same isolated build kept
Parallax active on the owner's Helldivers 2 and ATLYSS screenshots throughout static, pan and noisy-still runs.
Noisy-still coverage was 0.554 and 0.636, respectively. The screenshot runs check coverage and pausing.
Matched Parallax exports from the height and halo playtests on the RTX 5060 used the same captures, input
crop and default detail. At 100% strength, mean absolute RGB change from the input fell from 1.881 to
1.732 code values in Helldivers 2 and from 0.891 to 0.842 in ATLYSS. The two outputs differed by at least
two codes in any RGB channel on 5.65% and 0.84% of pixels, respectively. These are still-frame measurements.
Using the same 960×540 reference renders and FLIP 1.7 LDR settings, the combined playtest scored
0.096189/0.105702 on brick and 0.057875/0.060475 on rock at 50%/100% Shading. The pre-halo build
scored 0.096228/0.105855 and 0.056506/0.059276. Rock fidelity thus got worse in this fixture, and
the rock emboss comparison still had lower mean error.
The standalone playtest includes the combined variant. Native 1080p timing used default detail, frame
generation off, 50% Shading, and 100% Parallax with a 3 px cap on the same RTX 5060. Across five loaded
blocks the medians were 0.2481 ms and 0.2893 ms, respectively. Five high-resolution 2 ms idle-gap blocks
ranged from 0.2469 to 0.2476 ms for Shading and from 0.2889 to 0.3460 ms for Parallax. Earlier Shading
variants showed a slower idle cluster, so this sample does not establish that it has disappeared.
A repeat on the same RTX 5060 used the byte-matched playtest shaders and the same 1080p Shading settings.
All 15 loaded blocks met the 0.25 ms target (0.2469–0.2492 ms, median 0.2478 ms). With high-resolution
2 ms idle gaps, eight of 15 blocks exceeded the target (0.2471–0.3031 ms, median 0.2518 ms).
Motion depth passed on all three adapters. The standalone build passed the upscaling, frame generation,
theme and strength-zero self-tests. In-game appearance remains unverified.
The combined build also passed live Windows Graphics Capture on the RTX 5060. Four-second runs of a
640×360 moving pattern at 20 fps used Bilinear upscaling, 100% Parallax, and the order Frame generation →
Upscaling → Smart parallax. Parallax stayed active and the runs presented 75 generated frames at fixed
2× and 237 at fixed 4×, with zero reported scheduler skips.
With Smart Parallax Off, a four-second 640×360 → 1920×1080 Bilinear run on the same adapter presented
79 generated frames at fixed 2× with zero reported skips or drops. The stage was absent from diagnostics.
The engine's Off path returns before creating the relief pass and releases its resources when switched off.

### Generated-frame stability playtest candidate

An offline diagnostic harness replayed the four owner recordings through the same frame generator and compared
each output with and without Parallax. Hardware was the RTX 5060 with driver 32.0.16.2114. Both runs used 1.1.1
(`1e3220b`) as the source base, 1920×1080 capture, 100% Parallax strength, 3 px cap, 3 px detail, and Performance
frame generation at half flow resolution with precise motion. This replay uses raw inputs and omits temporal
upscaling and presentation. Each check excludes the first eight captured input frames to let the history settle.

| Recording | Real frames with zero RGB change, baseline → candidate | Mean relief-strength gap at phase 0.75, baseline → candidate (RGB code values) |
|---|---:|---:|
| ATLYSS 2× | 33 / 59 → 8 / 59 | 0.1149 → 0.0730 |
| ATLYSS 4× | 17 / 36 → 10 / 36 | 0.0560 → 0.0329 |
| Helldivers 2 2× | 38 / 81 → 0 / 81 | 0.0989 → 0.0769 |
| Helldivers 2 4× | 56 / 67 → 4 / 67 | 0.0531 → 0.0335 |

The gap is the absolute difference between the generated and newest real frames' mean absolute RGB changes from
relief. It covers pairs where both builds applied relief to the real frame. All recordings were replayed at all
four phases for comparison. The remaining zero-change frames were not classified individually.

The follow-up candidate limits the colour shift when Parallax follows frame generation. It passed the 50% and
100% synthetic Shading and Parallax checks with the opaque HUD unchanged, including filtered Parallax on generated
phases. Filtered displacement and the Motion depth regressions passed on the RTX 5060, RTX A400 and Radeon iGPU.

On the 1.1.1-based follow-up build and RTX 5060 driver 32.0.16.2114, the revised GPU timing harness waits for
each queued batch before collecting another set of timestamps. The `--relief --relief-strengths 0.5,1 --adapter 5060`
run and its `--relief-parallax` counterpart measured these medians at 1920×1080 input and output with default
detail and no generated phases:

| Tier and strength | Loaded total (ms) | Idle-gap total (ms) |
|---|---:|---:|
| Shading, 50% | 0.2428 | 0.2423 |
| Parallax, 100%, 3 px cap | 0.2842 | 0.2836 |

With synthetic 2× and 4× phases and the new filter, Parallax Apply took 0.2240 ms and 0.2219 ms per displayed
phase respectively at 720p → 1080p. Analyse took 0.0836 ms and 0.0856 ms per real frame. Those measurements
exclude frame generation and motion-guided sampling. The owner's second live test found the shimmer less apparent,
with some shimmer still visible after frame generation.

Two later Helldivers 2 recordings were replayed with the filter off and on, otherwise using the same RTX 5060
replay settings above. Among pairs with more than 0.05 code values of mean relief change on the real frame,
the phase 0.75 gap as a share of real-frame relief fell from 8.96% to 7.53% for the 2× recording (77 pairs)
and from 12.86% to 10.93% for the 4× recording (40 pairs). Mean real-frame relief also fell from 0.810 to 0.758
RGB code values and from 0.590 to 0.552 respectively. The replay uses phase 0.75 for both recordings to compare
the behaviour just before the newest real frame. The original 2× output used phase 0.5.

### Grain-tolerant HUD learning

The 1.1.1-based candidate counts a pixel toward HUD learning only when changes in the same direction are also
present on both sides along at least one axis. On the RTX 5060 with driver 32.0.16.2114, six of the ten material
inputs above paused with the previous detector during the 90-frame `--relief-image` grain sequence. All ten
stayed active with the candidate. The `--relief --relief-strengths 0.5,1 --adapter 5060` suite and its Parallax
counterpart kept the opaque HUD pixel-exact on real and generated phases. Sky and stylised frames still paused.

At 1920×1080 input and output, 50% Shading took 0.2491 ms loaded and 0.2495 ms with idle gaps. At 100% and
a 3 px cap, Parallax took 0.2909 ms loaded and 0.2906 ms with idle gaps. Both used default detail and no frame
generation. The Motion depth correctness checks passed on the RTX 5060, RTX A400 and Radeon iGPU.

### Flow-aligned post-generation replay

The next candidate averages neighbouring height for post-generation displacement. In an offline replay of the
owner's two newest Helldivers 2 recordings on the RTX 5060 (driver 32.0.16.2114), generated-frame relief was
compared with real-frame relief sampled at the backward-flow position. Flow cells needed at least 0.65 confidence
and at most 0.22 residual. The 1920×1080 replay used 100% strength, a 3 px cap, 3 px detail, and Performance frame
generation with half-resolution flow and precise motion. The baseline is the previous post-generation filter,
using the same 1.1.1-based grain candidate in both runs.

| Recording | Pairs | Phase | Mean aligned RGB error, baseline → candidate | Error as a share of aligned real-frame relief |
|---|---:|---:|---:|---:|
| Helldivers 2 2× | 83 | 0.50 | 0.2411 → 0.2341 | 28.08% → 27.65% |
| Helldivers 2 4× | 70 | 0.75 | 0.1436 → 0.1383 | 15.44% → 15.10% |

Aligned real-frame relief fell from 0.8585 to 0.8469 and from 0.9300 to 0.9161 RGB code values in the two
recordings. A live playtest is still needed to evaluate the visible change.

An isolated replay compared the combined playtest's two-height blend with using only the newest warped height.
Both tests used the same settings above and measured against the phase-weighted relief contribution from both real
frames, sampled along forward and backward flow. The table shows mean RGB error divided by mean reference relief
on pixels with reliable flow in both directions. Both variants used identical reference frames and pixel masks.

| Recording | Pairs | Phase | Two heights | Newest height |
|---|---:|---:|---:|---:|
| Helldivers 2 2× | 82 | 0.50 | 29.21% | 30.24% |
| Helldivers 2 4× | 69 | 0.25 | 26.06% | 28.71% |
| Helldivers 2 4× | 69 | 0.50 | 22.09% | 22.68% |
| Helldivers 2 4× | 69 | 0.75 | 17.11% | 17.23% |

The two-height blend had lower error at each tested phase. This comparison isolates height selection and leaves
the existing colour filter and motion protection in place.

The original recording manifests contain 25 generated outputs at phase 0.50 out of 120 recorded outputs at
requested 2×. At requested 4×, 42 of 120 were generated, with 14 each at phases 0.25, 0.50 and 0.75.
All generated outputs have a matching newest input frame in the manifest. These counts describe the captured
output mix. The replay scores use fresh interpolation phases from the recorded input pairs.

Two further variants recomputed height from each generated image without height history. The full variant ran
the existing guided filter and learned protection for the generated images. The band-pass variant ran prefiltered
log luminance, the guided base and a height pass normalized with four local luminance samples. It kept the original
real-frame protection, motion confidence, fade and colour filtering. The real-frame relief statistics, scene-cut
decisions and aligned reference energy matched across all three runs.

| Recording | Pairs | Phase | Two real heights | Full recompute | Band-pass recompute |
|---|---:|---:|---:|---:|---:|
| Helldivers 2 2× | 82 | 0.50 | 29.21% | 50.77% | 73.80% |
| Helldivers 2 4× | 69 | 0.25 | 26.06% | 39.57% | 73.83% |
| Helldivers 2 4× | 69 | 0.50 | 22.09% | 38.03% | 71.33% |
| Helldivers 2 4× | 69 | 0.75 | 17.11% | 35.13% | 69.23% |

The RTX 5060 GPU timestamp medians at the end of the 2× and 4× replay runs were 0.2194 and 0.2194 ms
per additional analysis with the full variant, and 0.1610 and 0.1614 ms with the band-pass variant.
These timings cover the additional GPU analysis in the offline replay. Both variants increased total analysis work
and had higher aligned error at every tested phase. The two-height blend remains in the playtest build.

Three more trials used the same RTX 5060, driver, settings and isolated 1.1.1-based replay. One tapered the
displacement by up to 25% at mid-phase where the two warped heights disagreed. One interpolated the motion field
within a cell and retained nearest-cell motion where the two estimates differed by at least one pixel. The third
used the generator's finer motion selection for height sampling when confidence was high. All three retained
the real-frame relief output in the replay. The selected-motion trial retained the existing protection coordinates.

| Recording | Phase | Two real heights | Displacement taper | Interpolated cell flow | Generator selection |
|---|---:|---:|---:|---:|---:|
| Helldivers 2 2× | 0.50 | 29.21% | 29.20% | 29.27% | 32.00% |
| Helldivers 2 4× | 0.25 | 26.06% | 26.05% | 26.06% | 26.51% |
| Helldivers 2 4× | 0.50 | 22.09% | 22.09% | 22.12% | 23.14% |
| Helldivers 2 4× | 0.75 | 17.11% | 17.11% | 17.10% | 17.46% |

The displacement taper changed the aligned error by at most 0.012 percentage points in this replay. Cell-flow
interpolation was mixed. These measurements use the original coarse flow to align the reference.

A second replay ran the coarse and selected-motion candidates on the same generated images. It aligned the
reference using the generator's half-resolution selection motion and blend share. It measured pixels where both
the coarse flow and selected motion were confident and the four neighbouring selection vectors agreed within one
pixel of their interpolated value. This models the generator's smooth-region warp. It omits the colour evidence
used at motion boundaries and does not reproduce the generator's final anti-aliasing pass.

| Recording | Pairs | Phase | Coarse sampling | Generator selection |
|---|---:|---:|---:|---:|
| Helldivers 2 2× | 82 | 0.50 | 30.49% | 27.61% |
| Helldivers 2 4× | 69 | 0.25 | 25.74% | 25.14% |
| Helldivers 2 4× | 69 | 0.50 | 22.31% | 21.04% |
| Helldivers 2 4× | 69 | 0.75 | 16.99% | 16.22% |

Selected motion reduced the error on the alternative reference at each phase. The change needs game testing
to judge visible shimmer across motion boundaries and bright surfaces.

The integrated playtest also falls back progressively as neighbouring selection vectors disagree. In the same
RTX 5060 replay against the smooth-region reference, the final candidate measured 27.72% at 2×. At 4×, it
measured 25.16%, 21.08% and 16.25% at phases 0.25, 0.50 and 0.75. Its coarse-sampling baseline and pair
counts were the same as in the preceding table.

The standalone build used `1e3220b` with the pending relief changes. The selected-motion fixtures kept opaque
HUD pixels exact and passed scene-cut, real-frame identity and unreliable-selection checks on the RTX 5060,
RTX A400 and Radeon iGPU. The RTX 5060 also passed the upscaling, frame generation, theme, relief and
strength-zero tests. A three-second Windows Graphics Capture check with a 640×360, 20 fps moving pattern
and Bilinear 1920×1080 output presented 55 generated frames with Parallax after fixed 2× and 178 with Motion
depth after fixed 4×. Separate live cases ran with upscaling before frame generation, Motion depth before
frame generation and with it off, and Parallax on the RTX A400 with frame generation on the RTX 5060.

At 1080p input and output without frame generation, the standalone build measured 0.2475 ms under load and
0.2976 ms with idle gaps for 50% Shading. Parallax measured 0.2898 ms under load and 0.3399 ms with idle
gaps at 100% and a 3 px cap. Both used default detail and the same RTX 5060 and driver as the replay.

### Shading timing investigation

An isolated fixture at `1e3220b` with the pending selection changes ran blocks of 400 frames at 1920×1080 on
the RTX 5060 (driver 32.0.16.2114). It used 50% Shading with default detail and frame generation off.
The reported time is the sum of the Analyse and Apply GPU timestamp medians. The baseline and each sRGB
trial ran ten blocks under load and ten with a requested 2 ms idle gap every eight frames.

| sRGB decode | Loaded median (ms) | Idle-gap median (ms) | Idle-gap range (ms) |
|---|---:|---:|---:|
| Baseline for polynomial trial | 0.2478 | 0.2472 | 0.2466–0.2984 |
| Degree-eight polynomial | 0.2514 | 0.2503 | 0.2500–0.3015 |
| Baseline for lookup-table trial | 0.2481 | 0.2476 | 0.2468–0.2976 |
| 256-entry sRGB lookup table | 0.2564 | 0.2565 | 0.2553–0.3118 |

A separate instrumented build bracketed each analysis pass individually. The per-pass medians across five
loaded blocks were 0.0551 ms for luminance, 0.0152 and 0.0139 ms for the two box passes, 0.0481 ms for
gating and 0.0474 ms for height. The two slower idle-gap blocks had medians of 0.0681 ms, 0.0156 ms,
0.0156 ms, 0.0593 ms and 0.0577 ms in the same order. The extra timestamps perturb the total, so these
measurements locate the cost and the uninstrumented runs above measure the budget. The gate cache trial
raised the loaded median gate cost to 0.0491 ms. Loading the previous height from the exact texel in the
no-motion branch reduced the loaded height pass to 0.0461 ms, with no corresponding improvement in the
slower idle-gap total. These variants stayed in the benchmark build.

The same uninstrumented shader was run with Windows `Sleep(2)` and with a high-resolution waitable timer
requesting 2 ms. The median wall-clock pause for each block was 13.36–13.86 ms with `Sleep(2)` and
2.01–2.05 ms with the waitable timer. Across five blocks per mode, the GPU total ranged from 0.2468 to
0.2982 ms with `Sleep(2)` and from 0.2472 to 0.2976 ms with the waitable timer. The 0.25 ms budget is
met under load in these runs. Meeting it consistently after a gap remains unverified.

The next trial changed only height-pass scheduling. Each thread used the original height calculation for a
2×2 block, and read the frame's cut flag once for the block. At the same 1080p settings and on the same GPU,
two interleaved five-block runs produced these results:

| Height dispatch | Loaded median by run (ms) | Pooled range with high-resolution 2 ms gaps (ms) |
|---|---:|---:|
| Original 8×8 workgroup, one pixel per thread | 0.2481, 0.2483 | 0.2472–0.2978 |
| 8×8 workgroup, four pixels per thread | 0.2399, 0.2398 | 0.2386–0.2892 |

Two one-run alternatives were slower under load. Caching half-resolution inputs in group-shared memory
with four pixels per thread took 0.2440 ms. A 16×16 workgroup with one pixel per thread took 0.2491 ms.
The four-pixel trial improved the loaded timings and reduced the slowest observed idle-gap result.
All 312 benchmark PNGs matched the original by SHA-256. They cover the Shading and Parallax synthetic
fixtures at 50% and 100%, and the saved 1857×1017 Helldivers 2 and 1856×1016 ATLYSS inputs. The captures
also stayed active through both motion and the noisy-still sequence. Motion depth checks passed on all three
test adapters.

The combined playtest build uses commit `1e3220b` plus the pending relief changes (source fingerprint
`3e173597fc0e373b`). On the same RTX 5060, at 1920×1080 input and output, the Shading run measured 0.2492 ms
under load and 0.2486 ms with idle gaps. Parallax measured 0.2912 ms and 0.2910 ms under the same conditions
at 100% strength with a 3 px cap. The CLI strength-zero test copied every pixel exactly for both BGRA8
and RGBA8 output. The true Off mode still leaves the relief pass uncreated.

The combined build also ran both saved game crops through `--relief-image --relief-strengths 0.5,1 --adapter 5060`.
Helldivers 2 (1793×953 analysed) and ATLYSS (1792×952 analysed) both stayed active throughout the still, pan,
and noisy-still sequences. Their noisy-still textured coverage was 0.565 and 0.656 respectively.

The same playtest ran against `moonsr_testpattern --width 640 --height 360 --fps 20 --motion-pattern 1` through
Windows Graphics Capture on the RTX 5060. The CLI used Bilinear at 1920×1080 output with a three-second window
per case. It presented 59 generated frames with Parallax after fixed 2× generation, 177 after fixed 4×, and 177
with Parallax before fixed 4×. Parallax reported active in all three cases. With relief and generation off, the
reported pipeline contained only Upscaling. This checks routing and presentation on a synthetic window.

The expanded synthetic suite adds a 55%-opaque panel and dense smoke over bricks. On the same RTX 5060,
`--relief --relief-strengths 0.5,1 --relief-no-time --adapter 5060` and its `--relief-parallax` counterpart passed.
The 960×540 scenes used default detail and a 40-frame pan. The table reports the largest absolute 8-bit channel
change on the translucent panel and mean absolute relative luminance change over the dense smoke core. The
opaque HUD remained pixel-exact, including on the synthetic generated phases.

| Mode and strength | Translucent HUD max (RGB levels) | Dense smoke mean change |
|---|---:|---:|
| Shading, 50% | 9 | 0.142% |
| Shading, 100% | 11 | 0.286% |
| Parallax, 50% | 10 | 0.142% |
| Parallax, 100% | 14 | 0.286% |

An isolated test moved the edge protection ramp from 0.22–0.55 to 0.12–0.40 stops per pixel. Both suites passed,
and the translucent panel's four peak changes were identical to the table. The playtest retains the original ramp.

The wall-path suite now includes an orbit around the wall centre, with the camera continuously facing it. In the
same 1.1.1-based playtest on the RTX 5060 (driver 32.0.16.2114), the 480×270 orbit used default detail, 100%
Parallax, and no lighting or cavity modulation. With a 2 px cap, SSIM to the displaced-wall reference rose from
0.973525 to 0.976178, and mean extra warp error was 0.1111 RGB code values. With a 3 px cap, SSIM rose from
0.973525 to 0.975862, and mean extra warp error was 0.1550 code values. Both caps passed the 0.5-code-value check.

The rebuilt halo playtest (`1e3220b` plus pending changes) passed the Parallax suite on the RTX 5060 with
driver 32.0.16.2114. For the final 448×119 wall crop at the 3 px cap, FLIP 1.7 LDR mean error moved
from 0.053595 to 0.047176 on strafe and from 0.055473 to 0.048095 on dolly. The earlier height playtest
scored 0.047177 and 0.048097 with Parallax. Shading and cavity are disabled in this fixture to isolate
displacement. Mean extra warp error stayed below 0.5 RGB codes on strafe, dolly, yaw and orbit at both caps.

A second isolated replay rendered Shading and Parallax with their default lighting and cavity settings at
100% strength. The displaced wall reference used a directional light derived from the wall's known height
gradient, with a linear-light gain capped at 15%. On the same RTX 5060 and driver, FLIP 1.7 LDR mean error
at the 3 px cap fell from 0.070066 to 0.066214 on strafe and from 0.074439 to 0.069701 on dolly. SSIM
improved on both paths at both caps. All four paths passed the 0.5-code-value extra warp-error check.
This is a synthetic geometry and lighting comparison.

## Smart parallax playtest build (2026-11-23)

Working tree based on `8a975b9`, benchmark source fingerprint `ddcc6a97fd59b8e4`. Hardware and driver versions
are listed above. Times are GPU timestamp medians. Analyse runs per real frame and Apply per displayed frame.

The RTX 5060 1920×1080 input and output test used `moonsr_bench --relief --relief-strengths 0.5,1 --adapter 5060`
and the same command with `--relief-parallax`. Timing uses 50% strength for Shading, 100% and a 3 px cap for
Parallax, default detail, and no frame generation. The loaded case uses filler copies between relief frames.

| Tier | Loaded total (ms) | Idle-gap total (ms) |
|---|---:|---:|
| Shading | 0.2423 | 0.2949 |
| Parallax | 0.3061 | 0.3342 |

With relief off, the engine does not create or dispatch the relief stage. This was verified from the engine code
path. The test above does not measure application pacing with relief off.

`moonsr_bench --relief-motion-depth --adapter 5060 --adapter A400 --adapter Radeon` used a 640×352 input and output,
100% strength, default detail and 2 px cap. The fixture supplied 8 px cell flow from known camera and object motion.
These times cover relief only, after the flow exists, with a real frame displayed.

| Adapter | Parallax analyse + apply (ms) | Motion depth analyse + apply (ms) |
|---|---:|---:|
| RTX 5060 | 0.0302 + 0.0201 | 0.0836 + 0.0227 |
| RTX A400 | 0.2014 + 0.1534 | 0.2743 + 0.1683 |
| Radeon iGPU | Correctness only | Correctness only |

On the RTX 5060, mean RGB change inside the independently moving patch fell from 2.677 to 0.258 code values.
All three adapters passed the tests for rotation-only motion, the synthetic near plane and moving object,
stationary 2× and 4× generated phases, and the simulated scene cut. Live generated-frame quality is pending.

The ordered-stage integration was also checked with Windows Graphics Capture on the animated test pattern
at 640×360 input and 1920×1080 output. Motion depth reached its active tier with generation off, before and
after fixed 2× generation, and before MoonSR temporal upscaling. With the RTX 5060 as the main GPU and the
RTX A400 selected for relief, both the D3D12 cross-adapter path and forced CPU staging presented frames. This
check was for routing and correctness. Its GPU numbers are separate from the single-device timing table.

## Reference technologies — what can be measured here

| Reference | Status | Reason |
|---|---|---|
| Bilinear, Catmull-Rom, Lanczos-3 | measurable | implemented in-tree |
| NVIDIA Image Scaling | possible later | MIT shader source; needs a licence note and integration as a reference scaler |
| FSR 1 | possible later | pending file-level licence check |
| DLSS 4.5 SR, FSR 4, XeSS | not measurable on captures | need engine inputs; would require recording outputs from a game that supports them, with matched ground truth |
| Lossless Scaling LS1 / LSFG | not measured | proprietary; requires a purchased copy and a capture-of-output methodology |
| DLSS FG / MFG, XeSS FG, FSR FG | not measurable on captures | engine-integrated |
| "ANR FG" | not measured | no public reference found |

## Methodology (`tools/bench`, executable `moonsr_bench`)

Build: `powershell -ExecutionPolicy Bypass -File tools\build.ps1 -BuildDir build\bench -Target moonsr_bench`
(RelWithDebInfo, /W4 clean). `moonsr_bench --help` lists every option. The tool links the same `moonsr_upscale`
static library as the application, so it runs the shipped shader bytecode through the real `ScalePass::Render`; nothing
is re-implemented for the GPU part.

**Upscaling quality.**
1. Deterministic synthetic ground truth, 1920x1080, 8-bit sRGB-encoded PNGs written to `benchmarks/output/gt/`
   (git-ignored; regenerate with `moonsr_bench --gen`; the FNV-1a hash of every image is recorded in the result files and
   printed by `--gen`, and two consecutive `--gen` runs produced identical hashes). Seven images: `zone_plate` (local
   frequency rises from 0 at the centre to exactly Nyquist at the corners), `slanted_edges` (six tiles with edges at
   2, 5, 15, 30, 45 and 75 degrees, 8x8 supersampled), `thin_lines` (1-2 px lines horizontal, vertical, 45 deg, 20 deg,
   concentric rings and sparse coloured 1 px lines, 4x4 supersampled), `checker` (1, 2, 4 px pixel-aligned and 3 px rotated
   15 deg), `text` (GDI, greyscale anti-aliased Segoe UI 9-36 px and Consolas 10-20 px, dark-on-light and light-on-dark,
   some coloured; the only image that depends on the installed fonts), `gradient_noise` (smooth ramps, a low-contrast
   diagonal ramp, plus +-2 level triangular noise), `natural` (value-noise background, 70 soft discs of varying edge
   softness, 14 hard-edged rotated rectangles, 2-4 px grain).
2. The input is made by an exact area-averaging (box) downsample of the ground truth, in float, rounded to 8 bit, by the
   factor 2 or 1.5 (input 960x540 / 1280x720).
3. The input is uploaded as B8G8R8A8_UNORM, rendered with `ScalePass::Render` using `FitMode::Stretch` into a
   B8G8R8A8_UNORM target of ground-truth size (the tool verifies the returned dest rect is the whole target) and read back
   through a staging texture. Every scaler that `ScalePass::ListScalers()` reports as available is run, with sharpness 0
   and 0.5.
4. Metrics are computed on the CPU against the ground truth: PSNR on RGB (MSE over the three channels, peak 255) and on
   BT.709 luma of the encoded values (float, not rounded); SSIM on that luma with the standard 11x11 Gaussian window
   (sigma 1.5, normalised, K1 0.01, K2 0.03, L 255), valid region only (no border padding), mean over windows.

**Gamma space (methodology limitation).** Downsampling, upscaling and every metric operate directly on the
gamma-encoded 8-bit values, with no linearisation. This matches what the shaders do (see `shaders/README.md`, "Gamma
space") and what a game-capture pipeline delivers, but a linear-light downsample of the ground truth would give a
different input (different averages at edges) and different scores. All absolute numbers below are specific to this
gamma-space definition and must not be compared with published figures computed in linear light or on other content.

**Validation of the metric code (`moonsr_bench --selftest`, non-zero exit code on any failure).** Checks:
identical images give PSNR +inf (RGB and luma) and SSIM exactly 1; a constant +10 level offset gives
20*log10(255/10) = 28.1308 dB; an alternating +-7 error gives 20*log10(255/7) = 31.2288 dB; SSIM of two constant images
equals the closed form (2*mx*my + C1)/(mx^2 + my^2 + C1); the separable Gaussian SSIM agrees with an independent
straightforward 2-D 11x11 implementation to 3e-16 on three textured pairs; the window sums to 1 with the hand-computed
centre tap 0.266012; the area downsample equals hand-computed 2x2 block means and 1.5x fractional-coverage averages; the
CPU bilinear reference reproduces hand values and is the identity at 1x; PNG write/read is lossless; on the GPU,
nearest-neighbour 2x of an asymmetric random colour image is bit-exact pixel replication (proves upload, channel order,
orientation and readback) and bilinear 4x is within 1 level of the CPU reference. Not done: no comparison against an
external reference implementation (no scikit-image / numpy on this machine and nothing was downloaded), so agreement with,
for example, `skimage.metrics.structural_similarity` is argued from the formula and the independent in-tree
implementation, not demonstrated. LPIPS and FLIP are not implemented.

**Alignment check (run automatically with every bilinear sharpness-0 quality run).** The GPU bilinear output is compared
with a CPU bilinear upscale of the same input using correct centre alignment (source position (x+0.5)*src/dst - 0.5,
clamped). To show the check can see an offset, the GPU output is also compared with CPU references deliberately shifted by
+-0.5 source texel.

**GPU timing (`--time`).** Isolated pass time on an otherwise idle GPU, *not* in-game cost (no capture, no present, no
competition with a running game, no cross-adapter copy). The source is a generated `natural` image at the input size.
After a warm-up of 50 renders and a flush/wait, 200 iterations are submitted in batches of 50; each batch sits inside one
`D3D11_QUERY_TIMESTAMP_DISJOINT`, and every iteration is bracketed by its own pair of timestamp queries (so idle gaps
between iterations are not counted). The context is flushed and every query is polled to completion before use; batches
reporting `Disjoint` are discarded (none occurred in the runs below). A "pass" is exactly one `ScalePass::Render` call:
render-target clear, the scaler draw and, if sharpness > 0 and the scaler is not Nearest, the second (sharpen) draw. As a
validity cross-check the tool also records the CPU wall-clock time per iteration over each batch (submit to last timestamp
readable). Timer frequency reported by the disjoint query: 1 GHz (NVIDIA), 100 MHz (AMD).

Outputs: console tables plus `benchmarks/output/results/<mode>_<tag>.csv` and `.json` (adapter, driver UMD version,
vendor/device id, OS build, git commit or "uncommitted", `source_hash`, date, sizes, scaler, sharpness, metrics, the
command line). `source_hash` is an FNV-1a hash of the `shaders/`, `src/core`, `src/gpu` and `src/upscale` sources on disk
at run time; it identifies the code state while the repository has no commit, and matches the executable only if the
executable was rebuilt after the last edit of those files.

Frame generation (later): take a high-frame-rate sequence, drop frames, interpolate, compare with the dropped
frames; plus pacing statistics from present timestamps.

## Results

Common to all results below: machine as in "Test machine"; OS build 10.0.26300.9457; binary
`build\bench\bin\moonsr_bench.exe` (RelWithDebInfo); application commit: **none** (the repository has no commit, so
`git rev-parse --short HEAD` fails and the tool records "uncommitted"); source hash `8bfacb10fa5ba4bf` (21 files); run on
2026-11-13. Scalers present in that build: Nearest, Bilinear, Bicubic (Catmull-Rom), Lanczos-3, MoonSR Spatial (the shader
`shaders/upscale/moonsr_spatial_ps.hlsl` is work in progress, so its rows describe that exact state only and will change).
Driver versions are the packed UMD versions reported by DXGI: RTX 5060 and RTX A400 32.0.16.2114, Radeon iGPU
31.0.24002.92. Raw data: `benchmarks/output/results/` (git-ignored; reproduce with the commands). Self-test:
`moonsr_bench --selftest` -> 18 checks, 0 failures (GPU checks ran on the RTX 5060).

### Upscaling quality

Commands:

```
moonsr_bench --quality --scales 2 --sharpness 0,0.5 --all-adapters --verbose --tag 2x_all_adapters
moonsr_bench --quality --scales 1.5 --sharpness 0,0.5 --adapter "RTX 5060" --verbose --tag 1_5x_rtx5060
moonsr_bench --quality --scales 2 --sharpness 0,0.5 --adapter "RTX 5060" --save-images --tag 2x_rtx5060_images
```

Scores below come from the RTX 5060. The RTX A400 produced bit-identical output to the RTX 5060 for every
image/scaler/sharpness at 2x (see the cross-vendor check), so its scores are the same; the Radeon iGPU differs by at most
2 levels per pixel and its scores differ in the third decimal at most.

#### 2x (960x540 -> 1920x1080), RTX 5060

Mean over the 7 images (PSNR is the arithmetic mean of per-image dB). The stress images (zone plate, thin lines, checker) pull the mean down and are dominated by content above the input's Nyquist limit, so read the per-image tables for anything beyond a coarse ranking.

| Scaler | Sharpness | PSNR RGB (dB) | PSNR luma (dB) | SSIM |
|---|---|---|---|---|
| Nearest | 0 | 26.93 | 27.29 | 0.8313 |
| Bilinear | 0 | 25.41 | 25.72 | 0.7699 |
| Bicubic (Catmull-Rom) | 0 | 26.46 | 26.77 | 0.8185 |
| Lanczos-3 | 0 | 26.64 | 26.95 | 0.8277 |
| MoonSR Spatial | 0 | 27.38 | 27.70 | 0.8343 |
| Nearest | 0.5 | 26.93 | 27.29 | 0.8313 |
| Bilinear | 0.5 | 25.83 | 26.13 | 0.7798 |
| Bicubic (Catmull-Rom) | 0.5 | 27.03 | 27.33 | 0.8264 |
| Lanczos-3 | 0.5 | 27.22 | 27.53 | 0.8348 |
| MoonSR Spatial | 0.5 | 28.05 | 28.38 | 0.8384 |

PSNR luma (dB) per image, sharpness 0:

| Scaler | zone plate | slanted edges | thin lines | checker | text | gradient noise | natural |
|---|---|---|---|---|---|---|---|
| Nearest | 13.25 | 37.90 | 16.26 | 11.81 | 24.01 | 52.25 | 35.54 |
| Bilinear | 12.77 | 36.50 | 15.30 | 9.63 | 23.93 | 45.11 | 36.78 |
| Bicubic (Catmull-Rom) | 13.43 | 38.09 | 16.06 | 10.78 | 24.73 | 46.47 | 37.82 |
| Lanczos-3 | 13.54 | 38.37 | 16.04 | 10.95 | 25.00 | 46.19 | 38.55 |
| MoonSR Spatial | 13.50 | 40.94 | 16.92 | 10.77 | 25.29 | 48.58 | 37.86 |

PSNR luma (dB) per image, sharpness 0.5:

| Scaler | zone plate | slanted edges | thin lines | checker | text | gradient noise | natural |
|---|---|---|---|---|---|---|---|
| Nearest | 13.25 | 37.90 | 16.26 | 11.81 | 24.01 | 52.25 | 35.54 |
| Bilinear | 12.93 | 37.55 | 15.43 | 9.79 | 24.23 | 46.16 | 36.86 |
| Bicubic (Catmull-Rom) | 13.55 | 39.66 | 16.17 | 10.93 | 24.97 | 48.14 | 37.88 |
| Lanczos-3 | 13.62 | 40.14 | 16.16 | 11.10 | 25.29 | 47.72 | 38.65 |
| MoonSR Spatial | 13.43 | 43.39 | 16.97 | 10.89 | 25.33 | 50.90 | 37.73 |

SSIM (luma) per image, sharpness 0:

| Scaler | zone plate | slanted edges | thin lines | checker | text | gradient noise | natural |
|---|---|---|---|---|---|---|---|
| Nearest | 0.5852 | 0.9968 | 0.7513 | 0.6698 | 0.9544 | 0.9939 | 0.8675 |
| Bilinear | 0.4542 | 0.9911 | 0.6625 | 0.4481 | 0.9487 | 0.9926 | 0.8925 |
| Bicubic (Catmull-Rom) | 0.5411 | 0.9939 | 0.7318 | 0.5968 | 0.9585 | 0.9930 | 0.9142 |
| Lanczos-3 | 0.5539 | 0.9940 | 0.7423 | 0.6220 | 0.9615 | 0.9928 | 0.9277 |
| MoonSR Spatial | 0.5668 | 0.9970 | 0.8029 | 0.6019 | 0.9661 | 0.9934 | 0.9122 |

SSIM (luma) per image, sharpness 0.5:

| Scaler | zone plate | slanted edges | thin lines | checker | text | gradient noise | natural |
|---|---|---|---|---|---|---|---|
| Nearest | 0.5852 | 0.9968 | 0.7513 | 0.6698 | 0.9544 | 0.9939 | 0.8675 |
| Bilinear | 0.4773 | 0.9933 | 0.6762 | 0.4727 | 0.9531 | 0.9929 | 0.8929 |
| Bicubic (Catmull-Rom) | 0.5619 | 0.9962 | 0.7432 | 0.6139 | 0.9620 | 0.9933 | 0.9144 |
| Lanczos-3 | 0.5693 | 0.9963 | 0.7535 | 0.6381 | 0.9651 | 0.9931 | 0.9283 |
| MoonSR Spatial | 0.5730 | 0.9988 | 0.8099 | 0.6151 | 0.9675 | 0.9937 | 0.9107 |

#### 1.5x (1280x720 -> 1920x1080), RTX 5060

Mean over the 7 images (PSNR is the arithmetic mean of per-image dB). The stress images (zone plate, thin lines, checker) pull the mean down and are dominated by content above the input's Nyquist limit, so read the per-image tables for anything beyond a coarse ranking.

| Scaler | Sharpness | PSNR RGB (dB) | PSNR luma (dB) | SSIM |
|---|---|---|---|---|
| Nearest | 0 | 26.78 | 27.17 | 0.8247 |
| Bilinear | 0 | 27.22 | 27.56 | 0.8267 |
| Bicubic (Catmull-Rom) | 0 | 28.49 | 28.85 | 0.8634 |
| Lanczos-3 | 0 | 28.87 | 29.25 | 0.8756 |
| MoonSR Spatial | 0 | 29.71 | 30.10 | 0.8769 |
| Nearest | 0.5 | 26.78 | 27.17 | 0.8247 |
| Bilinear | 0.5 | 28.05 | 28.38 | 0.8406 |
| Bicubic (Catmull-Rom) | 0.5 | 29.38 | 29.74 | 0.8752 |
| Lanczos-3 | 0.5 | 29.83 | 30.20 | 0.8844 |
| MoonSR Spatial | 0.5 | 29.97 | 30.35 | 0.8823 |

PSNR luma (dB) per image, sharpness 0:

| Scaler | zone plate | slanted edges | thin lines | checker | text | gradient noise | natural |
|---|---|---|---|---|---|---|---|
| Nearest | 13.69 | 36.46 | 15.69 | 9.22 | 24.82 | 52.72 | 37.61 |
| Bilinear | 14.47 | 38.09 | 16.72 | 9.61 | 25.80 | 47.89 | 40.32 |
| Bicubic (Catmull-Rom) | 15.57 | 39.55 | 17.64 | 10.24 | 26.88 | 49.66 | 42.44 |
| Lanczos-3 | 16.02 | 40.02 | 17.89 | 10.57 | 27.36 | 49.27 | 43.60 |
| MoonSR Spatial | 16.26 | 42.45 | 18.71 | 10.14 | 27.80 | 52.92 | 42.43 |

PSNR luma (dB) per image, sharpness 0.5:

| Scaler | zone plate | slanted edges | thin lines | checker | text | gradient noise | natural |
|---|---|---|---|---|---|---|---|
| Nearest | 13.69 | 36.46 | 15.69 | 9.22 | 24.82 | 52.72 | 37.61 |
| Bilinear | 14.77 | 39.66 | 16.95 | 9.90 | 26.36 | 50.40 | 40.63 |
| Bicubic (Catmull-Rom) | 15.86 | 41.32 | 17.82 | 10.60 | 27.43 | 52.27 | 42.85 |
| Lanczos-3 | 16.27 | 42.29 | 18.00 | 10.82 | 27.97 | 51.87 | 44.20 |
| MoonSR Spatial | 16.29 | 43.92 | 18.70 | 10.33 | 28.11 | 52.91 | 42.23 |

SSIM (luma) per image, sharpness 0:

| Scaler | zone plate | slanted edges | thin lines | checker | text | gradient noise | natural |
|---|---|---|---|---|---|---|---|
| Nearest | 0.6483 | 0.9930 | 0.7658 | 0.4760 | 0.9663 | 0.9943 | 0.9289 |
| Bilinear | 0.6304 | 0.9936 | 0.7761 | 0.4681 | 0.9685 | 0.9941 | 0.9560 |
| Bicubic (Catmull-Rom) | 0.7158 | 0.9954 | 0.8293 | 0.5589 | 0.9762 | 0.9944 | 0.9738 |
| Lanczos-3 | 0.7384 | 0.9958 | 0.8438 | 0.5974 | 0.9788 | 0.9943 | 0.9804 |
| MoonSR Spatial | 0.7620 | 0.9979 | 0.8772 | 0.5520 | 0.9819 | 0.9947 | 0.9724 |

SSIM (luma) per image, sharpness 0.5:

| Scaler | zone plate | slanted edges | thin lines | checker | text | gradient noise | natural |
|---|---|---|---|---|---|---|---|
| Nearest | 0.6483 | 0.9930 | 0.7658 | 0.4760 | 0.9663 | 0.9943 | 0.9289 |
| Bilinear | 0.6576 | 0.9958 | 0.7930 | 0.5124 | 0.9734 | 0.9945 | 0.9577 |
| Bicubic (Catmull-Rom) | 0.7379 | 0.9971 | 0.8409 | 0.6008 | 0.9799 | 0.9947 | 0.9753 |
| Lanczos-3 | 0.7567 | 0.9976 | 0.8524 | 0.6248 | 0.9824 | 0.9946 | 0.9823 |
| MoonSR Spatial | 0.7709 | 0.9986 | 0.8797 | 0.5766 | 0.9837 | 0.9947 | 0.9719 |

Observations (measured, specific to this content and gamma-space definition): at sharpness 0, MoonSR Spatial has the highest
SSIM on `slanted_edges` and `text` at both scales but not on `natural`, where Lanczos-3 is ahead (2x: SSIM 0.9277 vs 0.9122,
PSNR luma 38.55 vs 37.86 dB; 1.5x: 0.9804 vs 0.9724, 43.60 vs 42.43 dB). On the stress images (`zone_plate`, `thin_lines`,
`checker`) PSNR is 9-19 dB for every scaler because the content exceeds the input's Nyquist limit; only the ranking is
informative there. Nearest beats Bilinear in the 2x mean because bilinear adds blur on top of an already box-filtered
input. Sharpness 0.5 raised the mean PSNR and SSIM of every filtering scaler at both scales, but not on every image (for
example MoonSR Spatial on `natural`: 2x 37.86 -> 37.73 dB, 1.5x 42.43 -> 42.23 dB). `Nearest` ignores sharpness
(`ScalePass` skips the sharpen stage for it), which is why its rows are identical.

#### Cross-vendor correctness check (2x, all 7 images, sharpness 0 and 0.5, reference = RTX 5060)

Command: the first command above (`--all-adapters`; every output of adapters 2 and 3 is diffed per pixel against the
RTX 5060 output of the same scaler/sharpness/image).

| Adapter vs RTX 5060 | Scaler | Max abs pixel difference (8-bit levels, any channel), sharpness 0 / 0.5 | Worst-image share of differing pixels, sharpness 0 / 0.5 |
|---|---|---|---|
| RTX A400 | all five | 0 / 0 | 0% / 0% (bit-identical) |
| Radeon iGPU | Nearest | 0 / 0 | 0% / 0% (bit-identical) |
| Radeon iGPU | Bilinear | 1 / 2 | 5.59% / 5.61% |
| Radeon iGPU | Bicubic (Catmull-Rom) | 1 / 2 | 4.65% / 5.46% |
| Radeon iGPU | Lanczos-3 | 1 / 2 | 7.87% / 8.88% |
| Radeon iGPU | MoonSR Spatial | 1 / 2 | 12.21% / 12.63% |

The largest mean absolute difference on any image is 0.14 level (Lanczos-3, sharpness 0.5, iGPU). The same shader bytecode
therefore produces the same picture on all three adapters up to last-bit differences on the Radeon (texture filtering /
floating-point rounding); no vendor-specific error (swap, shift, wrong region) is visible. Per-image values are in
`quality_2x_all_adapters.csv` (records of kind `cross_adapter`).

#### Alignment / orientation check

Same runs as above (records of kind `alignment`). GPU bilinear vs the CPU centre-aligned bilinear of the same input:

| Run | PSNR GPU vs CPU reference (min over 7 images) | Max abs difference | PSNR GPU vs references shifted by +-0.5 texel (range over images) |
|---|---|---|---|
| 2x, RTX 5060 | 57.8 dB | 1 level | 15.0 to 38.9 dB |
| 2x, RTX A400 | 57.8 dB | 1 level | 15.0 to 38.9 dB |
| 2x, Radeon iGPU | 57.4 dB | 1 level | 15.0 to 38.9 dB |
| 1.5x, RTX 5060 | 55.6 dB | 1 level | 15.6 to 40.2 dB |

Result: the GPU matches the correctly aligned CPU reference to within rounding (max 1 level, mean <= 0.18 level), while a
half-texel shifted reference scores 19 to 51 dB lower on the same image, so a half-pixel shift, flip or channel swap would
be detected. The GPU bilinear PSNR against the ground truth equals the CPU reference's within 0.06 dB on every image.
Visual check of the saved 2x outputs (`benchmarks/output/images/2x/NVIDIA_GeForce_RTX_5060/`):
`natural_MoonSR_Spatial_s0.png` and `text_Lanczos_3_s0.5.png` are upright, correctly coloured (red/blue/yellow/green
text; purple/teal discs), and not offset relative to the ground truth and the 960x540 input.

### Upscaling GPU cost

Commands:

```
moonsr_bench --time --all-adapters --sharpness 0,0.5 --tag all_adapters
moonsr_bench --time --adapter "RTX 5060" --sharpness 0 --tag rtx5060_repeat
```

200 timed iterations per cell after 50 warm-up renders (defaults), default size pairs. **Isolated ScalePass pass time on an
otherwise idle GPU, not in-game cost.** Cells are mean ms per pass with the minimum in parentheses. No disjoint batches
were discarded. This was not a dedicated machine (other desktop applications were running), the power/clock state of each
GPU was not controlled, and the Radeon iGPU shares system memory.

#### 1280x720 -> 1920x1080, mean ms per pass (min ms in parentheses)

| Scaler | Sharpness | RTX 5060 | RTX A400 | Radeon iGPU |
|---|---|---|---|---|
| Nearest | 0 | 0.017 (0.016) | 0.135 (0.129) | 0.407 (0.398) |
| Bilinear | 0 | 0.017 (0.016) | 0.139 (0.129) | 0.415 (0.407) |
| Bicubic (Catmull-Rom) | 0 | 0.061 (0.061) | 0.510 (0.498) | 1.441 (1.432) |
| Lanczos-3 | 0 | 0.120 (0.119) | 0.871 (0.854) | 3.711 (3.707) |
| MoonSR Spatial | 0 | 0.234 (0.232) | 1.692 (1.685) | 7.653 (7.645) |
| Nearest | 0.5 | 0.017 (0.016) | 0.141 (0.134) | 0.406 (0.399) |
| Bilinear | 0.5 | 0.047 (0.047) | 0.361 (0.349) | 1.287 (1.275) |
| Bicubic (Catmull-Rom) | 0.5 | 0.094 (0.094) | 0.731 (0.718) | 2.280 (2.266) |
| Lanczos-3 | 0.5 | 0.152 (0.152) | 1.093 (1.072) | 4.542 (4.535) |
| MoonSR Spatial | 0.5 | 0.269 (0.268) | 1.925 (1.910) | 8.483 (8.473) |

#### 1920x1080 -> 3840x2160, mean ms per pass (min ms in parentheses)

| Scaler | Sharpness | RTX 5060 | RTX A400 | Radeon iGPU |
|---|---|---|---|---|
| Nearest | 0 | 0.065 (0.064) | 0.471 (0.464) | 1.477 (1.461) |
| Bilinear | 0 | 0.070 (0.069) | 0.486 (0.471) | 1.585 (1.565) |
| Bicubic (Catmull-Rom) | 0 | 0.253 (0.249) | 2.011 (2.002) | 5.697 (5.678) |
| Lanczos-3 | 0 | 0.533 (0.505) | 3.451 (3.417) | 14.826 (14.817) |
| MoonSR Spatial | 0 | 0.938 (0.937) | 6.797 (6.779) | 30.343 (30.327) |
| Nearest | 0.5 | 0.066 (0.065) | 0.480 (0.464) | 1.478 (1.462) |
| Bilinear | 0.5 | 0.234 (0.233) | 1.331 (1.302) | 4.983 (4.963) |
| Bicubic (Catmull-Rom) | 0.5 | 0.412 (0.395) | 2.891 (2.881) | 9.022 (8.930) |
| Lanczos-3 | 0.5 | 0.701 (0.696) | 4.318 (4.302) | 18.130 (18.114) |
| MoonSR Spatial | 0.5 | 1.083 (1.080) | 7.685 (7.666) | 33.650 (33.626) |

Validity cross-checks: the CPU wall-clock time per iteration differed from the timestamp mean by at most 0.16 ms in all 60
rows (at most 8.5% in the rows whose mean exceeds 1 ms; the wall figure includes per-batch query readback). A second RTX
5060 run (sharpness 0 only) reproduced all ten means to within 0.002 ms. Cost scales with output pixels (1080p -> 4K is 4x
the pixels; Nearest 0.017 -> 0.065 ms on the RTX 5060, 0.135 -> 0.471 on the RTX A400, 0.407 -> 1.477 on the iGPU).
Sub-0.05 ms values on the RTX 5060 are close to a fixed per-pass cost (clear + draw); treat them as resolution-limited, not
as shader ALU cost. Sharpness 0.5 adds a second full pass (sharpen stage plus an intermediate render target); it is not
applied to Nearest.

### Known limitations of the harness

- Gamma-space processing only (see Methodology); no linear-light variant.
- Ground truth is synthetic and small (7 images, one resolution); `text` depends on the installed Segoe UI / Consolas.
  PSNR and SSIM alone do not capture ringing, aliasing or temporal behaviour; LPIPS, FLIP and temporal metrics are not
  implemented.
- SSIM was validated analytically and against an independent in-tree implementation only, not against an external library.
- Timing is isolated and single-process; it does not model a GPU shared with a game, clock/power state, or presentation.
  Differences between GPUs reflect clocks and memory systems as well as shader cost.
- The reference scalers listed under "Reference technologies" (NIS, FSR 1, Lossless Scaling, DLSS / FSR 4 / XeSS) are not run: not measured.
- Results are not yet tied to a commit (the repository has none); `source_hash` is the only code identifier.

### Pipeline (capture → scale → present)
Not measured yet.

### Frame-generation throughput investigation (2026-11-16)

The supplied LSFG 3.x chart reports maximum **final output** FPS at fixed 2x, 100% flow and SDR, using a
secondary GPU. It lists the RTX 5060 at 708 / 330 / 224 FPS for 1080p / 1440p / 4K. These are third-party
measurements, not quality scores or MoonSR results. The chart's source procedure and hardware configuration have
not been reproduced, so these numbers must not be presented as a direct head-to-head comparison.

`moonsr_fgbench --time` measures a real PushFrame and one Interpolate as one GPU-timestamped pair, and now reports
`2000 / pair_median_ms` as a theoretical 2x output rate. This excludes capture, cross-adapter transfer,
presentation, latency, games, and sustained competing GPU load; it is not displayed FPS or the LSFG test.
On this machine with 100% flow and the Performance tier, 40 measured iterations after 10 warm-ups produced:

| Adapter | 1080p pair median / p95 | 1440p pair median / p95 | 4K pair median / p95 |
|---|---|---|---|
| RTX 5060 | 3.00 / 3.01 ms | 5.28 / 5.29 ms | 11.94 / 12.01 ms |
| RTX A400 | 19.94 / 20.09 ms | 35.18 / 35.32 ms | 79.50 / 79.80 ms |

The census-free search shader was specialized for Performance: at 1080p the measured push/interpolate medians
changed from 3.133 / 0.314 ms to approximately 2.682 / 0.313 ms on the RTX 5060, and from 20.219 / 2.305 ms
to 17.632 / 2.304 ms on the A400. On the 720p synthetic quality suite, the greatest PSNR decrease against the
previous build was 0.002 dB. These comparisons are not a substitute for real-game visual evaluation.

`--triplet A.png,B.png,C.png` compares interpolated A/C with the held-out B image for each selected config.
The three images must be equally spaced and have identical dimensions; the tool reports scene cuts but does not
exclude unsuitable triplets or account for irregular capture times. It is an offline measurement, not an adaptive
learner or per-game profile.

### Frame generation, iteration 1 (2026-11-13)

Tool: `moonsr_fgbench` (`tools/fgbench`). Synthetic sequences at 1280x720 with CPU-rendered ground truth (3x3
supersampled). Commands: `moonsr_fgbench --selftest --adapter 5060` (29/29 pass, re-run by the lead),
`--quality --adapter 5060 --save-images --tag final_5060`, `--time --all-adapters --iterations 200 --warmup 30`.
Run by the frame-generation engineer on the uncommitted tree; raw CSV/JSON in `benchmarks/output/results/fg_*`.
These are synthetic results and say nothing about real games. Nothing was compared with any other product.

Quality, PSNR in dB, mean over t in {1/4, 1/3, 1/2, 2/3, 3/4}, RTX 5060, Balanced tier, flowScale 0.5:

| Sequence | MoonSR FG | Linear blend | Repeat nearest |
|---|---|---|---|
| pan_subpx (0.4, 0.25 px) | 40.94 | 40.82 | **43.15** |
| pan_5px | 35.82 | 23.44 | 22.18 |
| pan_20px | 34.86 | 18.89 | 17.81 |
| pan_45px | 33.23 | 18.41 | 17.27 |
| hud_pan | 32.69 | 19.06 | 17.98 |
| layers_occlusion | 33.77 | 20.72 | 19.65 |
| small_fast_object | 33.57 | 25.32 | 24.04 |
| zoom_rotate | 29.65 | 18.81 | 17.72 |
| brightness_pan | 32.08 | 21.93 | 19.57 |
| periodic_fence | 29.76 | 14.64 | 12.63 |

Loses on sub-pixel motion at t <= 1/3. The small fast object itself is not tracked (whole-frame PSNR hides it).
HUD pixels fully covered by a static overlay are reproduced exactly at flowScale 0.5.

Isolated GPU cost, median ms, flowScale 0.5 (PushFrame once per real frame / Interpolate once per generated frame):

| Adapter | Size | Performance | Balanced | Quality |
|---|---|---|---|---|
| RTX 5060 | 720p | 0.52 / 0.08 | 0.76 / 0.13 | 1.01 / 0.19 |
| RTX 5060 | 1080p | 1.07 / 0.17 | 1.56 / 0.28 | 2.15 / 0.41 |
| RTX A400 | 720p | 2.98 / 0.62 | 3.98 / 1.03 | 5.43 / 1.58 |
| RTX A400 | 1080p | 6.71 / 1.41 | 8.83 / 2.31 | 11.99 / 3.55 |
| Radeon iGPU | 720p | 13.82 / 2.61 | 19.63 / 3.62 | 27.17 / 5.35 |
| Radeon iGPU | 1080p | 31.11 / 5.95 | 43.66 / 8.16 | 60.63 / 12.06 |

Live pipeline (lead, `moonsr_cli --fg N`, 30 fps test pattern 1280x720 -> 1920x1080, RTX 5060, 144 Hz monitor,
external frame limiter active on this machine):

| Requested | Effective | Displayed fps | Generated fps | Hold-back | Reported latency | Late generated skipped |
|---|---|---|---|---|---|---|
| off | 1 | 29.9 | 0 | 0 | 0.3-0.7 ms | - |
| 2X | 2 | 59.9 | 29.9 | 16.6 ms | 17.2 ms (one run: 27 ms) | 0 |
| 3X | 2 (limited) | 59.8-61.3 | 29.9-31.6 | 16.1-16.4 ms | 17.9-18.4 ms | 1 |
| 4X | 2 (limited) | 61.9-62.0 | 30.0-32.0 | 16.1-16.2 ms | 18.7-21.8 ms | 3 |

Before the automatic limiting existed, 3X on this machine ran at 51-55 ms latency with real frames dropped.

After the scheduler rework (review fixes; same tool, 7-10 s runs, V-Sync off unless noted):

| Source | Output | Requested | Effective | Displayed fps | Generated fps | Hold-back | Reported latency | Late skipped |
|---|---|---|---|---|---|---|---|---|
| 30 fps | 144 Hz, RTX 5060 | 2X | 2 | 57-61 | 28-30 | 16.0-16.7 ms | 16.8-18.4 ms | 0 |
| 30 fps | 144 Hz, RTX 5060 | 4X | 2 after 2 back-offs | 59.5-59.8 | 29.7-29.9 | 16.3-17.0 ms | 17.4-18.0 ms | 38 (during back-off) |
| 24 fps | 144 Hz, RTX 5060 | 3X | 3 | 71.4-72.0 | 47.6-48.0 | 27.1-28.1 ms | 38.2-38.6 ms | 1 |
| 30 fps | 60 Hz, RTX A400 | 2X | 2 | 56-60 | 26-30 | 16.1-17.5 ms | 17.4-17.8 ms | 0 |
| 30 fps | 60 Hz, RTX A400, V-Sync | 2X | 2 | 52 | 21-22 | 16.8 ms | 18.5-19.1 ms | 36 |
| 60 fps | 60 Hz, RTX A400 | 2X | 1 (refresh cap) | 59.8-61.6 | 0 | 0 | 1.1-2.8 ms | 0 |

Displayed fps counts presents submitted by MoonSR, not frames confirmed on screen.

### MoonSR Temporal, iteration 1 (2026-11-15) - experimental, NOT in the application

*Historical: `moonsr_tbench` and the temporal upscaler were removed; these results cannot be reproduced from this
repository.*

Tool `moonsr_tbench` (`tools/tbench`), synthetic sequences rendered at 1920x1080 with exact ground truth, RTX 5060,
working tree at 1409101. Commands and raw CSVs: `benchmarks/output/results/temporal_quality_r*.csv`,
`temporal_time_r2_time_*.csv`. Run by the upscaling engineer; self-test (38 checks) passes on all three adapters.
Synthetic results are not evidence about real games. Nothing was compared with any other product.

"Aliased" input = point-sampled, no anti-aliasing (the case of the owner's test game). PSNR in dB, no sharpening:

| Sequence, aliased 1.5x | MoonSR Temporal | Lanczos-3 | MoonSR Spatial | Temporal vs best single-frame |
|---|---|---|---|---|
| static | 27.97 | **29.31** | 27.97 | -1.34 |
| pan_slow | 29.15 | **29.70** | 28.13 | -0.55 |
| pan_medium | **29.71** | 29.64 | 28.05 | +0.06 |
| pan_fast | **30.40** | 29.69 | 28.09 | +0.71 |
| hud_pan | 28.50 | **28.86** | 27.53 | -0.36 (HUD ghost error 18.1 vs 6.5) |
| layers | **29.67** | 29.56 | 27.99 | +0.10 |
| zoom_rotate | 28.35 | **29.85** | 28.56 | -1.51 |
| scene_cut | 30.51 | **30.62** | 29.35 | -0.11 |
| thin_features | 16.93 | 17.65 | 16.87 | -0.75 |

At 2x aliased it leads on every moving sequence (+0.3 to +2.0 dB, flicker roughly halved); at 3x and on
prefiltered input results are mixed. Known defects: dark halos around static HUD glyphs, undershoot on thin dark
lines, no recovery of 1-px features, no gain on static content (no jitter = no new information).

Finding for the current scalers: at 1.5x on aliased input **Lanczos-3 beats MoonSR Spatial v0.1 by about 1.3-1.6 dB**
on every sequence.

Isolated GPU cost of `Render`, mean ms (PushFrame for the motion estimate is extra):

| Adapter | 720p -> 1080p | 1080p -> 1440p | 1080p -> 2160p |
|---|---|---|---|
| RTX 5060 | 4.24 | 8.44 | 9.83 |
| RTX A400 | 21.9 | 48.3 | 57.9 |
| Radeon iGPU | 64.3 | 135.4 | 175.0 |

VRAM: 86 MB (720p -> 1080p) to 281 MB (1080p -> 2160p).

## Regressions
None recorded.

## Adaptive frame generation: ATLYSS, 2026-11-17

ATLYSS was capped at 60 FPS on the RTX A400; MoonSR used the RTX 5060 and the 144 Hz output at 1920x1080.
For a 55-second diagnostic run, the sample interval was shortened from the normal two seconds to 500 ms. The learner
evaluated 91 moving-game triplets and selected 75% of the requested 100% flow after its validation gate passed.
It continued to collect evidence, saved 92 observations to an isolated test profile, and reapplied the setting on a
subsequent run. This verifies actual game-data collection and persistence, not a guarantee for other games or scenes.

With frame generation at 2x, the run reported about 60 captured and 120 submitted FPS, no dropped capture frames,
and no late generated presents after startup. These are submission counts, not a physical display measurement.
Windows GPU Engine counters averaged approximately 19.4% of the RTX 5060 3D engine at 100% flow and 13.0% at 75%
over separate eight-second intervals. Lossless Scaling 3.2.2 measured approximately 9.5% over ten seconds in a
separate run; the user reported stable 60/120, fewer artifacts and smoother motion, with more smearing at low FPS.
Counter readings are workload-dependent and cannot establish visual or pacing parity.

With V-Sync on, the old one-refresh submission lead produced ~15.9 ms p95 intervals between Present calls in the
ATLYSS test. Scheduling at the intended time instead produced ~8.8–9.0 ms p95 with the same ~120 submitted FPS.
At 30-to-60 FPS on the 60 Hz output, the corresponding p95 was ~20 ms instead of ~36 ms. DXGI frame statistics
were sampled where available; because many present counts were not individually observed, they do not prove
physical display cadence. A user-visible comparison of the final pacing change remains necessary.

An experimental shortcut skipping the finest motion-regularization pass lowered isolated GPU time, but caused up
to 0.65 dB PSNR loss on a synthetic fast pan and 0.35 dB on an ATLYSS-derived occlusion scene; it was not enabled.

### Scheduling hypothesis, 2026-11-18

The user reported that the revised pacing feels smoother, but still less smooth than Lossless Scaling. The monitor
does not support variable refresh. The user clarified that their comparison used a 72 FPS game cap with 2x
generation on the 144 Hz display, with both applications reporting stable 72 / 144 and V-Sync enabled. Thus the
120 FPS / 144 Hz mismatch of the earlier diagnostic run does not explain their comparison. The counters report
average rates and do not establish the actual cadence.

A separate 24-pair ATLYSS recording (`build/main/alfg-real-frames/index.csv`) suggests another scheduling issue
worth testing. Recorded generated outputs occurred a median 2.67 ms after the captured frame, while their real
partners were still scheduled relative to capture. Output-record timestamps had a 5.68 ms p05 and 11.27 ms p95
interval. An offline arithmetic exercise that shifted each recorded real output by its generated partner's delay
gave 7.52 ms p05 and 9.13 ms p95, with a median added delay of 2.67 ms. This is not a presentation test: recording
adds overhead, timestamps are before Present, the clip is short, and the calculation assumes the shift has no
effect on later capture, GPU work, V-Sync or queueing. The separate live V-Sync run already had a much tighter
8.10 ms median and 8.94 ms p95 at Present return. Neither result justifies enabling a scheduler change without a
controlled live comparison, including missed captures, latency and physical display timing where measurable.

In a subsequent 25-second 72-to-144 V-Sync CLI run on ATLYSS with 75% flow, MoonSR reported 1,823 captured and
3,642 presented frames, zero discarded captures, and no late generated frames after startup. Rolling DXGI samples
were typically 6.94 ms median and 6.95 ms p95; some Present-return windows had p95 around 9 ms. These are internal
measurements, and the user reported that the test did not visibly start, so they do not establish what was actually
visible or how motion felt. The test omitted the frame counter, which makes a visually identical output harder to
distinguish from the game itself. A future visible test should enable the counter and confirm it appears first.

The user then confirmed a counter-enabled 20-second repeat was visible and felt smoother than their normal MoonSR
run. This repeat captured 1,450 frames and submitted 2,897, with no discarded captures and one generated frame
flagged late. The rolling DXGI interval sample near the end was 6.94 ms median and p95. This still does not prove
Lossless Scaling parity or rule out missing display observations; the comparison to the user's normal run was
subjective and the CLI used an isolated learning profile at 75% flow.

A visible 20-second 30-to-60 V-Sync test on the same 144 Hz monitor captured 606 and submitted 1,203 frames,
with zero discarded captures or late generated frames. Present-return intervals at the end had a 16.48 ms median
and 17.73 ms p95. The user saw approximately 30 / 60 on the counter and reported smooth motion. DXGI samples
included gaps (13.90 ms median, 27.77 ms p95) and do not establish uninterrupted physical presentation. The 60 FPS
output does not divide evenly into 144 Hz, which also limits what this particular display test can show.
In the user's follow-up comparison at the same 30 FPS game cap, Lossless Scaling felt smoother overall, while
MoonSR felt slightly less smooth but more responsive. This subjective result does not isolate interpolation,
display cadence, frame buffering, or input latency; it suggests measuring those tradeoffs before changing them.

In 20-second 30-to-60 tests, an experimental 3 ms pair hold-back felt smoother to the user but too delayed; the
reported capture-to-present average at the end increased from 16.71 to 19.66 ms. A 1 ms hold-back felt smoother
with acceptable responsiveness and reported 17.73 ms; end-of-run Present-return p95 was 17.32 ms versus 17.73 ms
for the original. In a subsequent 72-to-144 test the 1 ms setting also felt smoother and responsive; 1,451 frames
were captured, with one discarded capture and no generated frames flagged late. Results are short, unblinded,
scene-dependent comparisons and do not establish parity with Lossless Scaling. MoonSR now applies up to 1 ms to
both slots when generating with V-Sync (bounded to 20% of a slot at very high rates); non-V-Sync is unchanged.
`MOONSR_FG_SCHEDULE_DELAY_MS=0` disables the hold-back for diagnostic comparisons.

On 2026-11-18, refinement was changed to reuse the winning candidate's already-computed SSD instead of evaluating
the same score again. On the RTX 5060, alternating 1080p performance-tier timings at 100% flow measured median
PushFrame 2.645–2.648 ms before and 2.603–2.608 ms after (100 iterations per run). At 75% flow results varied
enough that a speedup was not established. The complete quality CSVs for synthetic and ATLYSS-image-derived 720p
sequences at 75% and 100% flow were byte-identical before and after. These isolated GPU timings do not establish
whole-application GPU utilization or Lossless Scaling parity.

Per-pass diagnostic profiling at 1080p and 75% flow measured roughly 0.75 ms search, 0.40 ms regularization,
0.16 ms refinement and 0.19 ms HUD masking per pair; these instrumented numbers include profiler overhead and
omit some passes. An experiment deduplicating equal neighboring vectors within regularization increased median
PushFrame time from about 1.65 to 1.68 ms at 75% flow and showed no useful change at 100%; it was removed.
An experiment detecting all-unit HUD weights inside search was also slower and was removed. Parallelizing the
source-weight sum across eight lanes instead reduced median PushFrame from 1.640–1.642 to 1.619–1.621 ms at 75%
flow and from 2.599–2.600 to 2.542–2.548 ms at 100% in alternating 1080p runs. Its 720p quality CSV matched the
baseline byte-for-byte for the tested synthetic and game-image-derived sequences, and a captured ATLYSS triplet
produced the same reported PSNR/SSIM at 75% and 100%. Floating-point addition order differs, so these cases do not
prove bit-identical results for every possible HUD mask or scene.
The same eight-lane source-weight reduction was then applied to regularization. Alternating 1080p runs at 100%
flow measured median PushFrame 2.540–2.547 ms before and 2.452–2.453 ms after; at 75% flow, the second baseline
was 1.613 ms and the two experimental runs were 1.568 ms (the first baseline was 1.686 ms). The tested 720p quality
CSV remained byte-identical, the same captured ATLYSS triplet had unchanged reported PSNR/SSIM, and the
frame-generator self-tests passed. Results remain scene- and hardware-dependent, and reordered sums may differ
slightly for other masks.

The 720p repeating-fence sequence exposes a sharp flow-scale sensitivity: at the midpoint, Performance flow 100%
scored 30.37 dB versus 13.90 dB at 75%; 70% scored 31.25 dB. Enabling census only at the coarsest search level did
not fix the 75% case and was removed. This synthetic sequence does not predict every game's outcome, but shows why
an accepted lower scale must be reconsidered when the scene changes. The learner now rejects it if at least three
of the latest four samples have MSE above 1.5 times the baseline plus 1.0; self-tests cover the guard and recovery.
Additional 720p fence tests changed the coarse pyramid cutoff, enabled census at every level, and adjusted the
near-best ambiguity margin. None resolved the 75% case, and several reduced quality at other scales; all three
experiments were removed. A more reliable solution needs evidence beyond these parameter changes.
The benchmark now accepts `--history N` (0–16 real frames before the tested pair). At 75% flow the fence midpoint
scored 13.90, 13.25, 13.67 and 13.91 dB with 1, 4, 8 and 16 preceding frames, respectively. At 100% flow it scored
30.37, 29.76, 29.77 and 29.77 dB. Thus this synthetic failure persists with temporal history rather than being
limited to the first estimated pair. The default one-frame-history output matched the previous benchmark exactly.
`--history 0` also allows a direct comparison with the learner's fresh two-frame generator. Across the 15 tested
synthetic and game-image-derived sequences, the 75%-versus-100% midpoint PSNR difference with zero and eight
preceding frames differed by at most 0.35 dB; the fence regression remained about 16 dB. This supports the approach
on these scenes but does not establish that history is unimportant in all games.
A camera-motion experiment reduced warp confidence where two visible samples disagreed. At the 1/3 and 2/3 phases
it reduced PSNR by about 0.21 dB on `real_turn` and 0.18 dB on `real_rotate`, and increased the RMS error of the
change between those phases from 11.03 to 11.31 and from 11.29 to 11.54 pixel levels, respectively. The experiment
was removed. `moonsr_imgtool temporal` reports this phase-to-phase difference error against ground truth; it is a
useful regression measure, not a direct measure of perceived flicker or game artifacting.
An additional subpixel-refinement step improved the 1/3 and 2/3 phase PSNR by about 0.27 dB on `real_turn`, and
its temporal-difference RMS changed from 11.03 to 10.75 pixel levels. In alternating 1080p RTX 5060 tests it added
roughly 0.15–0.3 ms of analysis work, depending on flow scale and run. In ATLYSS at 30-to-90 FPS the user saw little
difference in one area and fewer object glitches in another. MoonSR uses this refinement when 3x or more is requested;
this includes the period when the display cap reduces the effective multiplier. The 2x benchmark CSV was identical
to its previous baseline. Adaptive profiles for the refined and standard estimators use separate keys.
After comparing the rebuilt MoonSR and Lossless Scaling at 30-to-90 FPS, the user reported that MoonSR felt similarly
smooth and more responsive, with slightly more visual artifacts. This is a subjective comparison in one game and
does not establish equality on other scenes or GPUs.
In separate ten-second Windows GPU Engine counter samples from that 30-to-90 ATLYSS comparison, Lossless Scaling
averaged approximately 7.25% of the RTX 5060 3D engine and MoonSR approximately 20.87% with upscaling disabled,
100% flow, and precise motion. The tools ran at different times and their complete settings and scene motion were
not independently matched, so the figures indicate a gap to investigate rather than a controlled performance ratio.
An opt-in native-size generated-frame copy experiment measured 20.94% in another ten-second sample; it was removed
because the run did not establish an improvement. Whole-application GPU utilization remains above the comparison
sample, and isolated compute benchmarks do not prove performance parity.
An exact-match shortcut skips subpixel refinement once a block's matching error is zero, when further refinement
would yield a zero offset. In alternating precise-motion benchmarks using a transformed captured image, RTX 5060
1080p full-flow PushFrame medians were 2.51–2.53 ms versus 2.69–2.82 ms without the shortcut; at 75% flow they
were 1.67 ms versus 1.70–1.71 ms. On the A400 at 720p full flow, medians were 8.27–8.28 ms versus 8.40–8.47 ms.
These are isolated, scene-dependent GPU timings, not whole-application utilization. The 720p quality benchmark's
result CSV matched byte-for-byte at 75% and 100% flow, and both GPUs passed the frame-generation self-tests.
Skipping neighbor candidates that exactly equal the already-scored parent vector further reduced RTX 5060
1080p full-flow PushFrame medians from 2.527–2.531 ms to 2.470–2.479 ms in alternating runs. At 75% flow the
difference was small (1.668–1.671 ms versus 1.658–1.666 ms). One A400 720p full-flow comparison measured
8.198 ms versus 8.155 ms. Quality CSVs again matched byte-for-byte, and both GPU self-tests passed. These
measurements retain the exact-match shortcut in both baseline and candidate and do not measure application GPU use.
A further experimental early exit from the neighbor-candidate loop when its best score was zero measured
2.453–2.459 ms versus 2.476–2.478 ms at 1080p full flow on the RTX 5060. At 75% flow the results overlapped
(1.652–1.661 ms versus 1.654–1.657 ms). This additional branch was removed: the measured gain was below 1%
in this workload and did not establish a meaningful improvement across flow settings. Raw timing results are
under `build/main/alfg-zero-cost/results`; the experimental executable is not the main application build.
The development profiler now reports cross-direction splatting and comparison, previously absent from its stage
summary. With precise motion, transformed captured-image content, and 1080p input on the RTX 5060, a 100-pair
profile averaged search 1.267 ms, regularization 0.636 ms, refinement 0.259 ms, and HUD masking 0.196 ms at
100% flow; cross splatting/comparison together were 0.022 ms. At 75% flow the corresponding costs were
0.722, 0.370, 0.255, 0.191, and 0.014 ms. The blocking development profiler perturbs execution; these figures
identify optimization targets, not production latency or an application-level comparison with Lossless Scaling.
Logs are under `build/main/alfg-stage-profile/results`. Search and regularization dominate the measured analysis
work, while the HUD pass remains near-constant because it processes the original frame size.
An experiment reused the first candidate's shared-memory samples when a regularization neighbor proposed the
same vector. Alternating RTX 5060 1080p precise-motion runs regressed full-flow PushFrame medians from
2.476–2.482 ms to 2.510–2.514 ms; 75% flow also regressed. The branch and shared-memory reuse were removed.
Results are under `build/main/alfg-regularize-reuse/results`. This rules out this particular reuse implementation
on the tested workload, rather than duplicate-candidate elimination in general.
Moving the HUD surround-weight check before the 7x7 dilation also failed to establish an improvement:
1080p full-flow RTX 5060 medians overlapped (baseline 2.472–2.476 ms, experiment 2.468–2.481 ms).
The change was removed. Results are under `build/main/alfg-hud-surround/results`.
A software regularization experiment also skipped scoring when all nine proposed
vectors were identical, because downstream consumers use only the selected xy.
Alternating 1080p timings were variable: baseline full-flow PushFrame medians
2.468/2.481 ms versus candidate 2.480/2.375 ms; at 75% flow both varied from
roughly 1.65 to 1.79 ms. This did not establish a consistent useful gain, and the
shortcut was removed without promoting it. Raw results are under `build/main/alfg-unanimous/results`.

For future live comparisons, `tools/measure_gpu_process.ps1 -ProcessId <pid> -Seconds 20
-OutputDirectory <new-run-directory>` records raw Windows GPU Engine samples and a JSON summary by adapter and
engine type, including OFA when exposed by the driver. It checks process start time against PID reuse, records
missing samples separately, and rejects overall coverage below 80%. Per-engine observed sample counts must also
be reviewed. Percentages across distinct engines are not a normalized whole-GPU load and can sum above 100%.
The script was verified against a live desktop application's counters and a process without exposed counters;
this validation is not a new MoonSR/Lossless Scaling measurement. Neither ATLYSS nor a scaling process was open
at that check. Match scene motion, game/output rate, sync, flow scale, and background workload when collecting
new comparison runs; prior 3D-only samples remain insufficient for a whole-GPU comparison.

An exact unweighted search path now certifies that all source and loaded reference
weights equal 1 before avoiding their multiplication and accumulation. Masked
windows keep the weighted path. Alternating 1080p RTX 5060 precise-motion runs
measured PushFrame medians 2.322–2.324 ms versus 2.477–2.481 ms at full flow, and
1.587–1.589 ms versus 1.666–1.671 ms at 75%. One A400 720p comparison improved
8.414 to 8.139 ms at full flow and 5.082 to 4.972 ms at 75%. Performance quality
CSV output matched the prior precise-motion suite byte-for-byte; both GPU self-tests
passed. This change is included in the normal build. Results: `build/main/alfg-unweighted-search`.

On November 19, a new user-arranged ATLYSS 30-to-90 comparison collected 20 valid
Windows GPU counter samples per application. Lossless Scaling averaged 7.00% on
the RTX 5060 3D engine; rebuilt MoonSR averaged 19.74%. Copy usage was approximately
0.02% in each, and the exposed OFA counter was zero. These are per-engine counters,
not power/whole-device measurements, and the applications ran sequentially with
user-controlled motion. MoonSR used no upscaling, Performance, 100% flow, V-Sync,
3x requested/precise motion, and learning disabled. Its 26-second run logged near
30/90 FPS, one discarded capture, and one late generated frame. The user confirmed
LS's requested 3x/V-Sync setup; its complete internal configuration was not inspected.
Logs and raw samples: `build/main/gpu-comparison-20261119-*`. The sizeable observed
3D-engine gap remains unresolved; isolated search savings do not establish parity.
The user confirmed the MoonSR counter was visible and reported smoother or more
responsive motion with more visual artifacts. Tightening the boundary hypothesis
weight barely changed the quality suite (mean full-frame/edge PSNR change of
-0.0010/-0.0005 dB across 150 finite rows). Computing squared spread before the
boundary branch also gave no measurable interpolation reduction (0.262–0.263 ms
baseline, 0.263 ms candidate). Both experiments were reverted. Results are under
`build/main/alfg-boundary-evidence`.

The A400 capture-to-present check used a synthetic 1080p source at 30 FPS,
Performance mode, 50% flow, 2x, and V-Sync on the 1920x1200 60 Hz output.
The 16-second pattern run with learning off logged 959 presents, one dropped
capture, and no late generated frames. A separate 45-second run with learning
on used the slow-motion learning pattern and logged 2,597 presents, seven
dropped captures, four late generated frames, and one skipped generated frame.
ALFG evaluated four triplets and retained the configured 50% after detecting
a regression in its lower-flow candidate. The runs used different patterns,
so the difference in pacing cannot be attributed to learning alone. Logs are
`build/main/alfg-a400-headroom/live-pattern.txt` and `live-learning.txt`.
The matched 45-second slow-motion control with learning off presented 2,720
frames with zero dropped captures and zero late generated frames. An experiment
spacing learning evaluations one capture period apart presented 2,716 frames
with one dropped capture, but reported 342 late generated frames and loaded an
existing profile. The spacing change was reverted. See `live-learning-control.txt`
and `live-learning-spaced.txt` in the same directory.
At startup, a requested multiplier above 2x now begins at 2x while the game rate is measured, then rises when the
refresh-rate cap permits. In ATLYSS at 72 FPS on the 144 Hz display, requesting 3x started at 2x with zero discarded
captures in the 12-second check; an earlier run began at 3x and discarded 19 captures before the rate cap settled.
At 30 FPS the same request rose from 2x to 3x after roughly two seconds, also with zero discarded captures. These
short, separate runs do not establish pacing under every workload or display configuration.
The tile-versus-full-frame mismatch was then reproduced using the same synthetic frames as a held-out triplet:
full-frame Performance flow 75% scored 14.12 dB versus 30.39 dB at 100%, while all nine 160x160 grid crops scored
higher at 75% than at 100%. The tile learner and its recent-regression guard cannot reliably identify this failure.
On the RTX 5060, isolated PushFrame plus Interpolate medians at 720p were 0.88 ms (75%) and 1.31 ms (100%), versus
0.10 and 0.11 ms on 160x160 tiles. These timings do not include capturing extra frames, allocating full-resolution
generator resources, readback, memory impact, or contention with presentation; a full-frame validation design must
measure those costs before enabling it.

## GPU priority under contention, 2026-11-21

Owner report: neural rendering performs poorly when the game drives the GPU to high usage. Cause: MoonSR ran at the
default GPU scheduling priority, so a busy GPU was time-shared between the game and MoonSR. Fix: the engine raises
the process GPU scheduling class to REALTIME (fallback HIGH) and the device thread priority to +7 right after device
creation (`src/engine/gpu_priority.h`). `MOONSR_GPU_PRIORITY=normal` disables it for A/B runs.

Setup: RTX 5060, driver 621.14, Windows 11 26300, hardware-accelerated GPU scheduling (HAGS) on, process not
elevated (no `SeIncreaseBasePriorityPrivilege`). Competing load: `moonsr_gpuload` in a separate process at normal
priority (two queued batches of ALU-bound compute at all times, about 8 ms per batch). Working tree on `d9bde08`.
Model: DLSS NR 310.9.16.0 (patched FP16), 1 pass, 100 % working resolution, 1280x720.

Mechanism check — a fixed 2.9 ms compute job (`moonsr_gpuload --probe --iterations 1200`) beside the load:

| Probe process class | p50 | p95 | p99 |
|---|---|---|---|
| idle GPU, no load | 2.91 ms | 2.96 ms | 3.12 ms |
| NORMAL | 7.42 ms | 7.62 ms | 15.6 ms |
| ABOVE_NORMAL | 7.41 ms | 7.71 ms | 15.3 ms |
| HIGH (+ device priority 7) | 7.42 ms | 7.75 ms | 18.2 ms |
| REALTIME | 3.10 ms | 3.15 ms | 3.28 ms |
| NORMAL, load process at IDLE | 3.08 ms | 3.11 ms | 3.14 ms |

Under HAGS, NORMAL to HIGH form one band. Only REALTIME (or lowering the competitor) puts the work ahead.

Neural rendering bridge alone (`MoonSR_NrBridgeSmoke [--priority] --timing <model> "RTX 5060"`, 160 frames, CPU
wall time per frame including completion; stage times are the bridge's GPU timestamps, `MOONSR_NR_TRACE=1`):

| Case | p50 | p95 | max | flow / model stage |
|---|---|---|---|---|
| idle GPU | 3.80 ms | 4.03 ms | 4.11 ms | 0.7 / 2.6 ms |
| full load, normal priority | 15.74 ms | 17.90 ms | 20.79 ms | 4.3–8.7 / 3.4–7.2 ms |
| full load, MoonSR priority | 4.65 ms | 4.72 ms | 4.82 ms | 1.0–1.2 / 2.6 ms |

Live pipeline (`moonsr_testpattern --fps 120` 1280x720, `moonsr_cli --scaler lanczos --adapter "RTX 5060"
--neural <id> --seconds 16`, output 1920x1080 at 144 Hz, V-Sync off):

| Case | captured / presented fps | NR frames | dropped | load batches in 24 s |
|---|---|---|---|---|
| idle GPU | 120.0–120.4 | 1883 | 2 | — |
| full load, `MOONSR_GPU_PRIORITY=normal` | 45.2–51.8 | 597 | 19 | 2553 |
| full load, realtime priority | 119.6–120.4 | 1878 | 2 | 1891 |

With priority the competing load lost 26 % of its throughput: the game pays for MoonSR's work, as intended.
Remaining cost under load: about 0.85 ms per NR frame, mostly in the optical-flow stage (NVOFA runs on a
driver-owned queue whose priority MoonSR does not set).

## GPU per stage: cross-GPU copies, 2026-11-22

Machine: Ryzen 9 7900X, RTX 5060 and RTX A400 (driver 621.14 / 32.0.16.2114), integrated AMD Radeon(TM) Graphics
(31.0.24002.92), Windows 11 26300, base commit `2689927` plus this change, RelWithDebInfo, GPUs otherwise idle.

`CrossAdapterLink` (`src/gpu/cross_adapter.cpp`), `moonsr_cross_adapter_smoke --bench`, 1920x1080, 300 iterations,
medians in ms. Measured by the gpu-rendering-engineer; the lead re-ran it and reproduced the Radeon -> A400 row
(GPU ready 2.237 vs 2.231, pipelined 1.332 vs 1.334). "Call" = CPU time of Transfer when the previous one has
finished; "ready" = until the destination can read the frame; "pipe" = back-to-back throughput per frame.

| Pair | Format | GPU path call | GPU ready (p95) | GPU pipe | CPU staging call | CPU ready (p95) | CPU pipe |
|---|---|---|---|---|---|---|---|
| 5060 -> A400 | BGRA8 | 0.140 | 1.639 (1.691) | 1.326 | 0.840 | 2.193 (2.275) | 1.322 |
| A400 -> 5060 | BGRA8 | 0.128 | 1.742 (1.788) | 1.332 | 1.797 | 2.127 (2.195) | 1.843 |
| 5060 -> Radeon | BGRA8 | 0.135 | 1.023 (1.218) | 0.781 | 0.857 | 1.670 (1.927) | 1.388 |
| Radeon -> 5060 | BGRA8 | 0.136 | 1.508 (1.647) | 1.208 | 1.880 | 2.210 (2.364) | 1.741 |
| A400 -> Radeon | BGRA8 | 0.137 | 1.875 (1.950) | 1.306 | 1.820 | 2.801 (2.931) | 1.869 |
| Radeon -> A400 | BGRA8 | 0.145 | 2.231 (2.359) | 1.334 | 2.109 | 3.464 (3.598) | 1.407 |

The GPU path (D3D12 cross-adapter heap in system memory, shared fences) was chosen automatically for all six pairs.
RGBA16F costs about twice BGRA8 and 3840x2160 about four times 1080p (5060 -> A400 BGRA8 at 4K: 5.95 ms ready, 5.24 ms pipe).
The A400 pairs top out near 1.3 ms per 1080p BGRA8 frame, likely the A400's PCIe link.

Live (`moonsr_testpattern` 1280x720 at 30 fps, `moonsr_cli --fg 2 --scaler lanczos --monitor 0 --seconds 10`, output
1920x1080 at 144 Hz on the 5060's monitor):

| Stage GPUs | Captured / presented | Dropped | Late |
|---|---|---|---|
| all Auto (5060) | 304 / 601 | 0 | 0 |
| Frame generation on A400 | 304 / 601 | 2 | 0 |
| Upscaling (main) on A400, other stages Auto (5060) | 303 / 600 | 2 | 0 |
| Frame generation on A400, Moon ALSR | 303 / 599 | 2 | 0 |
| Frame generation on Radeon iGPU | 296 / 575 | 5 | 1 |

Neural rendering on the A400 with the 5060 presenting: refused (NVIDIA runtime, see NR_BRIDGE.md). All stages on the
A400 with neural rendering: works (168 captured, 92 presented in 6 s; the A400 is too slow for 30 fps NR).

## Moon ALSR baseline (temporal harness), 2026-11-21

> Naming note (2026-11-22): "Moon ALSR" in the sections below is now named MoonSR in the product (the scaler list shows
> "MoonSR (experimental)"; CLI `--scaler moonsr`, alias `alsr`). Recorded numbers are unchanged. The learning options
> referred to below (`--alsr-learning`, `--nolearn`, `--fg-learn`) and the learners themselves were removed on
> 2026-11-22; those runs cannot be repeated with the current tree. Timing diagnostics are now under `--fg-diag`.

First measurement of Moon ALSR after the offline `tools/tbench` was removed: a new temporal mode of `moonsr_bench`
(`--temporal`, sources `tools/bench/temporal*.cpp`) with exact ground truth. Measured by the benchmark-specialist.

**Tree**: commit `814bc0d` + uncommitted harness (`tools/bench/temporal.h`, `temporal.cpp`, `temporal_seq.cpp`, edits to
`main.cpp`, `selftest.cpp`, `gpu_runner.h`, `report.cpp`, `CMakeLists.txt`). Source hash printed by the tool:
`e7b612144639a9ee (73 files)`. Build: `powershell -ExecutionPolicy Bypass -File tools\build.ps1 -BuildDir build\bench2 -Target moonsr_bench`
(RelWithDebInfo, /W4 clean). OS Windows 11 build 10.0.26300.9974, Ryzen 9 7900X, RTX 5060 and RTX A400 (both driver
621.14 / UMD 32.0.16.2114). Both GPUs ran in one invocation, so they scored identical inputs.

**Commands**

```
build\bench2\bin\moonsr_bench.exe --selftest --adapter 5060
build\bench2\bin\moonsr_bench.exe --temporal --adapter 5060 --adapter A400 --tag baseline
build\bench2\bin\moonsr_bench.exe --temporal --adapter 5060 --seqs pan_slow,scene_cut --scales 1.5 --tag det
build\bench2\bin\moonsr_bench.exe --temporal --adapter 5060 --seqs pan_slow,hud_pan --scales 1.5,2 --methods alsr --alsr-learning --tag learn   # option removed 2026-11-22
```

Defaults used: 1920x1080 ground truth, 64 frames per sequence, ground-truth supersampling 3x3, warm-up 8 frames excluded
from every average, sharpness 0 for all methods, Moon ALSR `AlsrSettings` defaults except `learning = false` and
`useLearned = false` (stability 0.85), `FrameGenSettings` defaults, no `SetProfile` (no persistence). Scales 1.5x
(1280x720 input) and 2x (960x540 input). Raw CSV: `temporal_summary_t_baseline.csv` (one row per run) and
`temporal_frames_t_baseline.csv` (one row per frame), written to `benchmarks\output\results` (git-ignored output dir).

**What is measured.** Seven procedural sequences (pure functions of x, y, frame, so the ground truth is exact):
`pan_slow` (1.1, 0.5 px/frame at 1080p), `pan_fast` (17.3, 6.1), `rot_zoom` (rotating, zooming 560 px disc with spokes
over a static background), `hud_pan` (3.7, 1.3 pan under a static HUD: health bar, seven-segment score on a translucent
panel, ammo ticks, 1 px crosshair), `scene_cut` (pan, hard cut to another scene at frame 32 with different motion),
`thin_pan` (1 px horizontal/vertical/diagonal lines and rings panning at 0.73, 0.41 px/frame), `still` (no motion).
The game-resolution frame is either **aliased** (point sample of the continuous scene at input pixel centres, as an
engine without AA renders) or **area** (exact box average of the 3x3-supersampled ground truth). Each frame goes through
the real `ScalePass` (Lanczos-3, MoonSR Spatial) or through `FrameGenerator::PushFrame` + `AlsrUpscaler::Render` on the same
`GpuDevice`, is read back and scored against the ground truth.

Metrics: luma PSNR (BT.709 on gamma-encoded values, float, peak 255), luma SSIM (11x11 Gaussian), **flicker** = mean
|e_t - e_(t-1)| of the luma error e = output - ground truth in 8-bit levels (excludes the scene-cut frame itself), HUD =
the same on the four HUD rectangles (MSE over their union; SSIM = mean of the four crops), cut window = mean over frames
32..39 (cut frame = 32), plus the single cut frame. Averages: arithmetic mean of per-frame values over frames 8..63.
GPU time is measured in a separate pass (scoring leaves the GPU idle between frames): the whole sequence is submitted
back to back twice with D3D11 timestamp queries around every call, and frames 8..63 of the second pass are reported.

**Validation of the harness** (`--selftest`, 0 failures, 22 temporal checks): scorer identity gives PSNR inf / SSIM 1 /
flicker 0; a constant +5 offset gives exactly 20 log10(255/5) = 34.151 dB; alternating +5/-5 gives flicker 10.000; an
error confined to a HUD rectangle matches the analytic HUD and frame PSNR (30.069 dB hud); an integer pan of (4, 2)
px/frame translates the ground truth with 0 of 107352 samples differing; point sampling at 1x equals the ss=1 render;
2x area input equals the 2x2 box mean; an end-to-end 1x run through the GPU runner reproduces the ground truth exactly
(PSNR inf); the runner output equals the one-shot `ScalePass` output; per-frame PSNR/SSIM equal `Compare()`; Moon ALSR
output is bit-identical across two runs. Determinism of the full harness: a rerun of 12 runs (pan_slow, scene_cut, 1.5x,
both input modes, 3 methods, RTX 5060) matched the baseline in output hash, ground-truth hash, PSNR, SSIM and flicker
in all 12; every one of the 84 RTX A400 runs produced output bit-identical to the RTX 5060 (FNV-1a of all frames).

### Results (RTX 5060; quality is identical on the RTX A400)

##### 1.5x, aliased input, RTX 5060 (luma PSNR dB / SSIM / flicker; best of the three in bold per metric)

| Sequence | Lanczos-3 | MoonSR Spatial | Moon ALSR |
|---|---|---|---|
| pan_slow | **37.81** / **0.9810** / **2.01** | 37.45 / 0.9754 / 2.20 | 36.56 / 0.9749 / 2.31 |
| pan_fast | **37.79** / **0.9808** / **2.11** | 37.39 / 0.9751 / 2.38 | 37.39 / 0.9773 / 2.33 |
| rot_zoom | **35.84** / **0.9790** / **0.53** | 35.60 / 0.9733 / 0.53 | 34.56 / 0.9668 / 0.83 |
| hud_pan | **35.84** / **0.9794** / **2.04** | 35.55 / 0.9741 / 2.29 | 34.96 / 0.9763 / 2.30 |
| scene_cut | **37.66** / **0.9810** / **2.07** | 37.30 / 0.9753 / 2.45 | 36.62 / 0.9767 / 2.44 |
| thin_pan | **14.42** / **0.7264** / 40.80 | 13.68 / 0.7109 / 42.14 | 13.91 / 0.7071 / **40.39** |
| still | **37.75** / **0.9807** / **0.00** | 37.36 / 0.9742 / **0.00** | 37.36 / 0.9741 / **0.00** |

##### 1.5x, area input, RTX 5060 (luma PSNR dB / SSIM / flicker; best of the three in bold per metric)

| Sequence | Lanczos-3 | MoonSR Spatial | Moon ALSR |
|---|---|---|---|
| pan_slow | 40.08 / 0.9772 / 2.03 | 40.07 / 0.9708 / 2.13 | **40.79** / **0.9845** / **1.79** |
| pan_fast | **40.09** / **0.9771** / **2.24** | 40.05 / 0.9707 / 2.43 | 40.01 / 0.9748 / 2.31 |
| rot_zoom | 38.61 / **0.9767** / 0.47 | **39.05** / 0.9708 / **0.43** | 38.05 / 0.9653 / 0.65 |
| hud_pan | 38.54 / 0.9765 / 2.16 | **39.00** / 0.9707 / 2.36 | 38.57 / **0.9780** / **2.09** |
| scene_cut | 39.96 / 0.9772 / 2.32 | 40.00 / 0.9708 / 2.55 | **40.52** / **0.9827** / **2.03** |
| thin_pan | 17.65 / 0.8185 / 29.21 | **18.23** / **0.8497** / **26.59** | 18.02 / 0.8330 / 26.95 |
| still | **40.02** / **0.9770** / **0.00** | 40.00 / 0.9704 / **0.00** | 39.99 / 0.9703 / **0.00** |

##### 2x, aliased input, RTX 5060 (luma PSNR dB / SSIM / flicker; best of the three in bold per metric)

| Sequence | Lanczos-3 | MoonSR Spatial | Moon ALSR |
|---|---|---|---|
| pan_slow | 35.08 / 0.9437 / 3.51 | 35.14 / 0.9407 / 3.39 | **35.71** / **0.9683** / **2.52** |
| pan_fast | 35.07 / 0.9429 / 3.72 | 35.08 / 0.9396 / 3.73 | **35.14** / **0.9510** / **3.36** |
| rot_zoom | 33.18 / **0.9202** / 0.79 | **33.24** / 0.9109 / **0.75** | 32.56 / 0.9024 / 1.02 |
| hud_pan | 34.33 / 0.9424 / 3.45 | 34.53 / 0.9398 / 3.56 | **34.83** / **0.9633** / **2.84** |
| scene_cut | 34.96 / 0.9438 / 3.90 | 35.02 / 0.9408 / 3.90 | **35.67** / **0.9661** / **2.88** |
| thin_pan | **12.00** / 0.5192 / 53.56 | 11.57 / 0.5209 / 54.36 | 11.86 / **0.5259** / **51.44** |
| still | **34.56** / **0.9181** / **0.00** | 34.48 / 0.9068 / **0.00** | 34.48 / 0.9066 / **0.00** |

##### 2x, area input, RTX 5060 (luma PSNR dB / SSIM / flicker; best of the three in bold per metric)

| Sequence | Lanczos-3 | MoonSR Spatial | Moon ALSR |
|---|---|---|---|
| pan_slow | 37.43 / 0.9505 / 2.93 | 37.77 / 0.9445 / 2.88 | **39.20** / **0.9733** / **2.28** |
| pan_fast | 37.42 / 0.9499 / 3.27 | 37.72 / 0.9436 / 3.32 | **37.75** / **0.9529** / **3.11** |
| rot_zoom | 35.45 / **0.9273** / 0.65 | **35.87** / 0.9169 / **0.57** | 35.09 / 0.9084 / 0.85 |
| hud_pan | 36.54 / 0.9497 / 3.04 | 37.28 / 0.9444 / 3.21 | **37.50** / **0.9640** / **2.63** |
| scene_cut | 37.33 / 0.9507 / 3.49 | 37.71 / 0.9447 / 3.53 | **38.76** / **0.9686** / **2.73** |
| thin_pan | 15.51 / 0.6925 / 36.54 | **15.93** / **0.7431** / **34.17** | 15.58 / 0.6982 / 35.48 |
| still | 36.35 / **0.9236** / **0.00** | **36.41** / 0.9115 / **0.00** | 36.40 / 0.9114 / **0.00** |

##### HUD region (hud_pan) and scene-cut window (scene_cut, frames 32..39), RTX 5060

| Scale | Input | Method | HUD PSNR | HUD SSIM | HUD flicker | cut-frame PSNR | cut window PSNR | cut window SSIM |
|---|---|---|---|---|---|---|---|---|
| 1.5x | aliased | Lanczos-3 | 24.50 | 0.8963 | 1.46 | 37.45 | 37.48 | 0.9809 |
| 1.5x | aliased | MoonSR Spatial | 24.34 | 0.8999 | 1.59 | 37.07 | 37.13 | 0.9752 |
| 1.5x | aliased | Moon ALSR | 23.09 | 0.8831 | 2.62 | 37.07 | 36.49 | 0.9769 |
| 1.5x | area | Lanczos-3 | 27.96 | 0.9474 | 1.46 | 39.79 | 39.83 | 0.9771 |
| 1.5x | area | MoonSR Spatial | 29.66 | 0.9584 | 1.59 | 39.88 | 39.92 | 0.9707 |
| 1.5x | area | Moon ALSR | 27.68 | 0.9315 | 2.96 | 39.88 | 40.23 | 0.9796 |
| 2x | aliased | Lanczos-3 | 26.31 | 0.8653 | 2.35 | 34.34 | 34.74 | 0.9396 |
| 2x | aliased | MoonSR Spatial | 27.22 | 0.8714 | 2.46 | 34.30 | 34.80 | 0.9354 |
| 2x | aliased | Moon ALSR | 25.76 | 0.8541 | 3.43 | 34.30 | 35.22 | 0.9526 |
| 2x | area | Lanczos-3 | 27.90 | 0.9119 | 2.09 | 36.20 | 37.05 | 0.9463 |
| 2x | area | MoonSR Spatial | 30.74 | 0.9232 | 2.22 | 36.32 | 37.42 | 0.9394 |
| 2x | area | Moon ALSR | 28.42 | 0.8909 | 3.44 | 36.32 | 37.90 | 0.9518 |

##### GPU time per frame (ms, mean / median / p95 over frames 8..63 of the second back-to-back pass; ALSR = Render + motion estimator PushFrame shown separately)

| GPU | Scale | Method | mean over 7 sequences, aliased (ms) | min..max over sequences (ms) | motion estimator (ms, mean) |
|---|---|---|---|---|---|
| 5060 | 1.5x | Lanczos-3 | 0.391 | 0.120..0.762 | - |
| 5060 | 1.5x | MoonSR Spatial | 0.286 | 0.232..0.450 | - |
| 5060 | 1.5x | Moon ALSR | 0.794 | 0.597..0.953 | 0.450 |
| 5060 | 2x | Lanczos-3 | 0.411 | 0.120..0.840 | - |
| 5060 | 2x | MoonSR Spatial | 0.270 | 0.223..0.474 | - |
| 5060 | 2x | Moon ALSR | 0.550 | 0.436..0.717 | 0.291 |
| A400 | 1.5x | Lanczos-3 | 0.983 | 0.911..1.085 | - |
| A400 | 1.5x | MoonSR Spatial | 1.840 | 1.683..1.919 | - |
| A400 | 1.5x | Moon ALSR | 4.581 | 4.214..5.554 | 2.540 |
| A400 | 2x | Lanczos-3 | 0.970 | 0.934..1.002 | - |
| A400 | 2x | MoonSR Spatial | 1.849 | 1.786..1.883 | - |
| A400 | 2x | Moon ALSR | 3.682 | 3.312..4.820 | 1.470 |

##### Moon ALSR GPU time per sequence (ms mean: Render / motion estimator), aliased input

| GPU | Scale | pan_slow | pan_fast | rot_zoom | hud_pan | scene_cut | thin_pan | still |
|---|---|---|---|---|---|---|---|---|
| 5060 | 1.5x | 0.770 / 0.463 | 0.884 / 0.450 | 0.953 / 0.431 | 0.757 / 0.457 | 0.597 / 0.438 | 0.832 / 0.506 | 0.765 / 0.406 |
| 5060 | 2x | 0.602 / 0.310 | 0.490 / 0.288 | 0.717 / 0.290 | 0.485 / 0.311 | 0.466 / 0.289 | 0.653 / 0.294 | 0.436 / 0.255 |
| A400 | 1.5x | 4.214 / 2.573 | 4.283 / 2.488 | 4.664 / 2.350 | 4.415 / 2.546 | 4.696 / 2.900 | 5.554 / 2.672 | 4.239 / 2.251 |
| A400 | 2x | 3.344 / 1.527 | 3.458 / 1.465 | 3.811 / 1.389 | 3.641 / 1.501 | 3.391 / 1.516 | 4.820 / 1.511 | 3.312 / 1.384 |

##### Moon ALSR minus the best spatial method (luma PSNR dB; negative = ALSR loses), RTX 5060

| Sequence | 1.5x aliased | 1.5x area | 2x aliased | 2x area |
|---|---|---|---|---|
| pan_slow | -1.25 | +0.71 | +0.57 | +1.42 |
| pan_fast | -0.40 | -0.07 | +0.05 | +0.03 |
| rot_zoom | -1.28 | -0.99 | -0.68 | -0.78 |
| hud_pan | -0.88 | -0.44 | +0.30 | +0.22 |
| scene_cut | -1.03 | +0.51 | +0.65 | +1.05 |
| thin_pan | -0.51 | -0.20 | -0.14 | -0.35 |
| still | -0.40 | -0.03 | -0.08 | -0.01 |

Output bit-identity RTX A400 vs RTX 5060 (FNV-1a of all 64 output frames): 84 of 84 runs identical.


**Timing caveat.** The RTX 5060 per-frame times are not stable: the same Lanczos-3 pass at 2x measured 0.120 ms median on
`pan_fast`/`rot_zoom` but 0.33-0.70 ms on other sequences (p95 up to 1.65 ms), so differences of a few tenths of a
millisecond on that GPU are noise (probably clock/power state, not investigated). The RTX A400 is steady (Lanczos 0.91-1.09 ms,
MoonSR Spatial 1.68-1.92 ms, per sequence mean). Treat the A400 column as the usable cost figure; the 5060 figures only say
that all methods are well under 1 ms there. The motion estimator is a separate cost that the engine pays anyway when frame
generation is on.

**Learning (`--alsr-learning`, removed 2026-11-22; RTX 5060, pan_slow and hud_pan).** At 2x learning is not supported by the upscaler
(`learningScaleSupported = 0`, 0 samples) and the output equals the learning-off output exactly. At 1.5x (1.28 M samples, 4
solves, learned filters active, coverage 0.39-0.69 after 64 frames) the change is +0.001 to +0.012 dB luma PSNR (pan_slow aliased
36.564 -> 36.565, hud_pan area 38.565 -> 38.577). 64 frames is far shorter than a real session, so this says nothing
about long-run learning; learning is not part of the baseline above and the learning runs are not bit-deterministic by design
(worker thread).

**Reading the results (plain statements).**
- Moon ALSR beats the best spatial scaler on the panning content with area (anti-aliased) input (e.g. pan_slow 2x +1.42 dB,
  scene_cut 2x +1.05 dB, SSIM 0.9733 vs 0.9505 at 2x) and at 2x aliased input (pan_slow +0.57 dB, scene_cut +0.65 dB, hud_pan +0.30 dB, SSIM up to
  +0.025 over Lanczos-3), with the lowest error flicker on those sequences; on pan_fast at 2x the gain is only +0.03 to +0.05 dB.
- Moon ALSR **loses** at 1.5x with aliased input on every sequence with motion: -1.25 dB (pan_slow), -0.40 (pan_fast), -1.28
  (rot_zoom), -0.88 (hud_pan), -1.03 (scene_cut) against Lanczos-3 in PSNR, although its SSIM is above MoonSR Spatial on pan_fast, hud_pan and scene_cut (and below Lanczos-3). It also loses at 1.5x area on pan_fast, rot_zoom, hud_pan and thin_pan, and on `rot_zoom` at every setting
  (-0.68 to -0.99 dB at 2x and 1.5x area, -1.28 dB at 1.5x aliased; flicker 0.83 vs 0.53 at 1.5x aliased): content with rotation/zoom is where the image-based motion
  estimate is least reliable.
- In the HUD rectangles Moon ALSR is below MoonSR Spatial in all four configurations and below Lanczos-3 in three of four
  (1.5x aliased 23.09 dB vs 24.50 Lanczos / 24.34 Spatial; the exception is 2x area, 28.42 vs 27.90 Lanczos / 30.74 Spatial),
  and its HUD flicker is higher in all four (2.62-3.44 vs 1.46-2.46 levels), i.e. the static overlay is not protected as well as
  a single-frame upscale. Whole-frame numbers hide this because the HUD covers about 3% of the frame.
- Scene cut: the cut frame scores exactly the same as MoonSR Spatial in all four configurations (e.g. 37.07 = 37.07 dB at 1.5x
  aliased; history restarts from the single-frame result), which is 0.04-0.38 dB below Lanczos-3 in two configurations (aliased) and 0.09-0.12 dB above it in the other two (area). The 8-frame window after the cut
  is 0.4-0.9 dB below ALSR own sequence mean (2x aliased 35.22 vs 35.67 dB, 2x area 37.90 vs 38.76) but still above the spatial
  scalers at 2x (34.74 / 34.80 aliased, 37.05 / 37.42 area); at 1.5x aliased the window is 0.99 dB below Lanczos-3 (36.49 vs 37.48).
- `thin_pan` (1 px lines) is bad for every method (PSNR 12-18 dB); ALSR is below the best spatial method in all four configurations (by 0.14-0.51 dB); it is above Lanczos-3 only at 1.5x area (+0.37 dB).
- `still`: ALSR reproduces the single-frame result (0.01-0.40 dB below Lanczos-3, zero flicker). It is not better than
  Lanczos-3 on static content at any setting; MoonSR Spatial is below Lanczos-3 in SSIM on every sequence here.
- MoonSR Spatial is within 0.8 dB of Lanczos-3 in PSNR (ahead by up to +0.74 dB on hud_pan 2x area, behind by up to 0.74 dB, thin_pan 1.5x aliased) and
  0.002-0.012 lower in SSIM on every sequence except thin_pan, where it is higher.

**Known limitations of the harness (what it does not represent about real games).**
- Synthetic content: hashed discs/rectangles, value noise and vector HUD. No real textures, lighting, particles, motion blur,
  transparency, film grain or video compression, no post-processing, no sub-pixel camera jitter and no TAA. Real game frames
  differ in ways that matter to a temporal upscaler.
- Motion is rigid translation, one rotation/zoom and one cut, with constant velocity at an exact 60 fps cadence; no frame-time
  jitter, dropped frames, parallax or independently moving objects.
- Aliased input is a single point sample of a continuous function (perfectly aliased); real engines mix MSAA, TAA and
  shader-level filtering. The ground truth is a 3x3 box-filtered render, so "exact" means exact for that definition; PSNR
  against it penalises sharper-than-box reconstructions.
- The HUD here is static and fully opaque/translucent vector art authored for 1080p; real HUDs animate and use fonts.
- Metrics are luma only (no chroma, no perceptual metric such as FLIP or LPIPS); flicker is an error-difference statistic, not
  a visibility model. HUD SSIM is the mean over four crops, not a masked SSIM map.
- Capture path effects are not modelled (Windows Graphics Capture format/timing, compression, cursor); input is exact BGRA8.
- Timing is an isolated back-to-back GPU time at idle on one process, not in-game cost (no competing game load, no capture or
  present), and see the timing caveat above. Quality runs score one frame at a time with a blocking readback.
- Learning is off in the baseline; the learned-filter path was measured only for 64 frames.
- No reference third-party upscaler (DLSS, FSR, XeSS) was measured: not measurable on captures, see the reference table above.

## Frame generation Fixed/Auto modes (2026-11-22)

Owner session 7. Figures are from the lead's verification run, as recorded in the session brief. Not verified here:
the raw output files, the exact command lines and the application commit (the working tree was uncommitted).

**Setup.**
- Machine: as in "Test machine". Output: RTX 5060 on the 1920x1080 @ 144 Hz display.
- Source: `moonsr_testpattern --motion-pattern 1` at 60 fps (test pattern resolution not recorded in the brief).
- Run length: 8 s per run. Values are averages of seconds 3 to 8 (the first seconds are excluded).
- V-Sync off unless the row says `--vsync`.
- Frame generation flags: `--fg 2`, `--fg 20`, `--fg 20 --vsync`, `--fg-auto 120`, `--fg-auto 300`,
  `--fg-auto 300 --vsync`, and `--scaler moonsr --fg-auto 144` (the last one with the MoonSR scaler).
- "Interval" is the presented frame interval (median / p95, ms). "Ratio" is the value printed by the CLI Summary line.
  "Late" and "skipped" are the counts from the same run. Displayed fps is presented frames per second; it is not game
  fps and not responsiveness.

**Results.**

| Run | Displayed fps | Ratio | Interval median / p95 (ms) | Late | Skipped |
|---|---|---|---|---|---|
| `--fg 2` | 119.7 | 2.01 | 8.10 / 12.98 | not recorded | 0 |
| `--fg 20` | 1201.1 | 19.93 | 0.83 / 0.86 | not recorded | 0 |
| `--fg 20 --vsync` | 119.9 (limited to 2X, note shown) | not recorded | 8.32 / 12.72 | not recorded | not recorded |
| `--fg-auto 120` | 117.4 | 1.97 | 8.33 / 8.95 | 7 | 0 |
| `--fg-auto 300` | 286.5 | 4.82 | 3.33 / 4.15 | 50 | 2 |
| `--fg-auto 300 --vsync` (target capped to 144) | 137.4 | not recorded | 6.94 / 9.18 | 24 | 0 |
| `--scaler moonsr --fg-auto 144` | 141.0 | 2.35 | 6.94 / 8.07 | 35 | 0 |

"Not recorded" means the value is not in the session brief; it is not an estimate.

**Latency** (arrival-to-present, MoonSR's own processing; excludes game render, DWM and display scan-out). From the
frame-generation agent's runs, same setup per the brief; the command lines for these runs are not recorded here.

| Mode | Latency |
|---|---|
| Fixed 2x | 8.4 ms |
| Auto 90 | 14.6 ms |
| Auto 120 | about 10.7 ms |
| Auto 144 | 15.8 ms |
| Auto 300 | 16.0 ms |
| 40 fps input, Auto 120 | 22.5 ms |

**Reading the results.**
- Auto presents a few percent below target (117.4 of 120; 286.5 of 300).
- Auto at fractional ratios (for example 1.97 and 4.82) keeps the 14-17 ms latency band; Fixed 2x is lower. The Auto
  mode accepts this to keep the grid steady (ARCHITECTURE.md A12).
- With V-Sync on, the target is capped to the refresh rate (144 Hz here).
- The GPU-limit controller (skipping above 10% of generated slots in two windows) did not trigger on this machine. Its
  lowering path was exercised only in the fgbench simulation.
- Limits: one GPU, one display, one test pattern at 60 fps, one 8 s length per row. No real game, no other GPU.
