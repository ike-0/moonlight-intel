# 0004 — Wayland: serialise the frame-pacing vsync source with the Vulkan present

| Field | Value |
|---|---|
| Status | In review |
| Created | 2026-10-26 |
| Branch | `claude/0004-wayland-vsync-commit-race` |
| PR | ike-0/moonlight-intel PR 6 |

Written by the executing session from the plan 0003 hardware-test diagnosis, at the user's
request.

## Goal

With frame pacing on, GNOME no longer disconnects a stream on the Vulkan (libplacebo) renderer
(`Wayland display connection closed by server (fatal)`), however busy the GPU is. The
frame-pacing vsync source never commits the stream surface while a Vulkan present is building
its commit.

## Context

**Symptom** (plan 0003 hardware test, 2026-10-26, on battery, frame pacing on): with
RAVU-Zoom-AR r2 or EWA Lanczos Sharp the video GPU time was about 7.5 ms per 8.33 ms frame, and
the stream died about 5 s later.
- SDL: `Wayland display connection closed by server (fatal)`.
- gnome-shell: `WL: error in client communication (pid …)`.
- Kernel log: clean (no GPU hang or reset).

**Cause**, from a standalone repro (scratch): a fullscreen SDL window with a libplacebo FIFO
swapchain of depth 1, an HDR10 hint and EWA upscaling at 120 Hz, as plvk does, on battery
with the desktop in HDR.

| Run | Result |
|---|---|
| EWA, no vsync source | 25 s, no error |
| EWA + a copy of upstream's `WaylandVsyncSource` | protocol error after 2.2 s |
| Bilinear + the same vsync source | protocol error after 0.7 s |
| EWA + vsync source + a commit lock shared with the present | 25 s, no error, 2842 frame callbacks |

The protocol error, with the requests just before it (`WAYLAND_DEBUG=client`):

```
{mesa vk display queue}  -> wp_linux_drm_syncobj_surface_v1#57.set_acquire_point(…)
{mesa vk display queue}  -> wp_linux_drm_syncobj_surface_v1#57.set_release_point(…)
{Default Queue}          -> wl_surface#38.frame(new id wl_callback#61)
{Default Queue}          -> wl_surface#38.commit()
{mesa vk display queue}  -> wl_surface#38.attach(wl_buffer#78, 0, 0)
…
wl_display#1.error(wp_linux_drm_syncobj_surface_v1#57, 3, "Release or Acquire point set but no buffer attached")
```

- **Upstream:** `pacer/waylandvsyncsource.cpp` re-arms a `wl_surface.frame` callback on SDL's
  window surface with `wl_surface_commit()`. The callback is dispatched on the main thread by
  SDL's event pump. `Pacer::initialize()` creates this source whenever frame pacing is on under
  Wayland.
- **Mesa 26.2 with Mutter 51:** the Vulkan WSI uses explicit sync
  (`wp_linux_drm_syncobj_surface_v1`), `wp_fifo_v1` and `wp_commit_timing_v1`. A present is a
  sequence of requests, all issued on the render thread inside `vkQueuePresentKHR`:
  `set_acquire_point`, `set_release_point`, `attach`, `damage_buffer`, `set_timestamp`,
  presentation feedback, the FIFO barrier, then `commit`. libplacebo calls `vkQueuePresentKHR`
  in `pl_swapchain_submit_frame()` (swapchain.c L986).
- **Why it's fatal now:** a commit from another thread between the sync points and the
  `attach` is protocol error `no_buffer`. Without explicit sync the stray commit was harmless,
  which is why the race only surfaced with this stack.
- **Load:** in a real stream the render thread usually presents just after the vsync source's
  commit, so light loads survive for minutes. Heavy GPU work makes both threads wake on the
  same vblank, and the race hits within seconds. The repro presents on every vblank, so it hits
  at any load.
- **EGL:** Mesa's EGL (`libEGL_mesa`, `libgallium` 26.2.5) doesn't use the syncobj protocol, so
  `EGLRenderer` doesn't hit the fatal form.
- **Workaround until this is merged:** frame pacing off. Upstream then creates no vsync source.

## Scope

**In:**
- A process-wide surface-commit lock in the fork module.
- The vsync source takes it around its two `wl_surface_commit()` calls.
- plvk takes it around `pl_swapchain_submit_frame()`, in `renderFrame()` and
  `cleanupRenderContext()`.

**Out:**
- Other renderers (EGL, VAAPI-direct, SDL): no explicit sync, or not used on the target.
- Any change to how frame pacing schedules frames.
- Reporting upstream (CLAUDE.md rule 1).

## Design

- `fork.h` / `fork.cpp`: `class SurfaceCommitLock`, an RAII guard over one static `std::mutex`.
  Upstream files only include `fork/fork.h`.
- `pacer/waylandvsyncsource.cpp`:
  - the `#include "fork/fork.h"` line;
  - one marked line `Fork::SurfaceCommitLock commitLock;` right before each of the two
    `wl_surface_commit(…)` calls, in `initialize()` and `frameDone()`. Each guard lasts to the
    end of its function, which covers the commit and the flush.
- `plvk.cpp` (already includes `fork/fork.h`):
  - one marked line before `if (!pl_swapchain_submit_frame(m_Swapchain))` in `renderFrame()`.
    The guard lasts to the end of `renderFrame()`: the present, plus texture cleanup and unmap.
    The `goto UnmapExit`s come after it, so no jump crosses its initialisation.
  - one marked line inside `cleanupRenderContext()`'s `if`, before its submit.
- **Lock cost** in the repro (EWA, battery, 15 s): held at most 0.50 ms; the main thread waited
  at most 0.37 ms, 0.002 ms on average.
- **Merges:** if upstream rewrites the vsync source or plvk's present path, re-insert the
  guards right before the surface commit and the swapchain submit.

## Steps

1. Commit this plan.
2. Implement the lock and the four guard lines.
3. Build on Arch, plus a scratch `CONFIG+=disable-libplacebo` build.
4. `docs/FORK.md`: delta D8.
5. Validate the Hardware test commands, fill in Execution notes, set the status, open the PR.

## Acceptance criteria

- [x] Builds cleanly on Arch and with `CONFIG+=disable-libplacebo`.
- [x] No new warnings in touched files.
- [x] Upstream files touched only by marked lines: `waylandvsyncsource.cpp` 3 (include + 2),
      `plvk.cpp` 2.
- [x] Repro: with the lock, 25 s at full load with no protocol error; without it, an error
      within seconds (table above).
- [ ] Hardware: frame pacing **on**, HDR, EWA Lanczos Sharp on battery for 60 s with the UFO
      test scrolling: no disconnect. (2026-10-26: no disconnect while cycling all scalers or during
      the full battery sweep (test B). But EWA ran 15 s + 6 s and RAVU 9 s + 6 s, not 60 s each.
      See Execution notes.)
- [x] `docs/FORK.md` updated.

## Hardware test (user runs on the laptop)

This branch has no upscaler (plan 0003), so the test runs on a local merge of both branches,
which is needed to load the GPU. The test branch isn't pushed; delete it afterwards.

```bash
cd ~/repos/moonlight-intel
git fetch origin
git switch -c test/0003-0004 origin/claude/0003-upscaler-hotkeys
# Both branches add a row to docs/FORK.md, the only conflict; -X ours keeps 0003's copy of it
git merge -X ours --no-edit origin/claude/0004-wayland-vsync-commit-race
# Expect 2 and 2: the commit lock is in both files
grep -c "SurfaceCommitLock commitLock" app/streaming/video/ffmpeg-renderers/plvk.cpp app/streaming/video/ffmpeg-renderers/pacer/waylandvsyncsource.cpp
git submodule update --init --recursive
mkdir -p build && cd build && qmake6 ../moonlight-qt.pro && make -j"$(nproc)" release
~/repos/moonlight-intel/build/app/moonlight 2>&1 | tee /tmp/ml-0004.log
```

**Stream settings:**
- 1920×1200, 120 fps, AV1, HDR on, V-Sync on, **frame pacing on**, fullscreen.
- **On battery**; the UFO test scrolling.

**Tests:**
- **A.** Ctrl+Alt+Shift+U up to EWA Lanczos Sharp (bilinear → spline36 → lanczos → EWA) and
  leave it for 60 s. Then RAVU-Zoom-AR r2 for 60 s. It stutters (over budget), but the stream
  stays up.
- **B.** Plan 0003's test E: the benchmark sweep on battery runs to `Benchmark done`.

**Collect:**
```bash
grep -E "moonlight-intel:|Renderer '|connection closed" /tmp/ml-0004.log
journalctl --user -b --since "-30min" | grep -c "error in client communication"
```

**Expected:** no `connection closed` line, and a count of 0.

**Afterwards:** `git switch claude/0003-upscaler-hotkeys && git branch -D test/0003-0004`.

## Risks and upstream-merge impact

- **Main-thread stall:** the vsync source's frame callback runs on the main thread and now waits
  while a present is in progress. Measured at ≤ 0.4 ms. A present that blocks for long would
  delay event handling by as much, but `vkQueuePresentKHR` with FIFO-v1 doesn't block.
- **Other committers:** the guard only covers the vsync source. SDL itself commits the surface
  on some window events (e.g. resize) on the main thread. Those are rare and were not seen in
  the trace; out of scope unless they show up.
- **Merge impact:** three marked lines in `waylandvsyncsource.cpp`, two in `plvk.cpp` next to
  the swapchain submit calls.

## Execution notes

- **As designed.** One placement change: `SurfaceCommitLock` is declared right after
  `initialize()` in `fork.h`. Next to `logGpuTiming()` it conflicted with plan 0003's rewrite
  of the following comment.
- **Builds.**
  - Arch: clean. The only warnings are upstream's two `lock_queue` / `unlock_queue`
    deprecations in `plvk.cpp`, which were there before.
  - Scratch `CONFIG+=disable-libplacebo`: 0 warnings.
  - `waylandvsyncsource.o` references `Fork::SurfaceCommitLock`, so the guard is compiled in
    (`HAS_WAYLAND`).
  - The hardware test's local merge with plan 0003's branch (`-X ours`) builds, with no new
    warnings.
- **Repro** (scratch, not committed; same setup as Context, on battery, desktop in HDR):
  - the four runs in the Context table;
  - lock cost over 15 s of EWA: held at most 0.50 ms; main-thread wait at most 0.37 ms, average
    0.002 ms.
- **Merging with plan 0003.** `fork.h`, `fork.cpp` and `plvk.cpp` merge cleanly. `docs/FORK.md`
  conflicts because both branches add rows after D5. Whichever PR merges second needs `intel`
  merged in, keeping all of D6, D7 and D8. The hardware test's local merge uses `-X ours`,
  which only touches that file; its `grep` checks that the guard lines are in.

**Hardware test, first run (user, 2026-10-26, battery, local merge with plan 0003).**
- **Setup:** frame pacing on (`Frame pacing: target 120 Hz with 120 FPS stream`, no "Vsync
  source failed"), V-Sync on, HDR stream (`pq=1`, HDR swapchain), Vulkan with the VAAPI backend,
  all on battery.
- **Result:** the user cycled bilinear → spline36 → lanczos → EWA Lanczos Sharp → FSR 1 → NIS →
  RAVU → bilinear over about 2.5 min. No `connection closed` in the log, and no `error in client
  communication` in GNOME's journal after 10:10, the last unlocked repro run.

| Scaler | Held | Video GPU avg / p99 (ms) | Frames per 5 s |
|---|---|---|---|
| bilinear | 68 s | 0.27–0.48 / 0.45–0.54 | 594–599 |
| spline36 | 13 s | 2.76–4.09 / 3.79–4.21 | 595–600 |
| ewa_lanczossharp | 15 s | 8.03–8.16 / 8.39–8.42 | 499–505 |
| fsr1 | 17 s | 2.30–3.73 / 3.27–3.81 | 595–596 |
| nis | 16 s | 2.64–3.20 / 4.63–5.17 | 593–598 |
| ravu-zoom-ar-r2 | 9 s | 4.20 / 8.16 | 570 |

- **Interpretation:** before the fix, EWA at this load (8.0–8.2 ms per 8.33 ms frame, about
  100 fps) disconnected 8 s after switching, and the repro within 2 s. It now ran 15 s.
- **Test B (same day):** plan 0003's benchmark sweep on battery, with frame pacing on, ran to
  `bench done`: 7 + 7 lines, config restored, every step at least 540 frames. EWA measured video
  p99 8.24 ms at 544 frames, so the GPU was saturated, as in the crashes before the fix.
  - No `connection closed` in the log.
  - No `error in client communication` in GNOME's journal since 10:20, when the fixed build
    started.
- **Still open:** the planned 60 s holds. EWA has run 15 s + 6 s and RAVU 9 s + 6 s without a
  disconnect. Before the fix, the same load failed within 5–8 s in streams and 2 s in the
  repro.
