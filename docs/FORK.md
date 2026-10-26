# Fork ledger

Every behavioural difference between `intel` and upstream `master`, so upstream merges can be
resolved with intent rather than guesswork. Update it in the same PR as the change.

## Upstream base

| Field | Value |
|---|---|
| Last upstream commit merged into `intel` | `ac296864` (2026-10-24) |
| Fork created | 2026-10-25 |

**Licences.** The fork is GPL-3.0, inherited from upstream. `app/fork/shaders/` vendors MIT code
(FSR 1 by AMD and NIS by NVIDIA, both ported to mpv by agyild) and LGPL-3.0-or-later code
(RAVU by bjin). Both licences allow combining into a GPL-3.0 work. Each file keeps its notice
and has a dated "Modified by moonlight-intel" line; `app/fork/shaders/README.md` records the
source, revision, SHA-256 before and after, licence and edits of each file.

## Deltas

| ID | Plan | Area | Files | What and why |
|---|---|---|---|---|
| — | — | tooling | `CLAUDE.md`, `docs/`, `.claude/settings.json`, `.gitignore` | Fork workflow scaffolding; no runtime change |
| — | 0001 | docs | `CLAUDE.md`, `docs/arch.md` | Target desktop is GNOME; Arch packages, build and diagnostics guide. No runtime change |
| D1 | 0001 | video / libplacebo | `plvk.cpp`, `plvk.h` | Video frames render with member `m_RenderParams` (a copy of `pl_render_fast_params`, same defaults) so later plans tune one place. Logs `moonlight-intel: frame …` (swapchain + source size and colour space) on the first frame and on change. GPU timing: see D3. Merge note: in `renderFrame()` keep `&m_RenderParams` in the `pl_render_image()` call and the hook lines around it (`logFrameInfo()`, `recordRenderStats()`); the rest is appended at the end of the file / class |
| — | 0002 | docs | `CLAUDE.md`, `docs/arch.md` | `gputop` replaces `intel_gpu_top` (deprecated on the `xe` driver); notes on fork settings and desktop HDR. No runtime change |
| D2 | 0002 | settings / fork module | `app/fork/fork.*`, `app/fork/intelpreferences.*`, `app/gui/IntelSettingsGroup.qml`, `app.pro`, `main.cpp`, `qml.qrc`, `SettingsView.qml` | Fork code lives in `app/fork/`; upstream files call only `fork/fork.h`. Settings → `moonlight-intel` group (last in the right column) holds every fork option, via the `IntelPreferences` QML singleton (`IntelPreferences 1.0`), stored in `[moonlight-intel]` of the shared `Moonlight.conf` and written through on change. Stock Moonlight ignores the group. Merge notes: `app.pro`: one marked block after the main `HEADERS` list (fork sources, plus `QT += dbus` and the GNOME backend under `linux:qtHaveModule(dbus)`); `main.cpp`: include + `Fork::initialize()` right after the `StreamingPreferences` singleton registration; `qml.qrc`: `gui/IntelSettingsGroup.qml`; `SettingsView.qml`: `IntelSettingsGroup {}` as the last item of `settingsColumn2` |
| D3 | 0002 | video / stats overlay | `plvk.cpp`, `plvk.h`, `app/fork/renderstats.*`, `ffmpeg.cpp` | The libplacebo timing callback is always installed. Video GPU time excludes the OSD `"overlay"` passes, so toggling the overlay doesn't change it; scaler passes are summed separately. About once a second plvk publishes GPU avg/p99/max, source → target size, scaler, swapchain size and colour space to `RenderStats`; the snapshot is cleared when the renderer is destroyed. The stats overlay gets `GPU render (video)`, `Scaling` and `Output` lines (only whole lines are appended). The 5 s `gpu-time(video)` log and the one-time `gpu-pass[i]` list (overlay passes marked) run when "Log GPU render timing" is on or `MOONLIGHT_INTEL_GPU_TIMING=1`. Merge notes: `ffmpeg.cpp`: include + `Fork::appendOverlayStats(…)` as the last statement of `stringifyVideoStats()`; plvk: constructor block and `recordRenderStats(&mappedFrame, &targetFrame)` after `pl_render_image()`, the rest appended at the end of the file / class |
| D4 | 0002 | desktop HDR | `app/fork/desktophdr*`, `app/fork/gnomedisplayconfig.*`, `session.cpp` | On GNOME, while the host sends HDR (optionally only while the stream is fullscreen and focused), the panel hosting the stream switches to BT.2100 and back to its previous colour mode afterwards, through Mutter's DisplayConfig D-Bus API with the **temporary** method (no `monitors.xml` write, no confirmation dialog). A panel already in HDR is never touched. Switches wait while GNOME's overview is open (`OverviewActive` on `org.gnome.Shell`), since a modeset there closes it and refocuses the stream. A marker in `$XDG_RUNTIME_DIR/moonlight-intel/` lets the next launch undo a switch after a crash. If the swapchain is still SDR 2 s after a switch, the renderer is recreated once. All D-Bus calls run on a worker thread. Backends are behind `DesktopHdrBackend`, so KDE/Windows can be added. Merge notes, `session.cpp` (include + five marked lines): `Fork::notifyHostHdr(enabled)` first in `clSetHdrMode()`; `Fork::sessionStarted(m_Window)` after `m_InputHandler->setWindow(m_Window)` in `exec()`; `Fork::notifyWindowState(m_Window)` first in `case SDL_WINDOWEVENT:`; `Fork::notifyHostHdr(…)` after `setHdrMode(LiGetCurrentHostDisplayHdrMode())` in the decoder recreation path; `Fork::sessionEnded()` right after `DispatchDeferredCleanup:` |
| D5 | 0002 | video / renderer selection | `ffmpeg.cpp` | 8-bit (SDR) streams try `PlVkRenderer` before `EGLRenderer` when "Use Vulkan (libplacebo) renderer for SDR streams" is on (default), the same path as upstream's `PREFER_VULKAN=1`; EGL stays the fallback. 10-bit streams are unchanged. Merge note: the condition of the 8-bit `else` branch in `createFrontendRenderer()` is `qgetenv("PREFER_VULKAN") == "1" \|\| Fork::preferVulkanForSdr()` |
| D6 | 0003 | video / upscaler | `app/fork/scaler.*`, `app/fork/plvkscaler.*`, `app/fork/shaders/*`, `app/fork/intelpreferences.*`, `app/gui/IntelSettingsGroup.qml`, `app/fork/renderstats.*`, `plvk.cpp`, `plvk.h`, `app.pro` | Seven upscalers on the Vulkan (libplacebo) renderer, for HDR (PQ) and SDR: bilinear (default, as before), libplacebo's `spline36`, `lanczos` and `ewa_lanczossharp`, and FSR 1, NIS and RAVU-Zoom-AR r2 as vendored mpv user shaders (`LUMA` hooks, parsed from the qrc on first use; their PQ define and sharpness are written every frame). The three libplacebo filters run with `antiring = 0.8`. NIS always runs in its SDR mode (`NIS_HDR_MODE` 0), because its HDR mode oversharpened PQ content; FSR 1 follows the source (`FSR_PQ`). Settings → `moonlight-intel`: upscaler, FSR 1 / NIS sharpness, debanding (libplacebo defaults). A hook that fails to parse, or that libplacebo disables at render time, falls back to bilinear and the overlay shows `<name> failed -> bilinear`. The Scaling line shows the scaler with its sharpness, its GPU ms and the deband ms; both are part of the video time. Merge notes: `plvk.h`: include `fork/plvkscaler.h` + member `m_ForkScaler` in the fork block of the class; `plvk.cpp`: `m_ForkScaler.apply(m_Vulkan->gpu, m_Renderer, &m_RenderParams, &mappedFrame)` right before `logFrameInfo(&mappedFrame)`, and `m_ForkScaler.destroy()` as the first statement inside `if (m_Vulkan != nullptr) {` in the destructor (parsed hooks own GPU textures, so this must stay before `pl_renderer_destroy()` / `pl_vulkan_destroy()`); the `<algorithm>` include moved out with `summarizeGpuTimes()` (now in `renderstats`); everything else is fork code at the end of the file. `app.pro`: the fork block has a nested `libplacebo { }` section (`plvkscaler`, `shaders.qrc`), so builds without libplacebo carry neither |
| D7 | 0003 | input / overlay / benchmark | `app/fork/scaler.*`, `app/fork/toast.*`, `app/fork/systemstate.*`, `app/fork/renderstats.*`, `app/fork/fork.*`, `keyboard.cpp` | In-stream hotkeys, Ctrl+Alt+Shift + U / Y (next / previous upscaler), B (A/B against bilinear), `[` / `]` (sharpness ±0.05), T (benchmark sweep): key down, consumed, never sent to the host. Matched by keycode first, by scancode only for non-Latin keycodes, so upstream's combos keep their keys on every layout. Hotkey changes last for one stream; `(session)` on the Scaling line and toasts marks a scaler or sharpness that differs from the settings (hidden on the Scaling line while A/B or a benchmark step shows another scaler). Without an attached Vulkan renderer every key toasts "Upscaler needs the Vulkan renderer". Toasts: 1.5 s on upstream's status overlay, skipped while upstream shows a message there, and never clear one. Benchmark: every scaler for 1 s settle + 5 s measure with default sharpness and debanding off, one `bench` line per scaler (video avg / p99 / max, scaler avg, power source, platform profile, GT0 clock, battery W on battery, throttle reasons) and a `bench-summary` line per scaler (Δp99 / Δavg against bilinear), then the previous config returns; T, the stream ending or the renderer going away aborts it. A sampler thread reads sysfs at 4 Hz during a stream for the overlay's new `GPU state` line and the benchmark. Fork overlay lines wrap at `, ` before 85 characters, the overlay's 1024 px wrap at 20 pt (a wrapped overlay loses its outline). Merge notes: `keyboard.cpp`: include `fork/fork.h` + `if (Fork::handleStreamHotkey(event->keysym.sym, event->keysym.scancode)) return;` as the first statement inside the Ctrl+Alt+Shift block of `handleKeyEvent()`. On conflict re-insert it there; it must stay before upstream's keycode loop |

## Syncing with upstream

`master` never carries fork commits, so it always fast-forwards.

1. Update `master` — either press **Sync fork** on the `master` branch page on GitHub, or:
   ```bash
   git fetch upstream
   git switch master && git merge --ff-only upstream/master && git push origin master
   ```
2. Integrate into `intel` on a branch, never directly:
   ```bash
   git switch -c claude/sync-upstream-$(date +%Y%m%d) intel
   git merge --no-ff master          # resolve conflicts using the Deltas table above
   git submodule update --init --recursive
   # build (see CLAUDE.md), then PR into intel
   ```
3. In the PR: list upstream changes that touch files in the Deltas table, how each conflict was
   resolved, and update "Last upstream commit merged" above.

If `master` ever shows "N commits ahead" on GitHub, something was committed to it by mistake:
move those commits to a branch off `intel`, then let **Sync fork → Discard commits** reset it.

## Sync log

| Date | Upstream commit | PR | Notes |
|---|---|---|---|
| 2026-10-25 | `ac296864` | — | fork point |
