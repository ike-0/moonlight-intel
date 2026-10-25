# 0002 — Fork settings group, stats-overlay readouts and GNOME auto-HDR

| Field | Value |
|---|---|
| Status | Ready |
| Created | 2026-10-25 |
| Branch | `claude/0002-settings-overlay-autohdr` |
| PR | — |

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
- **HDR negotiation doesn't depend on the desktop mode.** `PlVkRenderer::getRendererAttributes()`
  always returns `RENDERER_ATTRIBUTE_HDR_SUPPORT`, because it tone-maps to SDR swapchains. The
  `VK_COLOR_SPACE_HDR10_ST2084_EXT` check (`plvk.cpp` ~L355, ~L500) only ranks GPUs. So switching
  the desktop *after* the stream starts never loses HDR negotiation.
- **Host HDR state** arrives in `Session::clSetHdrMode(bool)` (`session.cpp` ~L201) on
  moonlight-common-c's connection thread. It is re-applied after decoder creation
  (~L2253, `LiGetCurrentHostDisplayHdrMode()`). `clSetHdrMode` drops the call if the decoder
  lock is busy.
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
  - "Log GPU render timing" (default off; `MOONLIGHT_INTEL_GPU_TIMING=1` still forces it on).
- Three overlay lines (GPU, Scaling, Output + desktop HDR), also written to the end-of-stream
  stats log.
- plvk changes:
  - the timing callback is always installed;
  - the `"overlay"` pass is excluded from the video GPU time;
  - 1 s stats are published for the overlay;
  - the 5 s log and pass list are gated by the setting or env var.
- GNOME auto-HDR, with crash recovery and a one-shot renderer-recreate fallback.
- `docs/FORK.md` deltas, `docs/arch.md` note.

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
| `app/streaming/video/ffmpeg.cpp` | one call at the end of `stringifyVideoStats()` |
| `app/streaming/session.cpp` | ≤ 5 one-line hooks (see below) |
| `plvk.cpp` / `plvk.h` | extend the existing 0001 fork block; no new hook sites in upstream code beyond 0001's |

Don't touch `renderer.h`, `overlaymanager.*` (unless the buffer check in Steps fails),
`streamingpreferences.*` or `decoder.h`.

### New files (`app/fork/`, plus one QML file)

| File | Role |
|---|---|
| `fork.h/.cpp` | `Fork::initialize()`: registers QML singleton `IntelPreferences` (module `IntelPreferences 1.0`), starts the desktop-HDR controller (which runs crash recovery) |
| `intelpreferences.h/.cpp` | `QObject` singleton; `QSettings` group `moonlight-intel/`; write-through on change. Properties: `desktopHdrMode` (enum `DHM_OFF`, `DHM_HDR_STREAMS`, `DHM_HDR_STREAMS_FULLSCREEN`; default `DHM_HDR_STREAMS`), `logGpuTiming` (bool, default false), read-only `desktopHdrAvailable` (bool) + `desktopHdrUnavailableReason` (QString) fed by the controller |
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
  Scaling: 1728x1080 -> 2880x1800 (x1.67), bilinear
  Output: 2880x1800 PQ BT.2020, desktop HDR: on (auto, eDP-1)
  ```
  - Frame budget = 1000 / stream fps.
  - Scaling: show `downscale (x0.90)` or `1:1` when applicable.
  - Output transfer/primaries: libplacebo short names.
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
   Do the buffer check. Build.
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
     (desktop HDR);
   - the merge notes for each upstream hook line.
   Add a short "Desktop HDR" note to `docs/arch.md` (`gdctl show --properties` to inspect).
9. Plan status, criteria, Execution notes; PR into `intel` with
   `gh pr create --repo ike-0/moonlight-intel --base intel`, Hardware test copied verbatim.

## Acceptance criteria

- [ ] Arch build with libplacebo: clean, no new warnings in touched files. (Cloud:
      `disable-libplacebo` build compiles.)
- [ ] Non-Linux code paths compile by inspection: no QtDBus outside `linux`, backend factory
      returns `nullptr`.
- [ ] Settings page shows the `moonlight-intel` group at the end of the right column. Values
      persist across restarts under `[moonlight-intel]`; existing settings unchanged.
- [ ] Overlay shows the GPU, Scaling and Output lines during a plvk stream. Toggling the overlay
      on/off doesn't shift the logged `gpu-time(video)` value beyond noise.
- [ ] With the setting on and the panel in SDR Native, an HDR stream switches the panel to HDR
      with no dialog, and the overlay shows `desktop HDR: on (auto, …)`.
- [ ] Quitting the stream restores **SDR Native** (not "default"). `monitors.xml` is unchanged.
- [ ] Panel already in HDR before the stream → untouched during the stream and left in HDR after.
- [ ] `kill -9` of Moonlight mid-stream → the next Moonlight launch restores SDR Native.
- [ ] Fullscreen-only mode: windowed → SDR Native; fullscreen → HDR after ~2 s; back to windowed
      → SDR Native after ~2 s.
- [ ] Setting Off → no D-Bus writes (log shows none).
- [ ] `docs/FORK.md` and `docs/arch.md` updated.

## Hardware test (user runs on the laptop)

```bash
# 0. GNOME in SDR Native, HDR off. Record the starting state (no Moonlight running).
gdctl show --properties | grep -iE '^Monitor|Color mode'
sha256sum ~/.config/monitors.xml > /tmp/ml-0002-monitors.sha

# 1. Build
git switch claude/0002-settings-overlay-autohdr && git pull
git submodule update --init --recursive
mkdir -p build && cd build && qmake6 ../moonlight-qt.pro && make -j"$(nproc)" release

# 2. Run (from build/)
./app/moonlight 2>&1 | tee /tmp/ml-0002.log
```

**Stream settings:** 1728×1080, 120 fps, HEVC, HDR on, ~80 Mbps, V-Sync on, frame pacing on,
fullscreen. The host desktop or game must be in HDR. In Settings → `moonlight-intel`: "During HDR
streams", GPU timing log **on**.

**Tests:**
- **A. Auto switch:** start the stream.
  - Expect one brief blank, then HDR. Note roughly how long the blank lasts.
  - Overlay (Ctrl+Alt+Shift+S) shows `desktop HDR: on (auto, eDP-1)` and `Output: … PQ …`.
  - Quit the stream: one blank, back to SDR Native.
  - Check:
    ```bash
    gdctl show --properties | grep -iE 'Color mode'     # expect SDR native
    sha256sum -c /tmp/ml-0002-monitors.sha              # expect OK
    ```
- **B. Overlay timing:** while streaming, read `gpu-time(video)` lines with the overlay on and
  off. The values should agree within noise.
- **C. Already HDR:** turn GNOME HDR on manually, stream, quit. HDR must stay on. Then turn it
  back to SDR Native.
- **D. Crash recovery:** start a stream (panel goes HDR), then in another terminal
  `pkill -9 -f '^./app/moonlight'`. The panel stays HDR. Launch `./app/moonlight` again: the
  panel returns to SDR Native at launch.
- **E. Fullscreen-only:** set "During HDR streams, fullscreen only". Start the stream,
  Ctrl+Alt+Shift+X to windowed (SDR Native after ~2 s), and back (HDR after ~2 s).
- **F. Off:** set Off. A stream must not change the panel, and the log has no `desktop-hdr` writes.

**Collect (no IPs or hostnames):**
```bash
grep -E 'moonlight-intel:' /tmp/ml-0002.log
```

**Expected:** all of A–F as described. If the overlay shows `Output: … SDR …` after the switch
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

Filled in by the executing session: deviations from the plan, findings, follow-ups.
