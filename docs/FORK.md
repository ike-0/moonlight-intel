# Fork ledger

Every behavioural difference between `intel` and upstream `master`, so upstream merges can be
resolved with intent rather than guesswork. Update it in the same PR as the change.

## Upstream base

| Field | Value |
|---|---|
| Last upstream commit merged into `intel` | `ac296864` (2026-10-24) |
| Fork created | 2026-10-25 |

## Deltas

| ID | Plan | Area | Files | What and why |
|---|---|---|---|---|
| — | — | tooling | `CLAUDE.md`, `docs/`, `.claude/settings.json`, `.gitignore` | Fork workflow scaffolding; no runtime change |
| — | 0001 | docs | `CLAUDE.md`, `docs/arch.md` | Target desktop is GNOME; Arch packages, build and diagnostics guide. No runtime change |
| D1 | 0001 | video / libplacebo | `plvk.cpp`, `plvk.h` | Video frames render with member `m_RenderParams` (a copy of `pl_render_fast_params`, same defaults) so later plans tune one place. Logs `moonlight-intel: frame …` (swapchain + source size and colour space) on the first frame and on change. GPU timing: see D3. Merge note: in `renderFrame()` keep `&m_RenderParams` in the `pl_render_image()` call and the hook lines around it (`logFrameInfo()`, `recordRenderStats()`); the rest is appended at the end of the file / class |
| — | 0002 | docs | `CLAUDE.md`, `docs/arch.md` | `gputop` replaces `intel_gpu_top` (deprecated on the `xe` driver); notes on fork settings and desktop HDR. No runtime change |
| D2 | 0002 | settings / fork module | `app/fork/fork.*`, `app/fork/intelpreferences.*`, `app/gui/IntelSettingsGroup.qml`, `app.pro`, `main.cpp`, `qml.qrc`, `SettingsView.qml` | Fork code lives in `app/fork/`; upstream files call only `fork/fork.h`. Settings → `moonlight-intel` group (last in the right column) holds every fork option, via the `IntelPreferences` QML singleton (`IntelPreferences 1.0`), stored in `[moonlight-intel]` of the shared `Moonlight.conf` and written through on change. Stock Moonlight ignores the group. Merge notes: `app.pro`: one marked block after the main `HEADERS` list (fork sources, plus `QT += dbus` and the GNOME backend under `linux:qtHaveModule(dbus)`); `main.cpp`: include + `Fork::initialize()` right after the `StreamingPreferences` singleton registration; `qml.qrc`: `gui/IntelSettingsGroup.qml`; `SettingsView.qml`: `IntelSettingsGroup {}` as the last item of `settingsColumn2` |
| D3 | 0002 | video / stats overlay | `plvk.cpp`, `plvk.h`, `app/fork/renderstats.*`, `ffmpeg.cpp` | The libplacebo timing callback is always installed. Video GPU time excludes the OSD `"overlay"` passes, so toggling the overlay doesn't change it; scaler passes are summed separately. About once a second plvk publishes GPU avg/p99/max, source → target size, scaler, swapchain size and colour space to `RenderStats`; the snapshot is cleared when the renderer is destroyed. The stats overlay gets `GPU render (video)`, `Scaling` and `Output` lines (only whole lines are appended). The 5 s `gpu-time(video)` log and the one-time `gpu-pass[i]` list (overlay passes marked) run when "Log GPU render timing" is on or `MOONLIGHT_INTEL_GPU_TIMING=1`. Merge notes: `ffmpeg.cpp`: include + `Fork::appendOverlayStats(…)` as the last statement of `stringifyVideoStats()`; plvk: constructor block and `recordRenderStats(&mappedFrame, &targetFrame)` after `pl_render_image()`, the rest appended at the end of the file / class |
| D4 | 0002 | desktop HDR | `app/fork/desktophdr*`, `app/fork/gnomedisplayconfig.*`, `session.cpp` | On GNOME, while the host sends HDR (optionally only while fullscreen), the panel hosting the stream switches to BT.2100 and back to its previous colour mode afterwards, through Mutter's DisplayConfig D-Bus API with the **temporary** method (no `monitors.xml` write, no confirmation dialog). A panel already in HDR is never touched. A marker in `$XDG_RUNTIME_DIR/moonlight-intel/` lets the next launch undo a switch after a crash. If the swapchain is still SDR 2 s after a switch, the renderer is recreated once. All D-Bus calls run on a worker thread. Backends are behind `DesktopHdrBackend`, so KDE/Windows can be added. Merge notes, `session.cpp` (include + five marked lines): `Fork::notifyHostHdr(enabled)` first in `clSetHdrMode()`; `Fork::sessionStarted(m_Window)` after `m_InputHandler->setWindow(m_Window)` in `exec()`; `Fork::notifyWindowState(m_Window)` first in `case SDL_WINDOWEVENT:`; `Fork::notifyHostHdr(…)` after `setHdrMode(LiGetCurrentHostDisplayHdrMode())` in the decoder recreation path; `Fork::sessionEnded()` right after `DispatchDeferredCleanup:` |
| D5 | 0002 | video / renderer selection | `ffmpeg.cpp` | 8-bit (SDR) streams try `PlVkRenderer` before `EGLRenderer` when "Use Vulkan (libplacebo) renderer for SDR streams" is on (default), the same path as upstream's `PREFER_VULKAN=1`; EGL stays the fallback. 10-bit streams are unchanged. Merge note: the condition of the 8-bit `else` branch in `createFrontendRenderer()` is `qgetenv("PREFER_VULKAN") == "1" \|\| Fork::preferVulkanForSdr()` |

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
