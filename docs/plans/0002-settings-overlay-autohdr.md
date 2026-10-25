# 0002 — Fork settings group, stats-overlay readouts and GNOME auto-HDR

| Field | Value |
|---|---|
| Status | In review |
| Created | 2026-10-25 |
| Branch | `claude/0002-settings-overlay-autohdr` |
| PR | ike-0/moonlight-intel PR 3 |

## Goal

1. Moonlight's Settings page has a **`moonlight-intel`** group that holds every fork option.
   No fork behaviour needs an environment variable any more (the env var from plan 0001 stays
   as an override).
2. The stats overlay (Ctrl+Alt+Shift+S) shows three fork lines:
   - video-only GPU render time;
   - the scaling step (source → output size, ratio, scaler);
   - the output colour space and desktop-HDR state.
3. On GNOME with the panel in **SDR Native**, an HDR stream switches the panel to HDR (BT.2100)
   for the stream and restores SDR Native afterwards, also after a crash. No confirmation
   dialog, nothing written to `monitors.xml`.
4. SDR (8-bit) streams also render through `PlVkRenderer` (libplacebo) instead of `EGLRenderer`.
   This is controlled by a setting (default on), so the overlay readouts, and plan 0003's
   upscaler, cover every stream, not just HDR ones.

## Context

**Requirements set in planning (apply to this and all later plans):**
- All fork options live in Moonlight's settings window.
- All scaling/upscaling and fork render reporting lives in the stats overlay.
- The user keeps GNOME in **SDR Native** (colour mode 2) for colour accuracy everywhere else,
  and wants HDR only while streaming.
- Desktop-HDR switching is **GNOME only for now**. The code must leave room for KDE or Windows
  backends later; don't build those.
- The upscaler, previously "0002", is now plan 0003. It fills the group and overlay lines this
  plan creates. 0003 will also add an in-stream hotkey that cycles scalers; **not in this plan**.

**Depends on plan 0001 (PR 1).** That PR adds `m_RenderParams`, the libplacebo `info_callback`,
the `moonlight-intel: frame` log and `docs/arch.md`. This branch was cut from `intel` before
PR 1 merged, so step 1 merges `intel` in.

### Verified facts

Checked in planning against: Mutter `main` at `2b9c0787` (2026-10-23), libplacebo `v7.360.1`,
this repo at `intel` + PR 1. Line numbers are approximate; re-check them.

**GNOME display configuration (D-Bus, session bus):**
- Bus name and interface `org.gnome.Mutter.DisplayConfig`, object `/org/gnome/Mutter/DisplayConfig`.
  Interface XML: Mutter `data/dbus-interfaces/org.gnome.Mutter.DisplayConfig.xml`.
- `GetCurrentState() → (u serial, a((ssss)a(siiddada{sv})a{sv}) monitors, a(iiduba(ssss)a{sv}) logical_monitors, a{sv} properties)`
  - Monitor key: `(connector, vendor, product, serial)`.
  - Modes: `(id, w, h, refresh, preferred_scale, supported_scales, props)`; the current mode
    has `"is-current"`.
  - Per-monitor properties used here:
    - `"color-mode"` (u): 0 default, 1 BT.2100, 2 SDR native;
    - `"supported-color-modes"` (au);
    - `"rgb-range"` (u);
    - `"is-underscanning"` (b): absent means unsupported;
    - `"is-builtin"` (b);
    - `"display-name"` (s).
  - Global property `"layout-mode"` (u), and `"supports-changing-layout-mode"`.
- `ApplyMonitorsConfig(u serial, u method, a(iiduba(ssa{sv})) logical_monitors, a{sv} properties)`
  - Logical monitor: `(x, y, scale, transform, primary, [(connector, mode_id, props)])`.
  - Accepted monitor props: `"underscanning"` (b), `"color-mode"` (u), `"rgb-range"` (u).
  - **It replaces the whole configuration**, so every logical monitor must be resent with its
    current values.
  - `method`: 0 verify, 1 **temporary**, 2 persistent.
    - Temporary is not stored in `monitors.xml`.
    - It shows **no** "Keep these display settings?" dialog. Mutter only calls
      `request_persistent_confirmation()` for method 2 (`meta-monitor-manager.c` ~L3125).
- Refusals, both `org.freedesktop.DBus.Error.AccessDenied`:
  - stale serial ("based on stale information");
  - policy `ApplyMonitorsConfigAllowed == false`.
  - There is no per-caller allow-list.
- Signal `MonitorsChanged` fires after any change.
- CLI equivalents for manual checks: `gdctl show --properties`, and
  `gdctl set … --color-mode bt2100` (temporary unless `--persistent`).

**Moonlight:**
- **SDR streams don't use plvk today.** `FFmpegVideoDecoder::createFrontendRenderer()`
  (`ffmpeg.cpp` ~L338–420) tries `PlVkRenderer` only for 10-bit formats. For 8-bit it goes
  straight to `EGLRenderer` unless `PREFER_VULKAN=1` (the `else` branch, ~L405).
  - Confirmed on the laptop after PR 1 merged. An SDR stream logs
    `Renderer 'EGL/GLES' with 'VAAPI' backend chosen` and `EGLImage pixel format: 178`, and no
    `moonlight-intel: frame` lines. 178 is `AV_PIX_FMT_DRM_PRIME` in FFmpeg 9.0.2's headers,
    i.e. VA-API composed export.
  - plvk's name in that log line is `Vulkan (libplacebo)`.
- **HDR negotiation doesn't depend on the desktop mode.** `PlVkRenderer::getRendererAttributes()`
  always returns `RENDERER_ATTRIBUTE_HDR_SUPPORT`, because it tone-maps to SDR swapchains. The
  `VK_COLOR_SPACE_HDR10_ST2084_EXT` check (`plvk.cpp` ~L355, ~L500) only ranks GPUs. So switching
  the desktop *after* the stream starts never loses HDR negotiation.
- **Host HDR state** arrives in `Session::clSetHdrMode(bool)` (`session.cpp` ~L201) on
  moonlight-common-c's connection thread. It is re-applied after decoder creation
  (~L2253, `LiGetCurrentHostDisplayHdrMode()`). `clSetHdrMode` drops the call if the decoder
  lock is busy.
- **Measured on the laptop after PR 1 (2026-10-25)**, HDR stream 1920×1200@120, **AV1** (Main10):
  - HDR10 PQ reaches Vulkan on GNOME: the swapchain went `BT.709/sRGB hdr=off` →
    `BT.2020/PQ hdr=on` on frame 2. The swapchain is native 2880×1800 at 166% scale.
  - libplacebo time: 0.48 ms avg / 0.52 ms p99, in one fused pass,
    `"color decoding, color encoding"`.
  - Baseline with moving content (PR 2):
    - 0.35–0.47 ms avg / ≤ 0.53 ms p99 GPU time;
    - ≈ 119.4 fps rendered;
    - gputop: moonlight rcs 7.6 %, vcs 34.2 %; gnome-shell rcs 5.1 % (Mutter likely composites
      rather than scanning out);
    - GT0 at 800 MHz.
  - The `Using Mailbox present mode with V-Sync disabled` warnings come only from the test-only
    decoder probes, which always run with V-Sync off (`session.cpp` ~L288 asserts
    `!enableVsync || !testOnly`). The real stream uses FIFO and logs nothing.
- **Surface formats are cached.** libplacebo enumerates them only in
  `pl_vulkan_create_swapchain()` (`src/vulkan/swapchain.c` ~L429). If Mesa only exposes HDR10 PQ
  while the output is in HDR mode, a swapchain created before the switch stays SDR until
  recreated.
  - Plan 0001's **Run C** (GNOME SDR, HDR stream) answers this.
  - The design below works for both outcomes.
- **GPU timers are free.** libplacebo creates a timer for every dispatched pass whether or not
  `info_callback` is set (`src/dispatch.c` ~L962).
  - Pass descriptions: the OSD pass is `"overlay"` (`src/renderer.c` ~L945); scaler passes are
    `"<stage> upscaling (<filter>)"` (`src/shaders/sampling.c` ~L488); mpv user hooks use their
    `//!DESC`.
- **Current scaler:** `pl_render_fast_params` leaves `upscaler` NULL, i.e. bilinear texture
  sampling (`src/renderer.c` L202).
- **Overlay text:** `FFmpegVideoDecoder::stringifyVideoStats()` (`ffmpeg.cpp` ~L805) writes the
  text once a second on the decoder thread, into `OverlayManager`'s `char text[1024]`
  (`overlaymanager.h`). The renderer draws it on the render thread.
- **Settings:**
  - `StreamingPreferences` uses default-scope `QSettings` (`Moonlight.conf`, shared with stock
    Moonlight).
  - QML singletons are registered in `app/main.cpp` (~L943–970).
  - `app/gui/SettingsView.qml` has `settingsColumn1` and `settingsColumn2`;
    `advancedSettingsGroupBox` is the last group in column 2 and ends just before the column's
    closing brace.
- **Session lifecycle:** event loop in `Session::exec()`; window events at `case SDL_WINDOWEVENT:`
  (~L2057); teardown at `DispatchDeferredCleanup:` (~L2323); `toggleFullscreen()` (~L1518).
- **Qt:** `app/app.pro` uses `QT += core quick network quickcontrols2 svg`. QtDBus ships in
  Arch's `qt6-base`, so no new package is needed.

## Scope

**In:**
- A fork-owned module under **`app/fork/`**: preferences, render stats, desktop-HDR controller,
  GNOME backend.
- A `moonlight-intel` settings group with:
  - "Switch desktop to HDR during HDR streams": Off / During HDR streams (default) /
    During HDR streams, fullscreen only;
  - "Log GPU render timing" (default off; `MOONLIGHT_INTEL_GPU_TIMING=1` still forces it on);
  - "Use Vulkan (libplacebo) renderer for SDR streams" (default **on**): one marked condition
    in `createFrontendRenderer()`'s 8-bit branch. The HDR path is unchanged.
- Three overlay lines (GPU, Scaling, Output + desktop HDR), also written to the end-of-stream
  stats log.
- plvk changes:
  - the timing callback is always installed;
  - the `"overlay"` pass is excluded from the video GPU time;
  - 1 s stats are published for the overlay;
  - the 5 s log and pass list are gated by the setting or env var.
- GNOME auto-HDR, with crash recovery and a one-shot renderer-recreate fallback.
- `docs/FORK.md` deltas, `docs/arch.md` note.
- Docs fix: replace `intel_gpu_top` with **`gputop`**.
  - The target runs the `xe` kernel driver, where `intel_gpu_top` is deprecated (confirmed by
    the user). `gputop` (igt, `intel-gpu-tools` package) reads per-client DRM fdinfo.
  - Places: `CLAUDE.md` "Useful:" line, `docs/arch.md` diagnostics (~L85), and the 0001
    Baseline-results row label (`intel_gpu_top: Render/3D %, Video %` →
    `gputop: render %, video %`). The 0001 table row was already renamed in PR 2 (baseline
    results); skip it if it's done.
  - Check `gputop -h` for the actual options and don't invent flags. Note whether `sudo` is
    needed; fdinfo of your own processes is normally readable without it.

**Out:**
- The upscaler, deband, scaler hotkey (plan 0003).
- KDE or Windows desktop-HDR backends: leave the interface for them, no stubs.
- HDR brightness/luminance settings.
- Changing `StreamingPreferences` or any existing setting.
- Anything in `moonlight-common-c`; `.github/`.

## Design

### Merge footprint

Everything new lives in new files. Upstream files get small, `// moonlight-intel:`-marked hooks
only:

| Upstream file | Change |
|---|---|
| `app/app.pro` | one marked block: fork `SOURCES`/`HEADERS`; `linux { QT += dbus }` + GNOME backend sources |
| `app/main.cpp` | `#include "fork/fork.h"` + one call `Fork::initialize()` before the QML engine loads (next to the other `qmlRegister*` calls) |
| `app/qml.qrc` | one `<file>gui/IntelSettingsGroup.qml</file>` |
| `app/gui/SettingsView.qml` | one line in `settingsColumn2` after `advancedSettingsGroupBox` |
| `app/streaming/video/ffmpeg.cpp` | one call at the end of `stringifyVideoStats()`; one marked condition in `createFrontendRenderer()`'s 8-bit branch |
| `app/streaming/session.cpp` | ≤ 5 one-line hooks (see below) |
| `plvk.cpp` / `plvk.h` | extend the existing 0001 fork block; no new hook sites in upstream code beyond 0001's |

Don't touch `renderer.h`, `overlaymanager.*` (unless the buffer check in Steps fails),
`streamingpreferences.*` or `decoder.h`.

### New files (`app/fork/`, plus one QML file)

| File | Role |
|---|---|
| `fork.h/.cpp` | `Fork::initialize()`: registers QML singleton `IntelPreferences` (module `IntelPreferences 1.0`), starts the desktop-HDR controller (which runs crash recovery) |
| `intelpreferences.h/.cpp` | `QObject` singleton; `QSettings` group `moonlight-intel/`; write-through on change. Properties: `desktopHdrMode` (enum `DHM_OFF`, `DHM_HDR_STREAMS`, `DHM_HDR_STREAMS_FULLSCREEN`; default `DHM_HDR_STREAMS`), `logGpuTiming` (bool, default false), `vulkanForSdr` (bool, default true), read-only `desktopHdrAvailable` (bool) + `desktopHdrUnavailableReason` (QString) fed by the controller |
| `renderstats.h/.cpp` | Thread-safe snapshot (mutex) written by plvk's render thread, read by the decoder thread (overlay) and the HDR controller. `Fork::appendOverlayStats(char* out, int len)` formats the overlay lines |
| `desktophdr.h/.cpp` | Platform-independent controller: policy, debounce, marker file, worker `QThread`. Public API is thread-safe and non-blocking except `sessionEnded()` |
| `desktophdrbackend.h` | Abstract backend: `probe()`, `currentState()`, `setColorMode(target, mode)`. The factory returns the GNOME backend on Linux when available, else `nullptr`. Future KDE/Windows backends implement this |
| `gnomedisplayconfig.h/.cpp` | GNOME backend over QtDBus (Linux only) |
| `app/gui/IntelSettingsGroup.qml` | The settings `GroupBox` |

Fork-owned settings sit in the shared `Moonlight.conf` under `[moonlight-intel]`. Stock
Moonlight ignores them. **Never read that file during the session** (hard rule 4); the app reads
it at runtime.

### Settings group (`IntelSettingsGroup.qml`)

- **Layout:** `GroupBox` titled `moonlight-intel`, styled like the other groups (skyblue title,
  `font.pointSize: 12`, width from `parent`). Controls are normal focusable Qt Quick controls, so
  Tab/gamepad navigation works like the other groups.
- **Desktop HDR combo:**
  - Label "Switch desktop to HDR during HDR streams"; `ComboBox` with the three modes, bound to
    `IntelPreferences.desktopHdrMode`.
  - `enabled: IntelPreferences.desktopHdrAvailable`.
  - When unavailable, a small label shows `desktopHdrUnavailableReason`, e.g. "Requires GNOME
    48+ with an HDR-capable display".
  - Tooltip, same style as the existing ones: "GNOME only. The display switches to HDR when the
    host starts sending HDR, and back to its previous mode (e.g. SDR Native) when the stream
    ends. Each switch blanks the screen briefly."
- **GPU timing checkbox:** "Log GPU render timing", bound to `logGpuTiming`. Tooltip: "Writes
  libplacebo GPU timing to the log every 5 seconds. The stats overlay always shows it."
- **SDR renderer checkbox:** "Use Vulkan (libplacebo) renderer for SDR streams", bound to
  `vulkanForSdr`.
  - Tooltip: "Renders SDR streams through libplacebo, like HDR streams, so the moonlight-intel
    overlay readouts and upscaler apply. Off = upstream behaviour (EGL). Takes effect at the
    next stream."

### Vulkan renderer for SDR streams

- In the 8-bit `else` branch of `createFrontendRenderer()`, change only the condition, to
  `qgetenv("PREFER_VULKAN") == "1" || Fork::preferVulkanForSdr()`. One marked line; the rest of
  the upstream logic is untouched.
  - If `PlVkRenderer` fails to initialise, the existing fall-through to `EGLRenderer` still
    applies.
  - Like upstream's `PREFER_VULKAN`, it doesn't consult `vulkanIsSlow`.
- `Fork::preferVulkanForSdr()` returns a cached `std::atomic<bool>` that `IntelPreferences`
  updates on load and change. No `QSettings` access on the decoder path.
- The startup decoder probe (`Session::getDecoderInfo`) and the 10-bit path are unaffected.

### Render stats and overlay lines

**In plvk (extending plan 0001's code):**
- Always set `m_RenderParams.info_callback`/`info_priv`.
- In the callback, add `pass->last` to the frame's **video** time unless the shader description
  is exactly `"overlay"`.
  - Keep the per-pass list as in 0001, marking overlay passes in the log.
  - Also add up the time of passes whose description contains `"scaling ("` or comes from a
    user hook. The second bucket is empty until 0003; report it only when it is non-zero.
- Every ~1 s (render thread), publish to `RenderStats`:
  - video GPU avg / p99 / max (ms) over that window;
  - source crop w×h and target crop w×h (after `scaleSourceToDestinationSurface`);
  - swapchain w×h and colour space (primaries, transfer, `pl_color_space_is_hdr`);
  - source colour space HDR flag;
  - scaler name: `"bilinear"` while `m_RenderParams.upscaler` is NULL, otherwise the filter
    config's `name`;
  - a monotonically increasing `generation`.
- Clear the snapshot in `PlVkRenderer`'s destructor, so a stale renderer never shows.
- Keep the 5 s `gpu-time` log and the one-time `gpu-pass[i]` list.
  - Enabled when `IntelPreferences::logGpuTiming || MOONLIGHT_INTEL_GPU_TIMING=1`, read once in
    the constructor.
  - The log value is now video-only; say so in the log line (`gpu-time(video) …`).

**In `stringifyVideoStats()`:** one call appends `Fork::appendOverlayStats(...)` at the end.
- Lines appear only when the snapshot is fresh (updated within the last 3 s).
- Formats (ASCII only, since the overlay font may lack glyphs):
  ```
  GPU render (video): 1.21 ms avg / 1.65 p99 / 2.10 max of 8.33 ms frame
  Scaling: 1920x1200 -> 2880x1800 (x1.50), bilinear
  Output: 2880x1800 PQ BT.2020, desktop HDR: on (auto, eDP-1)
  ```
  - Frame budget = 1000 / stream fps.
  - Scaling: show `downscale (x0.90)` or `1:1` when applicable.
  - Output transfer/primaries: a **fork-local short-name table**. libplacebo only has long
    descriptive names; `pl_color_transfer_name(PL_COLOR_TRC_PQ)` is
    `"ITU-R BT.2100 PQ (perceptual quantizer), aka SMPTE ST2048"` (sic), which is too long for
    the overlay.
    - Primaries: BT.709 → `BT.709`, BT.2020 → `BT.2020`, DCI-P3 / Display-P3 → `P3`.
    - Transfer: sRGB → `sRGB`, BT.1886 → `BT.1886`, PQ → `PQ`, HLG → `HLG`,
      gamma 2.2 → `G2.2`, linear → `linear`.
    - Anything else → `prim#N` / `trc#N`.
    - The 0001 `frame` log line keeps the long names.
  - Desktop-HDR states: `off (setting)`, `unavailable`, `waiting for host HDR`,
    `on (auto, <connector>)`, `on (already on)`, `switching…`, `restored`,
    `failed: <short reason>`.
- Line 3 shows desktop HDR even when plvk isn't the renderer (snapshot stale → output part
  omitted).
- **Buffer check:** measure the worst-case total stats string with all lines and long values. If
  it can exceed 1023 bytes, raise `text[1024]` to `text[2048]` in `overlaymanager.h`, as one
  marked line, and record it in FORK.md. Otherwise leave it.

### Desktop-HDR controller

**Threading:**
- One app-wide controller with a worker `QThread` (own event loop).
- All D-Bus calls, timers and debounce run there.
- `notifyHostHdr()`, `notifyWindowState()` and `sessionStarted()` only post queued
  invocations; they are safe from the connection thread and the SDL loop and never block.
- `ApplyMonitorsConfig` replies only after the modeset (hundreds of ms), so it must never run on
  those threads.

**Session hooks** (`session.cpp`, one line each, marked):
1. `sessionStarted(window)` in `exec()` right after the SDL window exists. It captures the
   window's display and the policy from `IntelPreferences`.
2. `notifyHostHdr(enabled)` at the top of `clSetHdrMode()`, before the `SDL_TryLockMutex`, so it
   is never dropped. Also at ~L2253 next to `setHdrMode(LiGetCurrentHostDisplayHdrMode())`.
   Idempotent.
3. `notifyWindowState(isFullscreen)` in `case SDL_WINDOWEVENT:`, using
   `SDL_GetWindowFlags(m_Window) & SDL_WINDOW_FULLSCREEN` (covers `toggleFullscreen()` and
   compositor-driven changes).
4. `sessionEnded()` at `DispatchDeferredCleanup:`. It runs the restore and **waits up to 3 s**
   for it. The process may exit right after a stream (CLI `stream`, quit-after), and the restore
   must have landed by then.

**Policy (per session):**
- `DHM_OFF` or no backend → do nothing; overlay `off (setting)` / `unavailable`.
- **Arm:** read state.
  - Target monitor = the one hosting the window. Match the SDL display name to the connector or
    `display-name`.
  - Fallback: the only monitor whose `supported-color-modes` contains 1.
  - Several candidates and no match → log, overlay `unavailable` for this session.
  - Target already `color-mode == 1` → **passive**: never switch, never restore
    (`on (already on)`).
  - Otherwise remember `previousMode` (expect 2, SDR native; restore the exact value read).
- **Enter** when `hostHdr && (mode == DHM_HDR_STREAMS || fullscreen)` has held continuously for
  0 s (`DHM_HDR_STREAMS`) or 2 s (fullscreen-only):
  1. Write the marker.
  2. `GetCurrentState`.
  3. Rebuild the full config: every logical monitor with x, y, scale, transform, primary, and
     per monitor the `"is-current"` mode id, plus `underscanning` (only if `is-underscanning`
     is present) and `rgb-range` (if present). Set `color-mode = 1` only on the target. Pass
     `layout-mode` from the current properties when `supports-changing-layout-mode` is true.
  4. `ApplyMonitorsConfig(serial, 1 /*temporary*/, …)`.
  5. On AccessDenied with "stale", re-read and retry once. On any other error → overlay
     `failed: …`, delete the marker, don't retry this session.
- **Leave** when the enter condition has been false for 3 s (host HDR off), 2 s (left
  fullscreen in fullscreen-only mode), or immediately on `sessionEnded()`:
  1. Re-read state.
  2. Restore **only if** the target still exists and its `color-mode == 1`. Set `previousMode`
     on the current layout, temporary method.
  3. If the user changed it meanwhile (not 1, or the monitor is gone), leave it.
  4. Either way, delete the marker.
  - Re-entering later in the same session is allowed (fullscreen toggles, host HDR flapping).
- **Renderer fallback** (covers the "surface formats are cached" case):
  - 2 s after a successful enter, read `RenderStats`. If the source is HDR and the swapchain is
    not HDR, push one `SDL_RENDER_DEVICE_RESET` (at most once per session).
  - Log `moonlight-intel: desktop-hdr recreating renderer for HDR swapchain`.
  - Session already handles that event by recreating the decoder and renderer.
- **Logging:** every transition logs one `moonlight-intel: desktop-hdr …` line with the
  connector, old→new mode, serial and result. Never log vendor/product serial strings.

**Marker and crash recovery:**
- Location: `$XDG_RUNTIME_DIR/moonlight-intel/desktop-hdr.json`. If `XDG_RUNTIME_DIR` is unset,
  disable the feature and say so in the reason.
- Contents: `{connector, vendor, product, serial, previousMode, pid}`. Written before the switch,
  removed after restore or abandon.
- The runtime dir lives as long as the login session, which is exactly how long Mutter keeps a
  temporary config. A stale marker can't outlive the state it describes.
- `Fork::initialize()` runs recovery on the worker. If a marker exists, its pid is dead, and the
  monitor (all four ids match) is still in mode 1, restore `previousMode`. Then delete the
  marker. Log either way.

**Availability probe** (feeds `desktopHdrAvailable`/reason), run once at initialize on the
worker:
- the D-Bus name exists;
- `ApplyMonitorsConfigAllowed` is true;
- at least one monitor lists 1 in `supported-color-modes`.

**GNOME backend:**
- Use `QDBusInterface`/`QDBusMessage` with explicit signatures. Demarshal nested structs with
  `QDBusArgument`, or via `QDBusMessage::arguments()` and manual `beginStructure` walking.
- Keep all variant/struct handling in `gnomedisplayconfig.cpp`.
- Guard the whole file with `#ifdef Q_OS_LINUX`. The factory returns `nullptr` elsewhere, so
  macOS/Windows builds compile and show the option disabled.

## Steps

1. **Precondition:** PR 1 is merged
   (`gh api repos/ike-0/moonlight-intel/pulls/1 --jq .merged_at` is not `null`).
   Then `git fetch origin && git merge --no-edit origin/intel` into this branch (merge, no
   rebase). If PR 1 isn't merged, stop and ask.
2. Read `docs/FORK.md` and this plan; re-check every line reference in **Verified facts**.
3. Add `app/fork/` with `fork`, `intelpreferences`, `renderstats`; the `app.pro` block;
   `Fork::initialize()` in `main.cpp`; `IntelSettingsGroup.qml` + qrc + the SettingsView line.
   Build.
4. Extend plvk (always-on callback, overlay-pass exclusion, scaling bucket, 1 s publish, setting
   or env gating for the log). Add the `stringifyVideoStats()` hook and `appendOverlayStats()`.
   Do the buffer check. Add the SDR condition in `createFrontendRenderer()`. Build.
5. Add `desktophdrbackend.h`, `gnomedisplayconfig`, `desktophdr`; `QT += dbus` under `linux`;
   the session hooks. Build.
6. **Local session only, read-only D-Bus check:** run `gdctl show --properties` and confirm the
   property names and the target connector match the plan. Don't run `gdctl set`; the user runs
   the switching tests.
   - If GNOME's actual API differs from **Verified facts**, stop and write it under Execution
     notes.
7. Cloud-session check (if run in the cloud): `CONFIG+=disable-libplacebo` build still
   compiles. `app/fork/` must not depend on libplacebo; only plvk publishes stats.
8. Update `docs/FORK.md`:
   - new rows D2 (settings + fork module), D3 (overlay lines + plvk timing changes), D4
     (desktop HDR), D5 (Vulkan renderer for SDR streams);
   - the merge notes for each upstream hook line.
   Add a short "Desktop HDR" note to `docs/arch.md` (`gdctl show --properties` to inspect),
   and do the `intel_gpu_top` → `gputop` docs fix.
9. Plan status, criteria, Execution notes; PR into `intel` with
   `gh pr create --repo ike-0/moonlight-intel --base intel`, Hardware test copied verbatim.

## Acceptance criteria

- [x] Arch build with libplacebo: clean, no new warnings in touched files. (Cloud:
      `disable-libplacebo` build compiles.)
- [x] Non-Linux code paths compile by inspection: no QtDBus outside `linux`, backend factory
      returns `nullptr`.
- [ ] Settings page shows the `moonlight-intel` group at the end of the right column. Values
      persist across restarts under `[moonlight-intel]`; existing settings unchanged.
- [x] Overlay shows the GPU, Scaling and Output lines during a plvk stream. Toggling the overlay
      on/off doesn't shift the logged `gpu-time(video)` value beyond noise.
- [x] With the setting on and the panel in SDR Native, an HDR stream switches the panel to HDR
      with no dialog, and the overlay shows `desktop HDR: on (auto, …)`.
- [ ] Quitting the stream restores **SDR Native** (not "default"). `monitors.xml` is unchanged.
- [x] Panel already in HDR before the stream → untouched during the stream and left in HDR after.
- [x] `kill -9` of Moonlight mid-stream → the next Moonlight launch restores SDR Native.
- [ ] Fullscreen-only mode: windowed → SDR Native; fullscreen → HDR after ~2 s; back to windowed
      → SDR Native after ~2 s. Fullscreen but not in front (overview, other workspace or window)
      → SDR Native after ~2 s; back to the stream → HDR after ~2 s.
- [ ] Setting Off → no D-Bus writes (log shows none).
- [ ] SDR stream with the SDR-Vulkan setting on → log `Renderer 'Vulkan (libplacebo)' with 'VAAPI'
      backend chosen`, and the overlay lines appear. Setting off → `Renderer 'EGL/GLES' …`, as
      upstream.
- [x] `docs/FORK.md` and `docs/arch.md` updated; `git grep intel_gpu_top -- CLAUDE.md docs/arch.md`
      returns nothing.

## Hardware test (user runs on the laptop)

```bash
# 0. GNOME in SDR Native, HDR off. Record the starting state (no Moonlight running).
gdctl show --properties | grep -E 'Monitor |color-mode'   # expect color-mode ⇒ sdr-native
sha256sum ~/.config/monitors.xml > /tmp/ml-0002-monitors.sha

# 1. Build (starts at the repo root, so it works from any directory)
cd ~/repos/moonlight-intel
git switch claude/0002-settings-overlay-autohdr && git pull
git submodule update --init --recursive
mkdir -p build && cd build && qmake6 ../moonlight-qt.pro && make -j"$(nproc)" release

# 2. Run (from build/, where step 1 leaves you)
./app/moonlight 2>&1 | tee /tmp/ml-0002.log
```

**Stream settings:** 1920×1200, 120 fps, AV1, HDR on, V-Sync on, frame pacing on, fullscreen,
moving content (e.g. a scrolling UFO test). The 1728×1080 source was dropped: all fork work
targets 1920×1200. The host desktop or game must be in HDR. In Settings → `moonlight-intel`: "During HDR
streams", GPU timing log **on**.

**Tests:**
- **A. Auto switch:** start the stream.
  - Expect one brief blank, then HDR. Note roughly how long the blank lasts.
  - Overlay (Ctrl+Alt+Shift+S) shows `desktop HDR: on (auto, eDP-1)` and `Output: … PQ …`.
  - Quit the stream: one blank, back to SDR Native.
  - Check:
    ```bash
    gdctl show --properties | grep -E 'Monitor |color-mode'   # expect color-mode ⇒ sdr-native
    sha256sum -c /tmp/ml-0002-monitors.sha                    # expect OK
    ```
- **B. Overlay timing:** while streaming, read `gpu-time(video)` lines with the overlay on and
  off. The values should agree within noise.
- **C. Already HDR:** turn GNOME HDR on manually, stream, quit. HDR must stay on. Then turn it
  back to SDR Native.
- **D. Crash recovery:** start a stream (panel goes HDR), then in another terminal
  `pkill -9 -f '^./app/moonlight'`. The panel stays HDR. Launch `./app/moonlight` again: the
  panel returns to SDR Native at launch.
- **E. Fullscreen-only:** set "During HDR streams, only while fullscreen and in front". Start the
  stream, Ctrl+Alt+Shift+X to windowed (SDR Native after ~2 s), and back (HDR after ~2 s).
  - Then, still fullscreen, swipe up with three fingers (overview) or switch workspace: SDR Native
    after ~2 s. Back to the stream: HDR after ~2 s.
  - A peek shorter than 2 s doesn't switch. While the stream is out of front, the overlay
    shows `waiting for focus` or `restored`.
  - Stay in the overview for more than 2 s: the panel stays in HDR while the overview is open
    (log: `desktop-hdr leave waits for the overview to close`).
    - Pick another window: SDR Native about 1–1.5 s after the overview closes.
    - Or go back to the stream: no switch at all.
- **F. Off (doubles as 0001's Run C):** set Off, GNOME in SDR Native, and stream HDR.
  - The panel must not change, and the log has no `desktop-hdr` writes.
  - Note whether the second `frame` line reaches `BT.2020/PQ hdr=on`. If yes, Mesa offers PQ
    while the desktop is SDR, and the renderer-recreate fallback is only a safety net.
- **G. SDR renderer:** in Moonlight's Basic Settings untick *Enable HDR* and stream.
  - The log shows `Renderer 'Vulkan (libplacebo)' with 'VAAPI' backend chosen`.
  - The overlay shows the GPU/Scaling/Output lines.
  - The panel stays in SDR Native.
  - Then untick "Use Vulkan (libplacebo) renderer for SDR streams": the next stream logs
    `Renderer 'EGL/GLES' …`.
  - Re-tick both afterwards.

**Collect (no IPs or hostnames):**
```bash
grep -E "moonlight-intel:|Renderer '" /tmp/ml-0002.log
```

**Expected:** all of A–G as described. If the overlay shows `Output: … SDR …` after the switch
in A, check that the renderer fallback line `desktop-hdr recreating renderer` appeared, then
`Output: … PQ`.

## Risks and upstream-merge impact

- **`session.cpp`** is large and busy upstream. Keep the hooks as single marked lines at stable
  anchors: `clSetHdrMode`, the `SDL_WINDOWEVENT` case, `DispatchDeferredCleanup`, after window
  creation.
- **`SettingsView.qml`** is busy too. One line at the end of column 2 is the least
  conflict-prone spot.
- **Mutter API drift:** the colour-mode values and the method enum have been stable since
  GNOME 48. Check `supported-color-modes` rather than assuming, and fail soft with a reason
  string.
- **Modeset blank:** unmeasured; test A records it. If it's long, that argues against the
  fullscreen-only mode, not against the feature.
- **The user changes display settings mid-stream:** handled by the "still mode 1" check. GNOME
  Settings may show HDR on during a stream; that's expected.

## Execution notes

Executed locally on the laptop (2026-10-25). The remaining criteria need the hardware test.

**Step 1:** PR 1 and PR 2 were merged; `origin/intel` merged into this branch without conflicts.

**Re-checked facts (step 2, step 6):**
- All line references in Verified facts hold within a few lines.
- `gdctl show --properties` plus a raw `GetCurrentState` on GNOME/Mutter 51.1 match the plan exactly:
  - method signatures as listed;
  - `color-mode` u (0 default, 1 bt2100, 2 sdr-native), currently 1;
  - `supported-color-modes` `[0, 2, 1]`, `rgb-range` present, `is-underscanning` absent on
    `eDP-1`;
  - `layout-mode` 1 with `supports-changing-layout-mode` true;
  - `ApplyMonitorsConfigAllowed` true.
- SDL (sdl2-compat 2.32.74 on SDL 3.4.18, Wayland) names the panel `Built-in display`, which
  matches Mutter's `display-name`. The connector is `eDP-1`.
- One write-shaped call, beyond the read-only check: a scratch harness ran the real backend code
  with `ApplyMonitorsConfig` **method 0 (verify)** and the panel's *current* colour mode. Mutter
  accepted the marshalled configuration. Verify never applies, and even an apply would have
  changed nothing. `gdctl` showed the panel unchanged afterwards.

**Plan vs code: end-of-stream stats log.** The fork lines do **not** reach the
`Global video stats` log.
- Upstream writes that log in `FFmpegVideoDecoder::reset()` *after* deleting the renderers, so
  the render snapshot is already cleared, as this plan requires.
- Its buffer is `char[512]`, and upstream's own text already takes about 500 bytes.
- `appendOverlayStats()` appends whole lines only, so the log is never cut mid-line.
- The 5 s `gpu-time(video)` log covers the logging need. Getting the lines into that log would
  need two more `ffmpeg.cpp` changes (a bigger buffer, and a snapshot that outlives the
  renderer). That is a planning call.

**Buffer check:** the worst-case overlay text is 853 bytes (upstream 572, counting
`DISPLAY_BITRATE`, plus fork 280), under 1024. `overlaymanager.h` is unchanged.

**Deviations, all small:**
- **When the target is chosen.** The target monitor and the "already HDR → passive" decision are
  made at the first enter attempt, not at session start.
  - On Wayland, SDL only knows the window's output after the surface enters it, so at window
    creation it reports display 0.
  - SDR streams and Off now make no D-Bus calls at all.
  - Passive still lasts for the whole session. Every window event refreshes the display name.
- **Early host notifications.** Host-HDR notifications that arrive before `sessionStarted` are
  ignored, and each session starts with host HDR off. The decoder-creation hook reports the real
  state right after the window is shown, so a stale callback from the previous session can't
  switch the panel.
- **Hook signature.** `notifyWindowState(SDL_Window*)` replaces `(bool isFullscreen)`. The fork
  reads the fullscreen flag and display name itself, and the hook stays one line.
- **Overlay states.** The states are ASCII: `switching...`. I added `waiting for fullscreen` for
  fullscreen-only mode with host HDR on while windowed. When no fresh plvk stats exist (EGL),
  line 3 reads `Desktop HDR: <state>`.
- **Failed restores.** A restore that fails on D-Bus keeps the marker, so the next launch retries.
  "User changed it" and "monitor gone" delete it, as planned. Recovery always deletes the
  marker, so it runs once.
- **Clearing the snapshot.** The snapshot is cleared by an RAII member
  (`Fork::RenderStatsPublisher`), not by a line in `~PlVkRenderer`, so plvk gets no new hook
  site. It clears only if it was the last publisher, so a probe renderer being destroyed can't
  blank a live one.
- **Scaling bucket.** It matches `"scaling ("` steps only. libplacebo gives no generic marker for
  user-hook passes, so plan 0003 should match its own hooks' `//!DESC` in `isScalingPass()`.
- **Build scope.** The GNOME backend builds under `linux:qtHaveModule(dbus)` with a
  `HAVE_GNOME_DISPLAY_CONFIG` define, instead of `linux { }` and an in-file `Q_OS_LINUX` guard.
  Arch builds the same. A Linux Qt without QtDBus (e.g. the Steam Link SDK) still builds, with
  the option disabled.
- **Restore timing.** `sessionEnded()` sits right after `DispatchDeferredCleanup:`, so the
  restore blank happens while the stream window is still up and before the mouse is released.

**Hardware test fixes (found in PR review):**
- `gdctl` prints `color-mode ⇒ sdr-native`, not "Color mode … SDR native", and `^Monitor` only
  matched the `Monitors:` header. Both `gdctl` checks now use `grep -E 'Monitor |color-mode'`.
- Step 1 assumed the repo root as the working directory. Run from `build/`, it made
  `build/build/` and couldn't find `../moonlight-qt.pro`. It now starts with
  `cd ~/repos/moonlight-intel`.

**Hardware test results (user, 2026-10-25):**
- **A. Auto switch:** the panel switched to HDR on stream start. The overlay showed:
  ```
  GPU render (video): 0.61 ms avg / 0.63 p99 / 0.66 max of 8.33 ms frame
  Scaling: 1920x1200 -> 2880x1800 (x1.50), bilinear
  Output: 2880x1800 PQ BT.2020, desktop HDR: on (auto, eDP-1)
  ```
  - Host 120.58 fps, rendered 119.59 fps, 0.82 % "network jitter" drops, 7.69 ms frame queue
    delay, 0.83 ms decode.
  - The ~1 fps of drops matches the host sending about 0.6–1 fps more than the 120 Hz panel
    shows. That is upstream pacer behaviour, not fork behaviour.

**Bug found in test A: the overlay pass was counted as video GPU time.**
- 0.61 ms is above the 0001 baseline of 0.35–0.47 ms, although it is meant to exclude the
  overlay. That baseline was most likely measured with the overlay off: its overlay values
  weren't recorded.
- The plan's fact "the OSD pass is `"overlay"`" holds only for its first step. libplacebo
  7.360.1 (`src/renderer.c` L945–978) converts the overlay's colours in the same shader.
- A headless libplacebo run on this GPU, with plvk's sRGB overlay on an HDR10 source, prints
  these pass descriptions:
  - PQ BT.2020 target: `overlay, color decoding, colorspace conversion, color encoding`;
  - sRGB target: `overlay, color decoding, color encoding`;
  - the video pass, in both cases: `color decoding, color encoding` on PQ, plus tone and gamut
    mapping on sRGB.
- So neither the planned "description is exactly `overlay`" nor the first implementation ("every
  step is `overlay`") matched it.
- **Fix:** a pass is an overlay pass when its **first** step is `"overlay"`. Each overlay is
  drawn by a fresh shader that starts with that step, and no video pass does. Test B rechecks
  it.

**After the fix:**
- **B. Overlay timing:** with the overlay on, the overlay reads
  `GPU render (video): 0.47 ms avg / 0.53 p99 / 0.55 max`.
  - The log's six 5 s windows, overlay on and off, read 0.33–0.47 avg, 0.47–0.53 p99 and
    ≤ 0.56 ms max (591–598 frames each). That is the 0001 baseline range.
  - The one-time pass list now shows the split:
    - `gpu-pass[0] … 0.467 ms: color decoding, color encoding`;
    - `gpu-pass[1] … 0.159 ms: overlay, color decoding, colorspace conversion, color encoding
      (overlay, not in video time)`.
- **D. Crash recovery:** after `kill -9` mid-stream the panel stayed in HDR. The next launch
  restored `sdr-native`.
- **C. Already HDR:** with GNOME HDR turned on by hand, the overlay showed
  `Output: 2880x1800 PQ BT.2020, desktop HDR: on (already on)`, and HDR stayed on after
  quitting. Video GPU time: 0.48 avg / 0.53 p99 / 0.57 max ms.
- **monitors.xml:** `sha256sum -c /tmp/ml-0002-monitors.sha` → `OK` after tests A–D.
- **E. Fullscreen-only, first build:** Ctrl+Alt+Shift+X switching worked as planned. But a
  three-finger swipe up (overview) or a workspace switch left the panel in HDR, because the
  stream window stays fullscreen there.

**Change after test E (user decision):**
- Fullscreen-only mode now also requires the stream window to have keyboard focus. GNOME takes
  focus away for the overview, another workspace or Alt+Tab.
- The mode's label is now "During HDR streams, only while fullscreen and in front", and the
  tooltip explains it.
- "During HDR streams" still ignores focus.
- The focus state comes from the existing window-event hook (`SDL_WINDOW_INPUT_FOCUS`), so there
  is no new upstream line.
- New overlay state: `waiting for focus`.
- The controller harness adds 8 checks:
  - unfocused start;
  - HDR 2 s after focus;
  - a 1 s peek doesn't switch;
  - SDR 2 s after focus loss;
  - HDR 2 s after focus returns;
  - restore at session end;
  - "During HDR streams" ignores focus.
- Criterion and test E are extended accordingly and need a re-run.

**Second change after test E (user request):**
- **Problem:** a switch that fell due while the GNOME overview was open made GNOME close the
  overview and refocus the stream, which then switched back.
- **Fix:** a due switch now waits while GNOME Shell's `OverviewActive` property is true.
  - It is read from the `org.gnome.Shell` D-Bus interface, which any client can read; checked
    on GNOME Shell 51.1.
  - The controller polls it every 0.5 s while a switch is held.
  - After the overview closes, it waits 1 s more, so a refocus of the stream can cancel the
    switch first.
  - Session end isn't held.
- **Interface:** `DesktopHdrBackend::overviewActive()` defaults to false; the GNOME backend
  implements it.
- **Tests:** the controller harness adds 7 checks (32 in total):
  - held for 4 s with the overview open;
  - grace period after it closes;
  - SDR after closing to another window;
  - no switch when the overview closes back to the stream;
  - session end still restores.

**Found, not changed:** with "Vulkan for SDR" on, `getDecoderInfo()`'s HEVC 8-bit probe would
also try plvk. On the target the HEVC Main10 probe succeeds first and returns, so that probe
never runs. Upstream's `PREFER_VULKAN=1` behaves the same way.

**Verified without the hardware test:**
- Full Arch build and a scratch `CONFIG+=disable-libplacebo` build. The only warnings are the six
  existing upstream `lock_queue`/`unlock_queue` deprecations in `plvk.cpp`.
- Scratch harnesses:
  - controller state machine with a fake backend, 17 checks: recovery, immediate enter, 3 s
    host-SDR hold against a 1 s flap, 2 s fullscreen debounce both ways, passive, Off, user
    change respected, one renderer recreate;
  - overlay formatter, including whole-line truncation;
  - the settings group rendered offscreen in the Material dark style, available and
    unavailable.
- Smoke run of the real binary with a scratch config: logs `moonlight-intel: desktop-hdr
  available`, reads `[moonlight-intel]`, and exits cleanly through `aboutToQuit`.
