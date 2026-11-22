# Windows capture and presentation mechanics for MoonSR Suite

Author: graphics-researcher. Date: 2026-11-13.

**Method note.** Web tools WERE available. Evidence level labels used throughout:

- **[V]** verified from a fetched source this session (URL in the source list).
- **[V-code]** verified by reading Magpie source (Blinue/Magpie, `dev` branch) via fetch; the fetch tool summarises code with a small model, so treat details as "read from source via a summariser", not byte-exact.
- **[I]** inferred from sources or from general platform knowledge, not directly stated by a primary source.
- **[M]** written from model memory, not re-verified this session.
- **[U]** unknown / not publicly documented.

Not fetched, therefore not verified: OBS capture code itself (only the OBS PR text), Lossless Scaling's own documentation (only community summaries), DirectX developer blog posts, DWM internals. Magpie files not read: `CursorDrawer.cpp`, `Renderer.cpp`, `WindowHelper`. The `GraphicsCaptureSession` property pages on Learn have very sparse remarks for the 26100 properties.

---

## 1. Capture

### 1.1 Windows.Graphics.Capture (WGC)

**Item creation.** `GraphicsCaptureItem` from an HWND via `IGraphicsCaptureItemInterop::CreateForWindow` (monitor: `CreateForMonitor`). Robmikh's Win32CaptureSample is the reference; minimum OS build 17134 (1803) [V]. The sample README states the compile requirement of the 26100 SDK; running needs only the OS [V]. `GraphicsCaptureSession.IsSupported()` should be checked first [V].

**Frame pool.** `Direct3D11CaptureFramePool::Create` vs `CreateFreeThreaded` (the latter added in 1809 / 17763) [V]:
- `Create`: needs a `DispatcherQueue` on the creating thread, and that thread must pump messages; `FrameArrived` is raised on that thread [V, Win32CaptureSample README and Learn].
- `CreateFreeThreaded`: no dispatcher dependency; `FrameArrived` raised on the pool's internal worker thread, so the handler must be agile/thread-safe [V].
- Magpie uses `Create` with 4 buffers and a no-op `FrameArrived` handler just to wake its thread, then drains with `TryGetNextFrame` and keeps only the newest frame, copying it with `CopySubresourceRegion` [V-code]. It notes more buffers help latency at low frame rates [V-code comment].
- Frames must be released promptly; do not keep the frame or its surface after check-in [V, Learn screen-capture article].
- Magpie also notes WGC is incompatible with `D3D11_CREATE_DEVICE_SINGLETHREADED` [V-code comment]. [I] Create the capture device without that flag (and do not rely on it for the processing device either if shared).

**Pixel formats.** `B8G8R8A8UIntNormalized` for SDR. Learn says that with Windows HD Color enabled the content may not be BGRA8 and recommends `R16G16B16A16Float` throughout (pool, destination) to avoid clipping HDR content; tone-mapping then needed for SDR output [V]. DuplicateOutput1 is the DXGI equivalent for HDR [V, HDR article].

**Buffer count.** "Any integer" per Learn [V]. Practical value: 2 minimum for overlap [I]; Magpie uses 4 [V-code]. Pool size does not by itself add latency if you always take the newest frame [I]; stale buffers only matter if you stop draining.

**Frame properties.** `Direct3D11CaptureFrame.ContentSize`, `.Surface`, `.SystemRelativeTime` (QPC-based, usable to sync with other media) [V]. The surface is always the size passed to Create/Recreate; content larger is clipped, smaller leaves undefined data, so crop by `ContentSize` [V]. Magpie does not use `SystemRelativeTime` [V-code].

**Resize.** When `ContentSize` differs from the pool size, call `Recreate` (can also change device, format, buffer count). Recreate discards all outstanding frames, so drain first [V]. Magpie's WGC source never recreates (it creates the pool once) because its scaling session ends when the source resizes [V-code].

**Cursor.** `IsCursorCaptureEnabled` was added in 2004 / 19041 [V]. Magpie sets it false when the property exists [V-code]. Set false so the software cursor is drawn by us only once.

**Border.** `IsBorderRequired`: documented as Windows 10 version 2104 (10.0.20348) [V]. Docs say that to disable the border the app must call `GraphicsCaptureAccess.RequestAccessAsync(GraphicsCaptureAccessKind.Borderless)` and declare the `graphicsCaptureWithoutBorder` capability in its package manifest; if denied, setting false "succeeds" but is ignored; and if any other app sets it true for the same item, the border shows [V]. Magpie sets it false "when the property exists (Win11 and later)", with a code comment that this avoids the permission prompt for Win32 apps [V-code comment]. [I] So unpackaged Win32 apps appear to be able to set it false without the consent flow on Windows 11; Learn does not state this. On Windows 10 (before 2104-equivalent availability on client) the yellow border [U: exact client build where the property is present; Learn's "20348" is a Server-flavoured build number and client mapping is not stated]. Use `ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")` rather than a build check.

**MinUpdateInterval.** Present only in Windows 11 24H2 (26100) and later [V: Learn moniker list shows winrt-26100+; OBS PR text says "introduced in Windows build 26100"]. Property type `TimeSpan`, Learn gives no remarks [V]. Behaviour from third-party reports:
- Default capture is capped around 60 fps by the compositor's default frame-delivery interval (OBS PR #12759 describes "hard cap of 60 fps" and fixes it by setting MinUpdateInterval) [V, PR text; code not shown].
- Win32CaptureSample issue #82 (a Lossless Scaling developer test): default value reads 0; values 0 to 999 microseconds gave about 50 fps; 1 ms (10000 ticks) gave full 144 fps; values above 1 ms behaved as an interval cap (25 ms ~ 40 fps, 17 ms ~ 60 fps). Issue remains open; maintainer could not reproduce [V, issue summary].
- Magpie sets 1 ms with a comment that 24H2 requires this to exceed 60 fps [V-code comment].
- Recommendation: set to 1 ms (never 0 and never below 1 ms) when the property exists. [I] On Windows 10 and Windows 11 before 24H2 the OBS PR implies capture is limited to 60 fps [V, PR text] but the exact limiting mechanism there is [U]. Whether WGC on those builds follows the monitor refresh (e.g. 144 Hz) or is flat 60 fps is not established by these sources. Test on target machines.

**DirtyRegionMode / IncludeSecondaryWindows.** Both added in 24H2 (26100) [V]. `IncludeSecondaryWindows` default false, can change during capture; secondary windows are WS_POPUP or WS_EX_TOOLWINDOW windows that intersect the main window with only other secondary windows between; drawn into the texture and clipped to the main window bounds [V]. `DirtyRegionMode` has no useful remarks on the property page; the enum `GraphicsCaptureDirtyRegionMode` and `Direct3D11CaptureFrame.DirtyRegions` exist [I, from the enum link; details not fetched]. [I] Dirty regions are useful only to skip work on static frames; for frame generation we want a timestamp per delivered frame instead.

**Client area vs whole window.** WGC window items capture the whole top-level window including title bar and (on some builds) invisible resize borders / shadow; the frame content box depends on OS version. Magpie, after logic keyed to `GetOSVersion`: on Win10 and Win11 24H2+ it treats the capture region as `DWMWA_EXTENDED_FRAME_BOUNDS`; on earlier Win11 maximised windows with non-client rendering disabled it uses the client rect or intersects with the monitor work area; it notes a 1-pixel DPI-related offset it does not correct [V-code]. Practical crop: compute the client rect in screen coords (`GetClientRect` + `ClientToScreen`), subtract the frame origin (`DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)`), then `CopySubresourceRegion`/sample a sub-rect; validate against `ContentSize` every frame [I].

**Windows WGC cannot capture.** Magpie temporarily adds `WS_EX_APPWINDOW` to windows that WGC refuses (probably windows hidden from the taskbar / tool windows) and restores it afterwards [V-code]. Magpie also handles "Kirikiri" windows specially [V-code].

**Exclusive fullscreen, occluded, minimized, protected content.**
- Occluded windows: [M] WGC captures the window's own composition surface, so occlusion by other windows does not appear in the capture. Not verified this session.
- Minimized windows: [M] no new frames are produced while minimised. Not verified.
- Exclusive fullscreen: [M/I] a true exclusive fullscreen swapchain bypasses DWM, and WGC window capture of it historically yields black/no frames. Modern Windows 10/11 (the "Fullscreen optimizations" model) converts legacy "fullscreen exclusive" to independent flip, which composition-based capture can see for display capture; behaviour of HWND capture for flip-model games is [U] from these sources. Learn's flip-model article says borderless flip model is "effectively equivalent or better" than exclusive, and the user-visible product behaviour (Lossless Scaling requires windowed/borderless) is consistent with that [I].
- Protected content (DRM video): [M] captured as black. Not verified.

**Latency of WGC.** No primary source quantifies it. [I] WGC takes the frame from DWM's composition of that window, so a captured frame is at least as old as the DWM composition pass in which the window's latest submitted frame was consumed; add one frame-pool copy. The Blur Busters borderless/DWM test is cited by a search summary as showing about 1 frame of added latency for DWM-composed windowed games vs exclusive, which supports "about one DWM interval" but is not a WGC measurement [I, secondary]. Number of DWM intervals added by WGC itself: [U]; must be measured with a photodiode or PresentMon + our own timestamps.

**Independent flip / MPO interaction.** [I] A game in independent flip bypasses DWM composition of that window; capture then needs DWM to still produce the window surface. Whether WGC forces the game out of independent flip, adds a copy, or captures the pre-flip buffer is not publicly documented [U]. This is a high-priority measurement: use PresentMon (Learn recommends it for seeing which flip mode is used) on the game with and without our capture session running [V that PresentMon shows the mode].

**Per-OS availability table (ApiInformation checks).**

| Feature | Minimum build | Check | Source |
|---|---|---|---|
| WGC base, Create | 17134 (1803) | `GraphicsCaptureSession.IsSupported()` | V |
| CreateFreeThreaded | 17763 (1809) | `ApiInformation::IsMethodPresent(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool", L"CreateFreeThreaded")` | V |
| IsCursorCaptureEnabled | 19041 (2004) | `IsPropertyPresent(... "GraphicsCaptureSession","IsCursorCaptureEnabled")` | V |
| IsBorderRequired | Learn: 20348 (2104); in practice Win11 for desktop apps | `IsPropertyPresent(..."IsBorderRequired")` | V (Learn), I (practice) |
| MinUpdateInterval | 26100 (24H2) | `IsPropertyPresent(..."MinUpdateInterval")` | V |
| DirtyRegionMode | 26100 | `IsPropertyPresent(..."DirtyRegionMode")` | V |
| IncludeSecondaryWindows | 26100 | `IsPropertyPresent(..."IncludeSecondaryWindows")` | V |
| WDA_EXCLUDEFROMCAPTURE | 19041 (2004) | build check; see 2.2 | V |
| EnumAdapterByGpuPreference | 17134 (1803) | factory QI `IDXGIFactory6` | V |

Magpie requires Win10 v1903+ / FL 11 [V, README]. Magpie's WGC source guards optional properties with `ApiInformation::IsPropertyPresent` [V-code].

### 1.2 DXGI Desktop Duplication

- API: `IDXGIOutput1::DuplicateOutput(pDevice)` (or `IDXGIOutput5::DuplicateOutput1` for format choice incl. HDR; Learn calls DuplicateOutput1 the better-performing option) [V].
- The `pDevice` "must be created from the adapter to which the output is connected"; otherwise `E_INVALIDARG` [V]. This is the key multi-GPU constraint: duplication runs on the adapter that drives the monitor.
- Whole output only; no window selection [V by API shape]. One duplication per output per process; by default only four processes may duplicate concurrently (`DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`) [V].
- `DXGI_ERROR_UNSUPPORTED` for 8bpp and non-DWM modes; recover on desktop switch (`EVENT_SYSTEM_DESKTOPSWITCH`) and `WM_DISPLAYCHANGE` [V]. `E_ACCESSDENIED` for the secure desktop unless running as LOCAL_SYSTEM [V]. `DXGI_ERROR_SESSION_DISCONNECTED` when session is disconnected [V].
- `AcquireNextFrame(timeout, &info, &resource)`; frame is `B8G8R8A8_UNORM` with `DuplicateOutput` regardless of desktop mode [V]. Release with `ReleaseFrame` before the next acquire [V, sample]. Timeouts return `DXGI_ERROR_WAIT_TIMEOUT`; `DXGI_ERROR_ACCESS_LOST` requires recreating the duplication [V, sample].
- Dirty rects and move rects: process move rects first, then dirty rects; the OS may coalesce regions when it overflows [V].
- Rotation: the acquired surface is always unrotated; the app must rotate [V].
- Pointer: the pointer may be separate from the image; use `PointerPosition`, `LastMouseUpdateTime`, `GetFramePointerShape` on shape changes [V]. For multi-output the sample compares update timestamps [V].
- Latency vs WGC: [U] no primary comparison. [I] Duplication reads the final composed (or directly scanned-out) desktop image, so it sees whatever is on screen, including when a game is in independent flip [M, not verified]. Magpie's capture comparison table lists Desktop Duplication as requiring Windows 10 2004, not supporting recording/streaming or multi-screen spanning, and WGC as "most recommended" [V, summary].
- Frame rate: [I] one new frame per display refresh at most (vblank-driven). Not a 60 fps cap by itself.
- Limitation for us: captures everything including our own output unless excluded (2.2), and cannot isolate the game window. Useful as fallback for exclusive-fullscreen-ish cases and for the "game on the display GPU" topology.

### 1.3 Other approaches (honest notes)

| Method | Status | Notes |
|---|---|---|
| `DwmGetDxSharedSurface` (user32, undocumented) | Undocumented, unsupported | Magpie loads it dynamically each frame and opens the shared handle with `OpenSharedResource` then copies a rect; fails if the window has no redirection surface (e.g. DirectComposition apps); fixed BGRA8 [V-code]. Magpie's table marks DwmSharedSurface and GDI as "not recommended due to unstable performance" (one of the two; mapping unclear) [V, summary]. Not publicly documented behaviour; may change between builds. Legality: calling an undocumented export is not illegal, but no support or stability guarantees [I]. |
| GDI `BitBlt` / `PrintWindow(PW_RENDERFULLCONTENT)` | Documented, slow | [M] CPU round trip, cannot capture many flip-model/DX content reliably; OBS shows BitBlt can return stale frames (search summary of an OBS issue). Fallback only. |
| Hook `Present` (OBS Game Capture, Special K style) | Works at lowest latency [M] but injects a DLL | We will not inject by default. [I] Injection into a game process is the pattern anti-cheat (EAC, BattlEye, Vanguard, etc.) detects and may ban; vendor-specific policy is not something we verified. Magpie's FAQ says it is non-intrusive and there are no ban reports but warns users to understand the risk [V, FAQ summary]; this applies to Magpie's non-injecting design. OBS's own Game Capture is an injected hook ([V, search summary of OBS discussion]). Keep as an opt-in advanced mode if ever. |

---

## 2. Presentation

### 2.1 Overlay window styles

Magpie's scaling window (the closest working reference) [V-code]:
- Fullscreen: `WS_POPUP` (+ `WS_MAXIMIZE` on single monitor); extended styles `WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP`.
- It handles `WM_MOUSEACTIVATE` -> `MA_NOACTIVATE`.
- It toggles `WS_EX_TRANSPARENT` at runtime (via `SetWindowLong(GWL_EXSTYLE)`) so mouse input falls through to windows underneath only while the cursor is captured over the source [V-code, CursorManager].
- Magpie code comments: a layered window that is "not fully transparent" does not disable DirectFlip, so it sets layered opaque; and placing the window above all topmost windows in fullscreen helps GPUs without MPO get DirectFlip [V-code comments].
- Magpie's adaptive presenter uses `CreateSwapChainForHwnd` flip-discard when "DirectFlip" is enabled, and falls back to a pure DirectComposition path when disabled [V-code]. FAQ: DirectFlip can cause stuttering/low frame rate/frozen video in some games, so users get an option to disable it [V, FAQ summary].

Which flag combinations preserve independent flip / MPO:
- Learn defines DirectFlip as: swapchain buffers match the screen and the window client region covers the screen; with panel fitters, buffers within a hardware scale range; with MPO, a dedicated plane for the window. Once DirectFlipped, DWM sleeps (Independent Flip), and if other content comes on top DWM can reverse-compose or use MPO [V]. Learn also says flip model with the `FLIP_DISCARD` effect enables "reverse composition" [V].
- `WS_EX_LAYERED` + opaque: per Magpie's comment, does not break DirectFlip [V-code comment, not Microsoft docs]. A layered window with `SetLayeredWindowAttributes` alpha < 255 or `UpdateLayeredWindow` per-pixel alpha forces composition [I].
- `WS_EX_TRANSPARENT` (mouse click-through only, not visual transparency): [I] a hit-test flag, no stated impact on flip; but Magpie only enables it dynamically, not statically, on the top-level window. Unverified whether toggling mid-stream affects direct flip [U].
- `WS_EX_NOREDIRECTIONBITMAP`: required when presenting via DirectComposition and used with HWND swapchains by Magpie [V-code]. Avoids the GDI redirection surface. Not documented as affecting flip eligibility [U].
- `WS_EX_NOACTIVATE`: prevents activation; no flip impact expected [I].
- `WS_EX_TOOLWINDOW`: keeps it off the taskbar/Alt-Tab; no flip impact expected [I]; note WGC treats tool windows as "secondary windows" candidates [V].
- `WS_EX_TOPMOST`: needed to be above the game; Magpie notes being above all topmost windows helps non-MPO GPUs [V-code comment].
- Anything that makes DWM draw another visual over our window (the game window beneath is irrelevant, but other topmost windows e.g. notifications, Xbox Game Bar) forces reverse composition or MPO [V per Learn].
- `CreateSwapChainForComposition` + DirectComposition: Learn says flip swapchains are eligible for DirectFlip, but the exact conditions for a composition swapchain attached to a visual to get independent flip [U]; Magpie keeps it for resize transitions and for the presentation-manager path [V-code]. Magpie's `CompSwapchainPresenter` uses `IPresentationManager` (dcomp presentation API, Windows 11 only: it rejects Win10) with `SHARED_DISPLAYABLE` buffers, and tolerates `IsPresentationSupported()` reporting no independent flip [V-code]. Recommend `CreateSwapChainForHwnd` as the primary path [I].

### 2.2 Preventing recursive capture

- `SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)` (0x11): window shown only on the monitor, "everywhere else the window does not appear at all". Introduced in Windows 10 version 2004; on earlier builds it behaves as `WDA_MONITOR` (window appears as black in captures) [V]. Window must belong to the calling process and be top level [V]. Works only when DWM is composing the desktop, and is explicitly not a security/DRM guarantee [V].
- Interaction with WGC and Desktop Duplication: Learn says it covers capture "through a specific set of public operating system features and APIs" [V, wording], without enumerating them. [M/I] It is honoured by WGC and DXGI Desktop Duplication on 2004+. Because we capture the game HWND via WGC, our overlay is not in the frame anyway; this only matters for the Desktop Duplication fallback and for third-party capture of the screen (OBS display capture) where exclusion would hide our output too, so make it a user option [I].
- Magpie does not call `SetWindowDisplayAffinity` in `ScalingWindow.cpp` [V-code]. It has a capture-exclusion workflow tied to overlays (RTSS double-overlay note) [V, FAQ].
- Note: Some DWM paths treat a window with WDA_EXCLUDEFROMCAPTURE as non-eligible for MPO? [U]

### 2.3 DXGI flip model

Learn facts [V]:
- Buffer count at least 2; re-bind RTV after Present; call `ResizeBuffers` after `SetFullscreenState`; no MSAA in flip swapchains. `FLIP_DISCARD` or `FLIP_SEQUENTIAL` only. Supported formats: `R16G16B16A16_FLOAT`, `B8G8R8A8_UNORM`, `R8G8B8A8_UNORM` (plus `R10G10B10A2_UNORM` per the best-performance article).
- One flip swapchain per HWND; do not mix GDI or other APIs on the same HWND.
- Tearing: `IDXGIFactory5::CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING)`; create with `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`; present with `DXGI_PRESENT_ALLOW_TEARING` only with sync interval 0; `ResizeBuffers` must use the same flag; recommended to always pass the flag at interval 0 when tearing is supported and the app is windowed or borderless. Needs Win10 Anniversary Update or KB3156421. Disabling vsync does not guarantee uncapped rate if something else throttles Present.
- `FRAME_LATENCY_WAITABLE_OBJECT`: can reach 1 frame of latency in Independent Flip, with fallback when composed; a bug gave 2 frames minimum on the Anniversary Update and earlier, fixed in Fall Creators Update (1709).
- `ALLOW_TEARING` "can enable even lower latency than the waitable object, even in a window on systems with MPO support".
- `GetFrameStatistics` works for flip model in windowed mode; returns zeros for blt-windowed; returns `DXGI_ERROR_FRAME_STATISTICS_DISJOINT` on first call and mode changes; "not reliable in many multiple monitor scenarios"; may need `DwmFlush` before stats update on Hardware Flip Queue systems [V].
- `DwmGetCompositionTimingInfo(NULL, &info)`: since 8.1 the hwnd must be NULL [V]. Fields (`qpcVBlank`, `rateRefresh`, `cRefresh` ...) not fetched in detail [M].
- `IDXGIOutput6::CheckHardwareCompositionSupport` reports whether panel fitters / MPO scaling are available and whether the cursor would be stretched (`DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_CURSOR_STRETCHED`) [V].

Magpie swapchain specifics (`AdaptivePresenter`) [V-code]: `CreateSwapChainForHwnd`, `FLIP_DISCARD`, flags `ALLOW_TEARING` (if supported) and `FRAME_LATENCY_WAITABLE_OBJECT`, format `R8G8B8A8_UNORM`, alpha ignored, scaling `STRETCH` in release, `SetMaximumFrameLatency(bufferCount - 1)`, `MakeWindowAssociation(DXGI_MWA_NO_ALT_ENTER)`, wait on the waitable object (1000 ms timeout) per frame, `Present(0, 0)` (note: no `DXGI_PRESENT_ALLOW_TEARING` flag passed, which looks like a missed opportunity or a deliberate choice; Learn says the flag should be passed with interval 0 [V]). Buffer count 4 in "3D game mode" else 8, tied to its cursor-check cadence [V-code]. `DiscardView` after present. GPU fence wait before resize and `WaitForDwmComposition` helper to reduce edge flicker [V-code].

VRR behaviour for windowed flip: [U] Learn does not state when G-SYNC/FreeSync engages for a borderless flip window; the Learn VRR page only describes the tearing flags. [I] VRR in windowed mode requires the driver's "windowed mode" support and a tearing present with independent flip / MPO. Test per GPU vendor.

### 2.4 Pacing

- Default: waitable object + `SetMaximumFrameLatency(1)` or 2 + `Present(0, ALLOW_TEARING)` for lowest latency; `Present(1, 0)` for vsync-locked [I, built on V facts above].
- For frame generation the output cadence should be locked to the display refresh: derive refresh from `DXGI_OUTPUT_DESC1`/`QueryDisplayConfig` (`DISPLAYCONFIG_PATH_TARGET_INFO.refreshRate` rational) [M]; use `DwmGetCompositionTimingInfo(NULL)` `qpcVBlank` to phase-lock the render thread [M]; `DwmFlush()` blocks until the next DWM composition and is the cheap vblank-align wait [M].
- High-resolution waitable timer: `CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)` (Windows 10 1803+) gives about 0.5 ms granularity without `timeBeginPeriod` [M, not fetched]. Use for sleeping until a target present time minus a small spin.
- Magpie's pacing relies on the waitable object plus `Present(0,0)` and drops older queued frames [V-code].

### 2.5 Cursor handling

How Magpie does it (`CursorManager.cpp`, read via summariser) [V-code]:
1. **Clip** the real cursor to the source window region with `ClipCursor` (restore only if nobody else changed it via `GetClipCursor`).
2. **Hide the system cursor**: dynamically loads `ShowSystemCursor` from user32 (undocumented export in newer Windows), falls back to `MagInitialize` + `MagShowSystemCursor(FALSE)` from the Magnification API. `SetSystemCursor` is NOT used.
3. **Draw a software cursor** each frame using `GetCursorInfo` (HCURSOR, position, visible), rendered by `CursorDrawer` (monochrome and masked cursor pixel shaders exist) [V-code, shader file names].
4. **Map coordinates**: `SrcToScaling`/`ScalingToSrc` map positions linearly between the source window rect and the scaled output.
5. **Adjust cursor speed** with `SPI_SETMOUSESPEED` scaled by the zoom factor (checks `SPI_GETMOUSE` for acceleration; with acceleration it scales linearly, without it picks the nearest value from a sensitivity table), restored on exit.
6. **Positioning**: `SetCursorPos` considered unreliable; it uses a 1x1 `ClipCursor`, sleeps 8 ms, and posts `WM_SETCURSOR` to refresh the shape.
7. **Click-through**: toggles `WS_EX_TRANSPARENT` and hit-tests windows under the cursor (`WindowFromPoint`-style with `DWMWA_CLOAKED` filtering).
8. FAQ: a "3D Game Mode" changes cursor behaviour for 3D games (games that recentre the cursor or use raw input) [V, FAQ].

Options for MoonSR [I, built on the above plus general API knowledge]:
- (A) Virtual cursor via raw input: register for `WM_INPUT` and draw our own cursor, `ClipCursor` to a small rect or the game window, deliver the game's input unchanged. Works for FPS/raw-input games since they ignore the cursor; for windowed UI games it breaks mapping.
- (B) Scale mouse speed with `SPI_SETMOUSESPEED` so the on-screen real cursor motion matches the scaled image (Magpie approach). Imperfect with acceleration. Restore on crash handlers (a persistent system change; guard with an atexit/crash recovery path).
- (C) Region-warp: keep the real cursor mapped 1:1 to the game window rect and place the overlay window so the cursor appears over the right pixels (only works when output is not rescaled; not applicable to upscaling to a larger area).
- Always draw the software cursor into the presented frame (not the captured frame) so it is latency-free and not upscaled; keep `IsCursorCaptureEnabled=false`.
- Hiding: prefer `MagShowSystemCursor` (documented Magnification API, works on Win8+ [M]) over the undocumented `ShowSystemCursor`; both are what Magpie does in sequence [V-code]. Magnification API note: it requires `MagInitialize` and must be uninitialised on exit [M].
- Lossless Scaling's cursor method: [U] closed source; its user-visible behaviour (cursor scaling option) is not documented in fetched sources.

### 2.6 HDR and colour

All [V] from the Learn Advanced Color article unless marked:
- Flip model required. Option 1 (recommended general): `DXGI_FORMAT_R16G16B16A16_FLOAT`, default colour space `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709` (scRGB, DWM canonical space). Option 2 (HDR display, D3D11/12, no alpha blending): `R10G10B10A2_UNORM` + `IDXGISwapChain3::SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)` (HDR10).
- scRGB 1.0 = 80 nits on HDR displays; SDR UI must be scaled by `SdrWhiteLevelInNits/80`, from `DISPLAYCONFIG_SDR_WHITE_LEVEL` via `QueryDisplayConfig` in desktop apps.
- Detect HDR with `IDXGIOutput6::GetDesc1().ColorSpace` (`G2084_NONE_P2020` = HDR); it cannot distinguish SDR Advanced Color. Re-query when `IDXGIFactory1::IsCurrent()` is false; do not use `GetContainingOutput` (stale) - pick the output with the largest intersection with the window.
- Capturing HDR: WGC with `R16G16B16A16Float` pool; DXGI `DuplicateOutput1` for duplication. An FP16 swapchain presented to an SDR display is numerically clipped by DWM.
- Plan: processing in FP16 linear when HDR is detected; keep 8-bit path for SDR.

### 2.7 Monitors, DPI, refresh

- Process: per-monitor DPI awareness v2 via manifest (`<dpiAwareness>PerMonitorV2</dpiAwareness>`, Windows 10 1703+) [M]. Without it, WGC/Duplication sizes may be virtualised; Magpie's table says Graphics Capture and Desktop Duplication "do not ignore DPI virtualization" [V, summary].
- Enumerate: DXGI `EnumOutputs` + `GetDesc1` for rect and colour; `EnumDisplayMonitors`/`GetMonitorInfo` for work area [V-code uses the latter].
- Refresh: `QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS)` -> `targetInfo.refreshRate` (Numerator/Denominator, exact rational) [M]; `EnumDisplaySettings` gives integer Hz only [M]. `DXGI_OUTPUT_DESC1` has no refresh rate [M].
- `WM_DISPLAYCHANGE`/`EVENT_SYSTEM_DESKTOPSWITCH` should trigger recreate of duplication, swapchain and refresh re-query [V for Duplication].
- Hardware composition support per output: `IDXGIOutput6::CheckHardwareCompositionSupport` [V].

---

## 3. Multi-GPU

**Adapter selection.** `IDXGIFactory6::EnumAdapterByGpuPreference` (Win10 1803+). `HIGH_PERFORMANCE` order: xGPU, dGPU, iGPU; `MINIMUM_POWER`: iGPU, dGPU, xGPU; `UNSPECIFIED` = `EnumAdapters1` [V]. Use `EnumAdapterByLuid` to find the adapter that owns a given LUID [V, DXGI 1.4 page]. Identify the game's adapter via the window's monitor/output ownership or per-app GPU preference [I]; the game's render adapter is not directly queryable from the HWND [U].

**Which adapter renders vs displays.** Learn's flip-model article: content may be migrated between GPUs as part of present or composition (hybrid laptops, eGPU); it advises lower resolution swapchains for non-primary windows to reduce cross-GPU bandwidth [V]. Therefore a swapchain created on adapter B whose monitor is attached to adapter A will have its buffers copied by the OS [V/I]; to avoid that, create the swapchain on the adapter that owns the output. This matches LS community guidance to connect the monitor to the secondary (LSFG) GPU [V, secondary: community guides via search, not official].

**Duplication constraint.** Desktop Duplication device must be on the output's adapter [V].

**WGC with a device on a different adapter than the game.** [U] No primary doc says who pays for the copy. [I] The capture surface is allocated on the device given to the frame pool, so DWM/driver must copy from the window's composition surface (on the adapter that composes) to the frame pool texture (on our adapter); the cost is a PCIe copy per frame performed by the OS/driver, not by us. Measure.

**Cross-adapter copy options in D3D11.**
1. Shared NT handle on the same adapter: works only on the same adapter [M; consistent with "OpenSharedResource" usage in Magpie on the same device, V-code].
2. `D3D11_RESOURCE_MISC_SHARED` textures across adapters: [M] generally unsupported across physical adapters except via specific sharing (shared-displayable buffers, as Magpie's presentation path uses `SHARED_DISPLAYABLE`) [V-code]. Not documented for general cross-adapter compute.
3. D3D12 cross-adapter: `D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER` heap, resources need `D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER`; cannot mix with protected sessions; texture layout is row-major only (linear) [V for flag text, M for layout]. Requires D3D12 `CrossNodeSharingTier`/`CrossAdapterRowMajorTextureSupported` capability checks [M]. Since D3D12 is out of scope of the D3D11 baseline, this is a later extension.
4. CPU readback fallback: staging texture + `Map` on source device, upload on destination, with pipelining via 2-3 staging textures. [I] Works everywhere; costs about 1 copy GPU->sysmem and 1 sysmem->GPU; 4K RGBA8 is about 33 MB per frame, so about 4 GB/s at 120 fps in each direction; feasible on PCIe 3.0 x8+, latency adds at least one frame.
5. Let the OS do it: render to a swapchain on adapter B, display on adapter A; DWM/driver performs the migration [V per Learn]. Simplest, no guarantees about cost.

**What Lossless Scaling's dual-GPU instructions imply.** Community summaries of LS's official dual-GPU guide [V that these summaries say this; secondary]: render GPU runs the game; set it as Windows default high-performance GPU; in LS set Preferred GPU to the secondary; the secondary GPU renders generated frames and outputs to the display; monitor plugged into the secondary GPU. [I] Implies LS captures with WGC (it is the developer of the Win32CaptureSample issue test, which tested capture of LS itself [V]) and has the OS copy the game window across GPUs, then does all processing and presentation on the secondary GPU. No custom cross-adapter sharing is implied; they rely on Windows' copy. [U] Whether LS uses additional private sharing. Treat as inference.

---

## Recommended implementation

**Capture (primary): WGC, window item.**
- `IGraphicsCaptureItemInterop::CreateForWindow(hwnd)`.
- `Direct3D11CaptureFramePool::CreateFreeThreaded(device, B8G8R8A8UIntNormalized (SDR) or R16G16B16A16Float (HDR, detected), 4 buffers, item.Size())`. Drain with `TryGetNextFrame` in the handler; signal a latest-frame slot; copy out only the client sub-rect via `CopySubresourceRegion` and release the frame immediately.
- Session settings guarded by `ApiInformation::IsPropertyPresent`: `IsCursorCaptureEnabled=false`; `IsBorderRequired=false`; `MinUpdateInterval=1 ms` (never 0, never <1 ms); optional `IncludeSecondaryWindows=true` when the game uses popup child windows.
- On `ContentSize` change: drain, `Recreate`, recompute crop. Re-evaluate crop using `DWMWA_EXTENDED_FRAME_BOUNDS` and client rect.
- Device: create without `D3D11_CREATE_DEVICE_SINGLETHREADED`; on the processing/display adapter. If WGC fails or reports black, fall back (in order) to Desktop Duplication on the output's adapter, then optional DwmSharedSurface (flagged experimental), never injection by default.
- Add an instrumented probe mode logging `SystemRelativeTime`, arrival time and QPC to measure delivery jitter and rate on each OS.

**Overlay window.**
- `WS_POPUP`, ex-styles `WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED` (layered kept fully opaque, so per Magpie comment it does not break DirectFlip) and `WS_EX_TRANSPARENT` set for click-through (toggle at runtime if hit-testing for UI is needed). Handle `WM_MOUSEACTIVATE` -> `MA_NOACTIVATE`. Window sized to the monitor rect. `ShowWindow(SW_SHOWNOACTIVATE)`; `SetWindowPos(HWND_TOPMOST, ... SWP_NOACTIVATE)`.
- `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` only on build >= 19041 and only when the user selects display-capture mode.
- Validate each style combination with PresentMon (presentation mode column: Hardware Composed: Independent Flip or Direct Flip vs Composed: Flip) [V that PresentMon exposes the mode]. Include a toggle to drop `WS_EX_LAYERED` and test.

**Swapchain.**
- `CreateSwapChainForHwnd`, `DXGI_SWAP_EFFECT_FLIP_DISCARD`, buffers 3 (2 minimum), `SampleDesc {1,0}`, format `B8G8R8A8_UNORM` (SDR), `R16G16B16A16_FLOAT` + scRGB (HDR option 1) or `R10G10B10A2_UNORM` + `SetColorSpace1(G2084_NONE_P2020)` (HDR option 2), `DXGI_SCALING_STRETCH`, flags `FRAME_LATENCY_WAITABLE_OBJECT` and (when `CheckFeatureSupport` is true) `ALLOW_TEARING`. Same flags in `ResizeBuffers`. `MakeWindowAssociation(DXGI_MWA_NO_ALT_ENTER)`.
- Swapchain on the adapter that drives the target monitor. Match buffer size to the monitor native resolution to qualify for DirectFlip.

**Pacing.**
- `SetMaximumFrameLatency(1)` (2 if GPU-bound jitter appears); wait on the waitable handle before acquiring the back buffer; `Present(0, DXGI_PRESENT_ALLOW_TEARING)` for lowest latency/VRR, or `Present(1, 0)` as a vsync option. Frame generation: derive output cadence from the exact refresh (`QueryDisplayConfig`), phase from `DwmGetCompositionTimingInfo(NULL)`/`GetFrameStatistics`, sleep with a `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` timer then spin the last 100-200 microseconds. Drop stale captured frames: use the newest, as Magpie does.

**Cursor.**
- `IsCursorCaptureEnabled=false`; `ClipCursor` to the source window, hide system cursor with `MagShowSystemCursor(FALSE)` (init/uninit symmetrical), draw a software cursor from `GetCursorInfo`/`CopyIcon`/`GetIconInfo` into the presented frame at the mapped position; optional `SPI_SETMOUSESPEED` scaling with crash-safe restore; special "game cursor locked" mode for raw-input games that hide the cursor (follow Magpie's 3D game mode idea). Always restore clip, speed and cursor visibility on exit and on unhandled exceptions.

**Multi-GPU.**
- Default: process and present on the GPU that drives the monitor (`EnumAdapterByGpuPreference` + match output via `EnumOutputs`), WGC device on the same adapter, and let Windows copy the game window across. CPU-readback staging path as fallback if the OS copy proves too slow; D3D12 cross-adapter heaps only as a later experiment.

---

## Unverified / unknown

1. Added latency of WGC in DWM intervals, and whether WGC capture of a flip-model game forces it out of independent flip or MPO. Needs measurement (PresentMon + photodiode).
2. Which Windows 10/11 builds cap WGC at 60 fps without MinUpdateInterval (OBS PR says Windows 10 is limited to 60; Windows 11 pre-24H2 not stated) and whether the cap follows monitor refresh.
3. `IsBorderRequired` on unpackaged desktop apps: Learn requires consent + capability; Magpie code comment says Win32 apps avoid the prompt on Win11. Client build number for availability is unclear (Learn: 20348).
4. WGC behaviour with exclusive fullscreen, minimized windows, occluded windows, protected content: not verified here.
5. Whether `WS_EX_TRANSPARENT`, `WS_EX_NOREDIRECTIONBITMAP`, `WS_EX_TOOLWINDOW` change flip/MPO eligibility; only Magpie code comments about opaque layered windows and topmost ordering are available.
6. Whether `WDA_EXCLUDEFROMCAPTURE` windows remain eligible for MPO / independent flip.
7. VRR engagement conditions for windowed flip across NVIDIA/AMD/Intel.
8. Who pays the cross-adapter copy in WGC when device != game adapter; and whether the game's adapter can be queried from an HWND.
9. Lossless Scaling internals (capture API, cursor method, dual-GPU transport): closed source; only community summaries seen.
10. D3D11 cross-adapter texture sharing support matrix (vendor/driver dependent).
11. `DirtyRegionMode` semantics and benefit.
12. Exact `DwmFlush`/`DwmGetCompositionTimingInfo` pacing behaviour under independent flip (DWM may sleep, so DWM timing may stop updating) [I, not verified].
13. Anti-cheat policy toward overlay windows and WGC, per title.
14. Magpie `Renderer.cpp`, `CursorDrawer.cpp`, OBS source and DirectX developer blog posts were not read.

---

## Sources

Microsoft Learn:
- GraphicsCaptureSession: https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession
- IsBorderRequired: https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.isborderrequired
- MinUpdateInterval: https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.minupdateinterval
- DirtyRegionMode: https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.dirtyregionmode
- IncludeSecondaryWindows: https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.includesecondarywindows
- Direct3D11CaptureFramePool: https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframepool
- Screen capture article: https://learn.microsoft.com/en-us/windows/uwp/audio-video-camera/screen-capture
- Desktop Duplication API: https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api
- DuplicateOutput: https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutput1-duplicateoutput
- For best performance, use DXGI flip model: https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model
- DXGI flip model: https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-flip-model
- Variable refresh rate displays: https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/variable-refresh-rate-displays
- DXGI_FEATURE: https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_5/ne-dxgi1_5-dxgi_feature
- DXGI 1.4 improvements: https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-1-4-improvements
- IDXGISwapChain::GetFrameStatistics: https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-getframestatistics
- EnumAdapterByGpuPreference: https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_6/nf-dxgi1_6-idxgifactory6-enumadapterbygpupreference
- SetWindowDisplayAffinity: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowdisplayaffinity
- DwmGetCompositionTimingInfo: https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmgetcompositiontiminginfo
- D3D12_HEAP_FLAGS: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_heap_flags
- Advanced Color / HDR: https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range

Source code and community:
- Win32CaptureSample README: https://raw.githubusercontent.com/robmikh/Win32CaptureSample/main/README.md
- Win32CaptureSample issue 82 (MinUpdateInterval): https://github.com/robmikh/Win32CaptureSample/issues/82
- OBS PR 12759 (WGC above 60 fps): https://github.com/obsproject/obs-studio/pull/12759
- Magpie repo: https://github.com/Blinue/Magpie
- Magpie FAQ: https://github.com/Blinue/Magpie/wiki/FAQ
- Magpie capture comparison: https://github.com/Blinue/Magpie/wiki/Comparison-of-capture-methods
- Magpie CursorManager.cpp: https://raw.githubusercontent.com/Blinue/Magpie/dev/src/Magpie.Core/CursorManager.cpp
- Magpie ScalingWindow.cpp: https://raw.githubusercontent.com/Blinue/Magpie/dev/src/Magpie.Core/ScalingWindow.cpp
- Magpie DeviceResources.cpp: https://raw.githubusercontent.com/Blinue/Magpie/dev/src/Magpie.Core/DeviceResources.cpp
- Magpie GraphicsCaptureFrameSource.cpp: https://raw.githubusercontent.com/Blinue/Magpie/dev/src/Magpie.Core/GraphicsCaptureFrameSource.cpp
- Magpie DwmSharedSurfaceFrameSource.cpp: https://raw.githubusercontent.com/Blinue/Magpie/dev/src/Magpie.Core/DwmSharedSurfaceFrameSource.cpp
- Magpie AdaptivePresenter.cpp: https://raw.githubusercontent.com/Blinue/Magpie/dev/src/Magpie.Core/AdaptivePresenter.cpp
- Magpie CompSwapchainPresenter.cpp: https://raw.githubusercontent.com/Blinue/Magpie/dev/src/Magpie.Core/CompSwapchainPresenter.cpp
- Magpie PresenterBase.cpp: https://raw.githubusercontent.com/Blinue/Magpie/dev/src/Magpie.Core/PresenterBase.cpp
- Lossless Scaling dual-GPU community guide mirror: https://sageinfinity.github.io/docs/Guides/DualGPUGuide
- Dual-GPU write-up: https://benhaslett.substack.com/p/dual-gpu-frame-generation-with-lossless
- OBS discussion on capture methods: https://github.com/obsproject/obs-studio/discussions/11486
- Blur Busters G-SYNC 101 borderless vs fullscreen: https://static.blurbusters.com/gsync/gsync101-input-lag-tests-and-settings/10
- PresentMon: https://github.com/GameTechDev/PresentMon
