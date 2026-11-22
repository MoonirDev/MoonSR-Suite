# Adaptive upscaling

> Removed 2026-11-22 (owner decision): per-game learning (`alsr_learn`, learned filters, per-game profiles) is deleted
> together with its UI, settings keys and CLI flags. This file describes the removed feature and is kept for history
> only. Old profile folders under `%LOCALAPPDATA%\MoonSR\profiles` are left on disk and ignored.

Moon ALSR reconstructs frames using motion and temporal history. Its optional learner collects tiles from original game frames, trains filter buckets on a background worker and saves them per game. Selecting **Apply learned filters (experimental)** applies eligible buckets; it defaults off because the proxy validation cannot guarantee improvement on actual game images. Compare the result with the switch off. Learning can continue with application off.

Learning operates only for scale ratios above 1.01 and at most 1.75 on both axes. Outside that range, temporal reconstruction still runs but learned filters are not collected or applied. Proxy validation errors measure reconstruction of reduced training tiles, not ground-truth game quality.

Adaptive frame generation and ALSR share a profile folder under `%LOCALAPPDATA%\MoonSR\profiles\game-<identity>`, keyed by the captured executable's full path. The algorithms store independent data inside that folder: `upscaling.alsr` and one or more `fg-*.profile` files. FG evidence remains separated by GPU, driver, resolution, processing pipeline and settings. Resetting one learner leaves the other learner's files intact. If the executable path cannot be resolved, both learners run without persistence.

On first use, existing FG evidence is validated and migrated to the new folder. The old ALSR file was identified only by executable filename and is moved into the first matching game's folder if no new profile already exists. Thus, an old file shared by multiple games with identical executable names cannot be reliably attributed; no existing new profile is overwritten.
