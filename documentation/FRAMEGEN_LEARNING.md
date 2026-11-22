# Adaptive frame-generation learning

> Removed 2026-11-22 (owner decision): frame-generation adaptive learning (`adaptive_controller`, sampler, policy, profile) is deleted
> together with its UI, settings keys and CLI flags. This file describes the removed feature and is kept for history
> only. Old profile folders under `%LOCALAPPDATA%\MoonSR\profiles` are left on disk and ignored.

Adaptive learning is optional and is off by default. Enable it on the Frame generation page while using a game.
The selected flow scale remains the maximum: learning tries one lower setting and never changes the requested
frame multiplier. Disabling learning immediately restores the selected scale.

The learner occasionally samples three successive real inputs received by frame generation. Capture may have
discarded intermediate frames; the learner accepts such triplets only when the timestamps remain sufficiently
evenly spaced and rejects long gaps or unstable input. It analyzes the full first and third frames, then reads back a
small region of the generated and held-out middle frames for comparison. Samples with a capture discontinuity,
large timing imbalance, little motion, or an obvious cut are excluded. Two thirds of attempts explore the image;
they sample distributed positions across the available area, starting at a different position in each session so
short sessions can cover different regions. The remainder can revisit a recently useful region. It compares the Performance profile at the
selected flow scale with a candidate at roughly 75% of that scale. If it fails after enough evidence, the learner
tries a gentler candidate at roughly 90%. Both are rounded down to a 5% step and no lower than 25%. At 25% there
is no lower candidate.

The candidate is used only after at least 24 training and 8 validation observations, when both sets remain within
2% of the baseline mean squared error, serious validation regressions are infrequent, and GPU timing measured on
the full-frame evaluations indicates a 5% improvement. Timing alternates the evaluation order and compares the median of
the second evaluation in each order, reducing the effect of queued presentation work. This is an experimental
sample-based estimate, not a guarantee of whole-frame speed or visual quality for every scene. The current setting
is restored if subsequent evidence no longer passes the gate. A separate recent-scene guard also rejects the lower
scale if at least three of the latest four samples have more than 50% greater mean squared error plus a small
absolute margin; it clears as newer evidence replaces those samples. This can detect a changed scene sooner than
waiting for the full history to shift, but depends on suitable motion samples becoming available. Full-frame analysis
was added after a repeating-fence test showed that independently analyzed 160x160 tiles can favor a flow scale that
performs substantially worse on the full frame. Only a small area is scored each time, so localized failures can still
be missed. Full-frame profiles use a separate key and do not reuse or delete older tile-only evidence. Profiles also
separate the more precise motion estimator used for 3x-and-higher requests from the standard estimator.

Full-context evaluations are spaced at least eight seconds apart in normal use and are suppressed during sustained
overload. The baseline and candidate GPU work is split across separate submission opportunities. If V-Sync is on and
an already stale generated frame is more than 1.5 slots late, MoonSR may omit it to recover delay; real frames are
never deliberately skipped by this recovery rule. The Details section reports generated frames late and skipped.
When available, the GPU memory budget is checked conservatively before full-context allocation; failure is reported
in the learning status and allocation retries are delayed. Memory and timing costs vary by GPU and resolution.

## Development measurements

One isolated 720p test on the
RTX 5060 reported approximately 87 MiB additional local GPU allocation and 2.3 ms of combined GPU timestamp time;
subsequently reusing a single full-size output reduced the same test's reported allocation to about 84 MiB. Copying
only the scored region of the held-out middle frame reduced it further to about 80 MiB. Both comparison orders
passed the repeating-pattern regression test on the RTX 5060 and RTX A400. Full-frame results are still scored from
a small readback.
These figures depend on scene, driver and device. Two 30-second ATLYSS tests at 1080p and 72-to-144 FPS gathered
five and seven valid full-context samples respectively, with the second run reloading the first run's profile. The
user reported no noticeable stutters in the second run; MoonSR nevertheless recorded four and three dropped captures
and hundreds of generated-frame late flags. Those flags and the user report measure different things, so the
prototype remained diagnostic during scheduling and live presentation investigation.
The later prototype separated the baseline and candidate full-frame evaluations by at least half an input-frame
interval (bounded to 2–8 ms), with separate GPU timestamps and cancellation of partial work. In a third ATLYSS run,
11 valid comparisons were collected, with seven discarded captures and 320 late generated-frame flags; the user
reported motion felt about the same as the previous full-context run. The earlier repeat recorded three discarded
captures and 609 late flags while collecting seven comparisons. These short, non-identical runs do not establish
that splitting improved overall pacing.
In a 45-second run with checks spaced eight seconds apart, the user reported smooth motion, but the estimated
capture-to-present delay rose to roughly 24 ms and 1,435 generated frames were flagged late, with zero discarded
captures. A subsequent 30-second control with learning disabled after startup reported roughly 8 ms at the end,
zero late generated frames and zero discarded captures; the user felt the runs were about the same. At 30 seconds
the full-context run had 402 late flags and an estimated 20.77 ms delay. These are separate, scene-dependent runs,
but they indicate that full-context work may leave a lasting presentation backlog even when the FPS counter stays
stable; reducing validation frequency alone is not sufficient evidence that it is safe to enable by default.
An opt-in diagnostic catch-up experiment (`MOONSR_FG_CATCHUP_SLOTS=1.5`) omits a generated frame when its slot is
more than 1.5 slots late, preserving real frames. In a subsequent 45-second test with full-context checks, it
skipped one generated frame, discarded no captures and ended at about 8 ms estimated delay; the user noticed no
stutter. Delay temporarily reached about 20 ms before recovering. This single non-blind run is encouraging but
does not establish behavior under sustained overload, different multipliers, or other monitors.
At 30-to-90 FPS (3x) on the same 144 Hz display, a 45-second full-context diagnostic with catch-up enabled
recorded zero discarded captures, zero late generated frames and no recovery skips. The ending hold-back was
22.25 ms and the estimated capture-to-present delay was 23.22 ms; the user reported smooth motion and acceptable
responsiveness. Because the recovery rule never activated, this checks that it does not interfere in this run,
not that it can recover from sustained overload at 3x.
In a further 45-second diagnostic with two-second sampling, the full-context ATLYSS profile reached the evidence
threshold and rejected the 75% candidate: two of eight validation samples exceeded the severe-regression limit.
It then began testing the 90% candidate while keeping the requested 100% flow. A 10-second restart reloaded the
90% candidate's 11 training and four validation observations and continued collecting. The user reported smooth
motion without noticeable stutter during the 45-second run. This demonstrates a real-game reject-and-reload path,
not acceptance or application of a lower scale. Diagnostic profiles remain separate from normal profiles.
An accelerated sampling interval is available only for diagnostics via `MOONSR_FG_SAMPLE_INTERVAL_MS`.

Profiles contain aggregate measurements and sample identifiers, not captured images. They are stored under
`%LOCALAPPDATA%\MoonSR\profiles` and keyed by the game executable, selected GPU and driver, frame size, selected
flow scale, HUD protection, and relevant preceding pipeline settings. The reset button clears the active profile
for that combination, leaving other games and ALSR profiles alone. A missing, incompatible or corrupt profile is
ignored. If the executable cannot be identified or the profile directory is unavailable, the session uses its
own unsaved evidence.

Sampling uses GPU queries and nonblocking readback, while pixel scoring and profile I/O run on workers. Experiments
are deferred during recent capture disruptions. Under sustained lateness, full-context evaluations are suppressed
until headroom returns, and full-frame triplet collection is paused too. The learner does not reduce the requested
frame multiplier. In an isolated RTX A400 test, 1080p Performance-mode PushFrame plus Interpolate medians were
approximately 12.8 ms at 75% flow and 19.6 ms at 100%. The A400 passed the full-context and overload self-tests,
but these measurements are not a live-game pacing guarantee.

The status shows collected triplets, completed comparisons, and timing, motion, or busy skips. When the display
refresh limit reduces frame generation to 1x, learning is paused until generation resumes. Timing order alternates
between accepted comparisons so static-tile rejections cannot systematically bias the GPU measurements.

For diagnostics, `MOONSR_FG_DISPLAY_STATS=1` enables optional DXGI frame-statistics polling. It reports only samples
for which DXGI exposes consecutive present counts; it is not a complete measurement of physical scanout.
