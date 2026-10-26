# 0003 — Upscaler: libplacebo filters, FSR 1 / NIS / RAVU hooks, hotkey A/B and benchmark sweep

| Field | Value |
|---|---|
| Status | In review |
| Created | 2026-10-25 |
| Branch | `claude/0003-upscaler-hotkeys` |
| PR | ike-0/moonlight-intel PR 5 |

## Goal

1. The 1920×1200 stream is upscaled to the 2880×1800 panel by one of **seven** selectable
   scalers:
   - libplacebo's Bilinear, Spline36, Lanczos and EWA Lanczos Sharp;
   - FSR 1 (EASU + RCAS), NIS and RAVU-Zoom-AR r2, as vendored mpv-format GLSL hooks.

   This works for HDR (PQ) and SDR streams on the Vulkan (libplacebo) renderer.
2. The `moonlight-intel` settings group sets the default scaler, the FSR and NIS sharpness and
   debanding.
3. In-stream hotkeys (Ctrl+Alt+Shift+…) cycle scalers, A/B against bilinear and change sharpness,
   live and without restarting the stream. Each change shows a short toast and updates the
   stats overlay.
4. Ctrl+Alt+Shift+T runs a **benchmark sweep**: every scaler for 6 s. It logs one line per scaler
   with video GPU time, Δ against bilinear, GT0 clock and power state, so AC and battery runs
   can be compared directly.
5. The stats overlay shows the active scaler, its parameters and its GPU ms, plus a new
   `GPU state` line (power source, platform profile, GT0 clock).

Choosing the shipped default is **not** part of this plan. The default stays Bilinear (no
behaviour change), and the results PR sets it from the benchmark and A/B results.

## Context

**Motivation:** bitrate-bound. At 200 Mbps / 120 Hz, bits per pixel b = R / (W·H·f):
- R = bitrate (bit/s, here 200·10⁶);
- W×H = stream resolution (px);
- f = frame rate (Hz, here 120).

Native 2880×1800 gives b = 0.32; 1920×1200 gives b = 0.72 (2.25×). So the stream stays at
1920×1200 and the client upscales by exactly ×1.5.

**Budget (pass criterion):** Δp99 = p99(t_video | scaler) − p99(t_video | bilinear) ≤ 2 ms,
on AC and on battery separately.
- t_video: libplacebo GPU time per frame, OSD passes excluded, in ms. Baseline 0.33–0.47 ms on
  AC; about 0.66 ms on battery (plan 0002, test G).
- p99: the 99th percentile over the 5 s measure window (about 600 frames at 120 Hz).
- Both terms come from the **same** benchmark sweep, hence the same power state and thermal
  history.
- Frame period T = 1/f = 8.33 ms at f = 120 Hz. Decode runs on the video engine (≈ 2.8 ms of
  vcs time per frame) in parallel and is not part of t_video.
- Render time adds to latency whenever it delays the present. That is why the budget is 2 ms
  rather than "whatever fits in 8.33 ms".

**Requirements carried from planning (apply to all plans):**
- Every option lives in the Settings → `moonlight-intel` group. No env-var-only knobs.
- Scaling reporting lives in the stats overlay (Ctrl+Alt+Shift+S).
- Fork code goes in `app/fork/`. Upstream files only get single, marked `// moonlight-intel:`
  hook lines.

**Decisions taken in planning (2026-10-25):**
- **All seven scalers.** They are mutually exclusive per frame: a luma hook makes libplacebo's
  main scaler a no-op, so stacking them is meaningless, but they coexist as options. Cost of
  carrying all of them:
  - binary size, mainly RAVU: about 3 MB of text, about 1.5 MB after rcc's zlib;
  - a first-use compile hitch per scaler;
  - a longer hotkey cycle, which the A/B key offsets.

  Hooks are parsed lazily, so an unused scaler costs nothing at runtime.
- **Hotkey changes are session-only.** Each stream starts from the settings, and hotkeys never
  write them.
- **Feedback:** a 1.5 s toast plus the overlay's Scaling line.
- **Default:** Bilinear until the results PR.
- **Licensing:**
  - The fork is GPL-3.0, inherited from upstream.
  - FSR 1 and NIS ports are MIT (AMD, NVIDIA; ported by agyild). RAVU is LGPL-3.0-or-later
    (bjin). Both licences are GPL-3.0-compatible.
  - Each vendored file keeps its full header and gets a dated "Modified by moonlight-intel"
    line listing the change.
  - `app/fork/shaders/README.md` records the source URL, revision and licence of every file.

### Verified facts

Checked in planning against:
- libplacebo **v7.360.1** (tag commit `cee9b076`, the version on the laptop);
- `bjin/mpv-prescalers` at `b3f0a59d` (2024-01-11);
- agyild's FSR/NIS ports as mirrored in `iwalton3/default-shader-pack` at `9ab0195e`;
- this repo at `intel` `411e602`.

Line numbers are approximate; re-check them.

**libplacebo renderer:**
- `pl_render_fast_params` = `PL_RENDER_DEFAULTS` only (renderer.c L202). So `upscaler`,
  `downscaler`, `plane_upscaler`, `sigmoid_params`, `deband_params` and `dither_params` are all
  NULL.
  - `pl_render_default_params` uses lanczos + sigmoid + dither.
  - `pl_render_high_quality_params` uses ewa_lanczossharp + deband.
- `pass_scale_main()` (renderer.c ≈L1960–2081):
  - upscaling linearises only when `sigmoid_params` is set;
  - **for HDR both linearisation and sigmoidisation are hard-disabled** (L2033–2036: "clips to
    [0,1]" / "very nasty ringing").

  So every scaler, built-in or hook, works on **PQ-encoded** values for HDR streams, and on
  gamma-encoded values for SDR streams (sigmoid stays NULL).
- With `upscaler == NULL` and no hooks, the main scaler is "free sampling" (bilinear texture
  fetch) inside the single fused pass, as measured in 0001/0002.
- **Plane order** (`pass_read_image`, ≈L1719–1780): per plane, deband → film grain → user hooks
  for that plane type (`PL_HOOK_LUMA_INPUT` / `CHROMA_INPUT`).
  - Then every plane is aligned to the reference plane's (possibly hooked) size, with
    `plane_upscaler` (NULL = built-in bilinear).
  - Then YCbCr → RGB, then the main scaler. The main scaler is a no-op when a LUMA hook has
    already produced the output size.
- **Pass descriptions:**
  - user-hook passes are described by their `//!DESC` text (custom_mpv.c L1457);
  - libplacebo's generic scaler passes are described as `"<stage> <dir>scaling (<filter>)"`
    (sampling.c L488);
  - the fast-path samplers `bicubic`, `hermite`, `gaussian`, `bilinear`, `nearest` use bare
    names (sampling.c L297–404). This plan uses none of them.
- **Hook parameters** (`//!PARAM` blocks, custom_mpv.c ≈L690–800 and L1398–1431):
  - `//!TYPE DYNAMIC float` becomes a dynamic shader variable read from `*par->data` each
    frame: a live change with no recompile.
  - `//!TYPE DEFINE` becomes `#define NAME <int>`, re-emitted from `*par->data` each time the
    hook runs. A change produces a new shader text, so the first use of each value compiles
    once (libplacebo caches the pass).
  - `//!TYPE CONSTANT` becomes a Vulkan spec constant. That is **not** usable in file-scope
    `const float` initialisers, which NIS has, so don't use it.
- `pl_hook_par.data` is mutable (custom.h L287). `pl_mpv_user_shader_parse(gpu, text, len)` /
  `pl_mpv_user_shader_destroy()`. Parsed hooks own GPU textures (LUTs), so they must be
  destroyed while the `pl_gpu` is alive.
- `pl_renderer_get_errors()` / `pl_renderer_reset_errors()` (renderer.h L74–81): a hook that
  fails is disabled by signature (`PL_RENDER_ERR_HOOKS`) and rendering continues without it.
- `pl_deband_default_params`: iterations 1, threshold 3, radius 16, grain 4. libplacebo
  rescales the grain itself for HDR sources (renderer.h L172).

**Shaders:**

| File | Hook | Passes | Size condition | Tunables in the original |
|---|---|---|---|---|
| `FSR.glsl` (FidelityFX FSR v1.0.2, agyild port) | `LUMA` | EASU (`//!SAVE EASUTEX`, 1 comp), RCAS | `//!WHEN OUTPUT.w OUTPUT.h * LUMA.w LUMA.h * / 1.0 >`; EASU caps at 2× per axis | `#define SHARPNESS 0.2` (stops: 0 = strongest, 2 = weakest; used only inside RCAS), `#define FSR_PQ 0` in **both** passes ("source has PQ gamma"), `FSR_EASU_DERING 1`, `FSR_EASU_SIMPLE_ANALYSIS 0`, `FSR_EASU_QUIT_EARLY 0`, `FSR_RCAS_DENOISE 1` |
| `NVScaler.glsl` (NIS v1.0.2, agyild port) | `LUMA` | 1 compute pass (`//!COMPUTE 32 24 256 1`) + `coef_scaler` / `coef_usm` textures (2×64 rgba32f) | same `WHEN` as FSR | `#define SHARPNESS 0.25` (0–1; feeds **file-scope `const float`s**), `#define NIS_HDR_MODE 0` ("must be 1 for PQ") |
| `compute/ravu-zoom-ar-r2.hook` (bjin) | `LUMA` | 1 compute pass, `//!OFFSET ALIGN`, 2 LUT textures 18×2592 rgba16f | `//!WHEN HOOKED.w OUTPUT.w < HOOKED.h OUTPUT.h < *` | none (trained weights; anti-ringing variant) |

Rejected in planning:
- `CAS-scaled.glsl`: hard sRGB relinearisation pass, wrong for PQ.
- FSRCNNX, NNEDI3, ArtCNN: 2× only, ≥ 4 ms.
- Temporal upscalers (FSR 2+, XeSS, DLSS): need motion vectors.
- iHD VPP/SFC scaling:
  - moves the cost to the media engine, but needs a VA VPP stage (`VAEntrypointVideoProc`) in
    `vaapi.cpp`, plus an extra surface hop;
  - on Xe2-LPM, SFC is an 8×8 polyphase AVS scaler with P010 in/out
    (`vp_render_sfc_xe2_lpm_base.cpp`, `mhw_sfc.h`);
  - `VA_FILTER_SCALING_HQ` moves scaling to render-engine composition instead (iHD
    `ddi_vp_functions.cpp` ≈L4201);
  - a possible later plan.
- Intel AI super resolution (VPL `mfxExtVPPAISuperResolution`, VA `processing_mode` = HQ):
  - vpl-gpu-rt `c205262` gates it to `>= MFX_HW_PTL` (Panther Lake, Xe3); Lunar Lake is
    `MFX_HW_LNL` < PTL;
  - iHD `53f7ac0` has only the flag, with a TODO, and no consumer on Linux;
  - not available on this target.

**This repo (intel `411e602`):**
- `plvk.cpp`:
  - The constructor (L160–165) copies `pl_render_fast_params` into `m_RenderParams` and
    installs `gpuTimingCallback`.
  - `renderFrame()` calls `logFrameInfo(&mappedFrame)`, then `pl_render_image(…,
    &m_RenderParams)`, then `recordRenderStats(…)` (L1100–1106).
  - The destructor destroys overlay textures inside `if (m_Vulkan != nullptr) {` (L173),
    before `pl_renderer_destroy()` and `pl_vulkan_destroy()`.
  - `isScalingPass()` (≈L1435) matches `"scaling ("` only. Its comment already says plan 0003
    adds hook passes by `//!DESC`.
  - `publishRenderStats()` names the scaler from `m_RenderParams.upscaler`.
- `renderFrame()` runs on the render (pacer) thread. Key events run on the SDL main thread.
- `app/streaming/input/keyboard.cpp` `handleKeyEvent()` (≈L198):
  - Ctrl+Alt+Shift combos are matched on key **down** inside one `if` block: all SDLK
    keycodes first, then scancodes, then `return`.
  - Upstream uses Q Z X S M C D V L E K.
  - A matched combo never reaches the host.
- `app/app.pro`: the fork block (≈L246) must not depend on libplacebo. libplacebo builds set
  `CONFIG += libplacebo` earlier (L131), so the fork block can add a nested `libplacebo { }`
  section. `RESOURCES` is upstream's list at ≈L471 (don't touch it).
- Stats overlay text buffer: 1024 bytes; worst case measured in 0002 was 853.
- Status overlay (`Overlay::OverlayStatusUpdate`): red 36 pt, bottom-left. Upstream uses it for
  the poor-connection warning (session.cpp ≈L191–197) and gamepad mouse mode (≈L1568).
  `OverlayManager` calls are made from non-GUI threads upstream too.
- xe sysfs (kernel doc "Xe GT Frequency Management"): `<device>/tile#/gt#/freq0/act_freq` is
  read-only and world-readable; it reports the actual frequency resolved by PCODE. Throttle
  reasons are under `freq0/throttle/`.

## Scope

**In:**
- Seven scalers with live switching, debanding on/off, FSR and NIS sharpness.
- Vendored shaders, with their provenance and licences.
- Settings group additions.
- Hotkeys and toast.
- Benchmark sweep with power, profile and GT0 tagging.
- Overlay: Scaling line and `GPU state` line.
- Logs.
- `docs/FORK.md` deltas.
- Plan 0002's two open hardware checks, as tests J and K here.

**Out:**
- Choosing the shipped default (results PR).
- Chroma upscaler choice (`plane_upscaler` stays NULL = bilinear).
- Anti-ringing slider, sigmoid upscaling for SDR, dithering for 8-bit swapchains.
- A libplacebo shader cache on disk.
- iHD VPP scaling.
- Any change to decode, pacing or desktop HDR.

## Design

### Files

**New fork files:**

| File | Depends on libplacebo | Purpose |
|---|---|---|
| `app/fork/scaler.h`, `scaler.cpp` | no | Scaler ids and names, the session's live config, hotkeys, toast, benchmark controller, sysfs sampler |
| `app/fork/plvkscaler.h`, `plvkscaler.cpp` | **yes** | Owned by `PlVkRenderer`: lazy hook parsing and destruction, building `m_RenderParams` from the live config, PQ defines, pass classification, error reporting, bench frame feed |
| `app/fork/shaders/FSR.glsl`, `NVScaler.glsl`, `ravu-zoom-ar-r2.hook` | — | Vendored shaders with the edits below |
| `app/fork/shaders/shaders.qrc` | — | `:/fork/shaders/…` resources |
| `app/fork/shaders/README.md` | — | Provenance (URL, revision, date), licence and list of edits per file |

**Changed fork files:**
- `intelpreferences.*`, `IntelSettingsGroup.qml`;
- `fork.h`, `fork.cpp`;
- `renderstats.*`;
- the fork block in `app/app.pro`. Add `scaler.*` to the plain lists. Add a nested
  `libplacebo { SOURCES += fork/plvkscaler.cpp; HEADERS += fork/plvkscaler.h; RESOURCES +=
  fork/shaders/shaders.qrc }`.

**Upstream files**, marked lines only:

| File | Change |
|---|---|
| `app/streaming/input/keyboard.cpp` | `#include "fork/fork.h" // moonlight-intel`, plus **one** line as the first statement inside the Ctrl+Alt+Shift `if` block: `if (Fork::handleStreamHotkey(event->keysym.sym, event->keysym.scancode)) return; // moonlight-intel` |
| `plvk.h` | include `fork/plvkscaler.h`; member `Fork::PlvkScaler m_ForkScaler; // moonlight-intel` |
| `plvk.cpp` | **one** line right before `logFrameInfo(&mappedFrame);`: `m_ForkScaler.apply(m_Vulkan->gpu, m_Renderer, &m_RenderParams, &mappedFrame); // moonlight-intel`. **One** line as the first statement inside `if (m_Vulkan != nullptr) {` in the destructor: `m_ForkScaler.destroy(); // moonlight-intel`. The other changes (`gpuTimingCallback`, `isScalingPass`, `recordRenderStats`, `publishRenderStats`) are in the fork code already at the end of the file |

`m_ForkScaler.destroy()` must run while the GPU is alive. RAII alone would destroy it after
`pl_vulkan_destroy()`, so the line is required; the member destructor then only asserts that
it's empty.

### Scaler list

Persisted as an integer. **Append new values only.** The list order is also the hotkey cycle
order and the benchmark order.

| Id | Settings label | Overlay / log name | Implementation |
|---|---|---|---|
| 0 `SC_BILINEAR` | Bilinear (off) | `bilinear` | `upscaler = NULL`, no hooks |
| 1 `SC_SPLINE36` | Spline36 | `spline36` | `upscaler = &pl_filter_spline36` |
| 2 `SC_LANCZOS` | Lanczos | `lanczos` | `upscaler = &pl_filter_lanczos` |
| 3 `SC_EWA_LANCZOSSHARP` | EWA Lanczos Sharp | `ewa_lanczossharp` | `upscaler = &pl_filter_ewa_lanczossharp` |
| 4 `SC_FSR1` | FSR 1 (EASU + RCAS) | `fsr1` | hook `FSR.glsl`, `upscaler = NULL` |
| 5 `SC_NIS` | NIS (NVIDIA Image Scaling) | `nis` | hook `NVScaler.glsl`, `upscaler = NULL` |
| 6 `SC_RAVU_ZOOM_AR_R2` | RAVU-Zoom-AR r2 | `ravu-zoom-ar-r2` | hook `ravu-zoom-ar-r2.hook`, `upscaler = NULL` |

- `downscaler` and `plane_upscaler` stay NULL for every scaler.
- Hooks are upscale-only by their own `WHEN` conditions, so a smaller window falls back to
  bilinear by itself.

**Sharpness (UI and hotkeys):** one normalised value s ∈ [0, 1] per scaler, where higher means
sharper.
- FSR 1: stops = 2·(1 − s), so s = 0.90 is the shader default of 0.2 stops. Hotkey step 0.05 s
  (= 0.1 stop).
- NIS: percent = round(100·s). The default is s = 0.25, the shader default. Hotkey step 0.05.
- Other scalers have no sharpness. `[` / `]` then toasts "no sharpness for <name>".
- In the live config, s is held as integer hundredths (0–100). Settings values (0–1 doubles)
  are rounded to hundredths when a session starts. Sharpness comparisons, e.g. for the session
  marker, are then exact.

### Vendored shader edits

Copy each file byte-for-byte from the original source first and record its SHA-256 in the
README. Then apply only these edits:
- Every file: after the licence header, add
  `// Modified by moonlight-intel (2026-10): <one-line list of the edits>`.
- `FSR.glsl`:
  - Delete the `#define SHARPNESS …` line and **both** `#define FSR_PQ …` lines.
  - Insert before the first `//!HOOK`:
    ```glsl
    //!PARAM SHARPNESS
    //!DESC RCAS sharpening reduction in stops: 0.0 = strongest, 2.0 = weakest
    //!TYPE DYNAMIC float
    //!MINIMUM 0.0
    //!MAXIMUM 2.0
    0.2

    //!PARAM FSR_PQ
    //!DESC 1 when the source is PQ-encoded; set per frame by moonlight-intel
    //!TYPE DEFINE
    //!MINIMUM 0
    //!MAXIMUM 1
    0
    ```
- `NVScaler.glsl`:
  - Replace `#define SHARPNESS 0.25 …` with `#define SHARPNESS (float(NIS_SHARPNESS_PCT) / 100.0)`.
  - Delete `#define NIS_HDR_MODE 0 …`.
  - Insert before `//!HOOK`:
    ```glsl
    //!PARAM NIS_SHARPNESS_PCT
    //!DESC Sharpness in percent; set by moonlight-intel
    //!TYPE DEFINE
    //!MINIMUM 0
    //!MAXIMUM 100
    25

    //!PARAM NIS_HDR_MODE
    //!DESC 1 when the source is PQ-encoded; set per frame by moonlight-intel
    //!TYPE DEFINE
    //!MINIMUM 0
    //!MAXIMUM 1
    0
    ```
- `ravu-zoom-ar-r2.hook`: the **compute** variant (`compute/ravu-zoom-ar-r2.hook`); no edits
  beyond the header line.
- Keep every `//!DESC` line unchanged. Pass classification depends on them.

### Live config and threading

`scaler.cpp` holds the session's live config, protected by a mutex:
- `{ scaler, sharpness[FSR], sharpness[NIS], debanding, bypass (A/B), generation, session }`.
- `generation` increments on every change.
- `Fork::sessionStarted()` (existing hook) resets the config from `IntelPreferences` and keeps
  a copy of those values as `saved`. Hotkeys change only the live config.
- **Session marker:** `session` is recomputed under the mutex on every change, as
  `scaler != saved.scaler || sharpness[scaler] != saved.sharpness[scaler]`.
  - Only the active scaler's sharpness counts.
  - The marker is a comparison, not a sticky flag. Cycling back to the saved scaler and
    sharpness clears it.
  - A/B bypass and benchmark steps don't set it: they have their own overlay markers. The
    comparison uses the underlying scaler, and during a sweep the remembered config.
  - Debanding can't change in-stream, so it isn't compared.
- `Fork::sessionEnded()` aborts a running benchmark.

`Fork::currentScalerConfig(ScalerConfig*)` returns a copy. `PlvkScaler::apply()`, on the
render thread:
1. Reads the config. If its generation differs from the last applied one:
   - sets `upscaler`, `hooks` / `num_hooks` and `deband_params`
     (`&pl_deband_default_params` or NULL) in `m_RenderParams`;
   - parses the needed hook on first use: text from the qrc, parse time logged;
   - logs `moonlight-intel: scaler <name> sharpness=<s> (<native>) deband=<on|off> pq=<0|1>
     source=<settings|hotkey|bench> session=<0|1>`;
   - asks plvk to restart its stats windows (see Overlay below).
2. Every frame: writes `FSR_PQ` / `NIS_HDR_MODE` = `mappedFrame.color.transfer ==
   PL_COLOR_TRC_PQ`, and the sharpness values, into the hook's `pl_hook_par.data`. This is
   the only writer, on the render thread.
3. After a change, checks `pl_renderer_get_errors()` on the next frame. If the hook was
   disabled:
   - log `moonlight-intel: scaler <name> failed (libplacebo disabled the hook)`;
   - mark it failed for this renderer and fall back to bilinear;
   - the overlay shows `<name> failed -> bilinear`.

A renderer recreated mid-session (window events, the 0002 auto-HDR fallback) reads the same
live config, so hotkey choices survive it.

### Hotkeys

All are Ctrl+Alt+Shift+…, key down, consumed (never sent to the host):

| Key | Action | Toast |
|---|---|---|
| U | next scaler (wraps), clears A/B | `Upscaler: FSR 1, sharpness 0.90` |
| Y | previous scaler | same |
| B | A/B: toggle between the current scaler and bilinear | `A/B: bilinear` / `A/B: FSR 1` |
| ] | sharper (+0.05, clamped) | `FSR 1 sharpness 0.95` |
| [ | softer (−0.05, clamped) | same |
| T | start / abort the benchmark sweep | `Benchmark 1/7: bilinear (keep content moving)` … `Benchmark done: see log` |

Scaler and sharpness toasts end with ` (session)` while the session marker is set, e.g.
`Upscaler: Lanczos (session)`.

**Matching** mirrors upstream:
- Check the SDLK keycode first (`SDLK_u`, `SDLK_y`, `SDLK_b`, `SDLK_RIGHTBRACKET`,
  `SDLK_LEFTBRACKET`, `SDLK_t`).
- Fall back to the scancode **only when `sym` is not an ASCII letter or bracket**, i.e. on
  non-Latin layouts. Otherwise a Dvorak user's physical-B key (sym `x`) would be stolen from
  upstream's fullscreen combo.

**Without plvk:** when no fresh plvk stats snapshot exists (EGL renderer), every key toasts
`Upscaler needs the Vulkan renderer` and does nothing.

### Toast

- Uses `Overlay::OverlayStatusUpdate` for 1.5 s, via `Session::get()->getOverlayManager()`.
- Skipped when that overlay is already enabled (connection warning or gamepad mouse mode). The
  overlay line still updates.
- Hidden by an `SDL_AddTimer` callback, only if the overlay text is still the toast's text, so
  a connection warning that appeared meanwhile isn't cleared.
- A new toast replaces the pending one.

### Benchmark sweep

- **Start:** Ctrl+Alt+Shift+T. Remember the current live config, then step through the seven
  scalers in list order with debanding **off** and default sharpness.
- **Per step:** 1 s settle (discarded), then 5 s measure, about 42 s in total. `PlvkScaler`
  feeds each frame to `Fork::benchRecordFrame(generation, videoNs, scalerNs, debandNs)`.
  - Frames whose generation isn't the current step's are ignored.
  - Steps advance from that call (render thread, `SDL_GetTicks`). With no frames, the sweep
    waits; it doesn't time out.
- **System state** comes from an **SDL timer at 4 Hz, never the render thread**. It is started
  in `sessionStarted()` and stopped in `sessionEnded()`. It reads:
  - power source: any `/sys/class/power_supply/*/type == Mains` with `online == 1` → `AC`,
    else `battery`;
  - battery power: `BAT*/power_now` (µW), or `current_now·voltage_now`; logged only on battery,
    because on AC it is the charge rate;
  - `/sys/firmware/acpi/platform_profile`;
  - GT0 `act_freq`: card = the `/sys/class/drm/card*` whose `device/driver` resolves to `xe`,
    path `device/tile0/gt0/freq0/act_freq`;
  - the union of `freq0/throttle/reasons` over the window, if present.

  A missing file reads `n/a`. Never fail.
- **Log**, one line per step:
  ```
  moonlight-intel: bench 5/7 scaler=fsr1 sharpness=0.90 power=battery profile=balanced frames=598 video avg=0.98 p99=1.12 max=1.30 ms scaler avg=0.52 ms gt0=1100 MHz battery=8.4 W throttle=none
  ```
  Then one summary line per scaler:
  ```
  moonlight-intel: bench-summary scaler=fsr1 dp99=+0.59 davg=+0.51 ms vs bilinear
  ```
  Plus the pass list once per step: the existing `gpu-pass[i]` format, logged regardless of the
  log setting.
- **Flags:** a step with fewer than 540 measured frames (90 % of 600) is flagged `lowframes`.
  The host sends frames only on change, so content must keep moving.
- **End or abort:** restore the remembered config and toast. Abort on T, at session end, or
  when the renderer changes to EGL.

### Overlay

- **Scaling line**, extended from the 0002 format:
  `Scaling: 1920x1200 -> 2880x1800 (x1.50), fsr1 s0.90[ (session)], 0.52 ms[, deband 0.21 ms][, A/B][, bench 5/7]`.
  - `(session)`: the scaler or its sharpness differs from the saved settings (see Live config).
    So you can tell when a hotkey choice won't carry to the next stream.
  - Failure: `… (x1.50), fsr1 failed -> bilinear`.
  - Downscale and 1:1 keep the 0002 wording.
  - `RenderStatsSnapshot` gains `debandAvgMs`, a wider `scaler[40]` holding the formatted name
    and parameters, `session`, `bypass`, `benchStep` and `benchTotal`. plvk copies `session`
    from the config it last applied, so the marker always matches the scaler on screen.
- **New line** after `GPU render (video)`:
  `GPU state: AC, balanced, GT0 1450 MHz` / `GPU state: battery 8.4 W, low-power, GT0 1100 MHz`.
  It comes from the sampler and is shown whenever a stream is active, whichever renderer is in
  use.
- **Pass buckets** (fork code in plvk):
  - scaler passes: `"scaling ("` steps, plus any pass whose steps include one of the active
    hook's `//!DESC` strings. `PlvkScaler` extracts the `//!DESC` lines from the shader text it
    parsed; `PlvkScaler::isScalerPass(shader)`.
  - deband passes: step `"debanding"`.
  - Both buckets stay inside t_video. They are breakdowns, not exclusions.
- **On a config change**, plvk clears the 1 s stats window and the 5 s log window and re-arms
  the one-time `gpu-pass` list, so numbers never mix two scalers.
- **Buffer:** re-measure the worst-case overlay text with all suffixes (session, A/B, bench,
  deband, battery) and report it in Execution notes. It must stay ≤ 1024 bytes, with fork lines
  appended whole as in 0002.

### Settings group (`IntelSettingsGroup.qml`, `IntelPreferences`)

New persisted keys in `[moonlight-intel]`, write-through like 0002:

| Key | Type / default | Control |
|---|---|---|
| `upscaler` | int (`ScalerId`), `SC_BILINEAR` | ComboBox "Upscaler", all seven labels |
| `fsrSharpness` | double 0–1, 0.90 | Slider "FSR 1 sharpness", visible when FSR 1 is selected |
| `nisSharpness` | double 0–1, 0.25 | Slider "NIS sharpness", visible when NIS is selected |
| `debanding` | bool, false | CheckBox "Debanding (libplacebo)" |

Below them, a small label lists the hotkeys (U/Y cycle, B A/B, [ ] sharpness, T benchmark) and
notes that hotkey changes last for one stream.

Tooltips:
- the combo: upscaling applies only with the Vulkan (libplacebo) renderer, i.e. HDR streams or
  "Use Vulkan for SDR streams";
- EWA Lanczos Sharp: the most expensive.

## Steps

1. **Check out and re-check.**
   - Check out `claude/0003-upscaler-hotkeys`. Its first commit is this plan; the second sets
     plan 0002 to Done.
   - Re-check the Verified facts against the laptop's libplacebo (`pkg-config --modversion
     libplacebo`) and its headers.
   - Fetch the three shaders from their original sources (agyild's gists for FSR/NIS; bjin's
     repo for RAVU) and diff them against the mirror revisions above. Note any difference.
   - On the laptop:
     - `ls /sys/class/drm/card*/device/tile*/gt*/freq0/act_freq`
     - `readlink -f /sys/class/drm/card*/device/driver`
     - `ls /sys/class/power_supply/`
     - `cat /sys/firmware/acpi/platform_profile`

     Confirm that `gt0` is the render GT by reading `gt0` and `gt1` `act_freq` during a stream;
     the render GT should match gputop's GT0.
2. **Vendor the shaders.** Add the shaders, `shaders.qrc` and `README.md` (with SHA-256 before
   and after edits), and the `app.pro` fork-block entries. Build.
3. **Settings.** Add the `IntelPreferences` properties, then the settings group controls.
4. **Core.** Implement `scaler.{h,cpp}`: config, hotkeys, toast and sampler. Add the
   `fork.h` / `fork.cpp` entry points and the `keyboard.cpp` hook line.
5. **plvk.** Implement `plvkscaler.{h,cpp}` and the two plvk hook lines. Adapt the fork code in
   plvk: pass buckets, window resets on config change, scaler name in the snapshot.
6. **Overlay.** Add the snapshot fields and the overlay formatting, then measure the overlay
   buffer.
7. **Benchmark.** Implement the controller and its log lines.
8. **Headless check on the laptop** (same approach as the 0002 headless libplacebo run):
   - Parse all three shaders.
   - Render a synthetic 1920×1200 P010 PQ BT.2020 frame to a 2880×1800 offscreen PQ target with
     each scaler, then with each hook's PQ define at 0 and 1.
   - Log the pass descriptions, GPU times and `pl_renderer_get_errors()`.
   - Repeat for an 8-bit BT.709 frame to an sRGB target.
   - Record the table in Execution notes; it should be ≈ consistent with the benchmark.
9. **Harnesses** (scratch, not committed):
   - hotkey matching: SDLK first, scancode only for non-Latin syms, upstream combos untouched;
   - config generations and A/B semantics;
   - the session marker: set by a scaler or sharpness change, cleared when the saved values
     return, unaffected by A/B and benchmark steps, and reset at session start;
   - benchmark sequencing with a fake clock: settle discard, generation filtering, restore on
     abort;
   - sysfs readers against a fake tree with missing files.
10. **Docs.**
    - `docs/FORK.md`: D6 (upscaler: settings, plvk hooks, shaders and licences) and D7
      (hotkeys, toast, benchmark, `GPU state` line), with merge notes.
    - A licence paragraph under the Upstream base section.
    - `docs/arch.md`: running the benchmark (AC vs battery, moving content).
    - `CLAUDE.md` code map: `app/fork/` and `app/fork/shaders/`.
11. **Build and PR.** Build on Arch, plus a scratch `CONFIG+=disable-libplacebo` build. No new
    warnings in touched files. Fill in Execution notes, set the status, open the PR.

## Acceptance criteria

- [x] Builds cleanly on Arch, and with `CONFIG+=disable-libplacebo` (without libplacebo:
      no plvkscaler and no shader resources; hotkeys toast "needs Vulkan").
- [x] No new warnings in touched files.
- [x] Upstream files touched only by the listed marked lines: keyboard.cpp 2, plvk.h 2,
      plvk.cpp 2 new in upstream code (the fork code at the end of plvk.cpp may change).
- [ ] All seven scalers render on an HDR (PQ) and an SDR stream with no libplacebo errors.
      The log shows the `scaler … pq=1` / `pq=0` lines. (Headless: yes, both. Stream: tests A, G.)
- [ ] Hotkeys U/Y/B/[/]/T behave as specified, never reach the host, and don't alter upstream
      combos.
- [ ] Hotkey changes don't persist. Settings changes do, and apply at the next stream.
- [x] The Scaling line and toasts show `(session)` exactly while the scaler or its sharpness
      differs from the saved settings. Returning to the saved values clears it. A/B and
      benchmark steps don't set it. Covered by the config harness (step 9).
- [ ] The overlay shows the active scaler, its sharpness, scaler ms, deband ms (when on) and the
      `GPU state` line. Worst-case overlay text ≤ 1024 bytes (value recorded: 955).
- [x] The benchmark sweep logs 7 `bench` lines + 7 `bench-summary` lines and restores the prior
      config. Aborting with T restores it too. (Stream, test E on battery: 7 + 7 lines, restored
      bilinear. The abort was checked by the harness only.)
- [x] Headless table recorded in Execution notes.
- [x] `app/fork/shaders/README.md` lists source, revision, SHA-256, licence and edits for each
      shader. Vendored headers intact.
- [x] `docs/FORK.md` D6/D7 and the licence paragraph; `docs/arch.md` and `CLAUDE.md` updated.
- [ ] Hardware test results recorded (user).

## Hardware test (user runs on the laptop)

```bash
# 1. Build (starts at the repo root, so it works from any directory)
cd ~/repos/moonlight-intel
git switch claude/0003-upscaler-hotkeys && git pull
git submodule update --init --recursive
mkdir -p build && cd build && qmake6 ../moonlight-qt.pro && make -j"$(nproc)" release

# 2. Run (works from any directory)
~/repos/moonlight-intel/build/app/moonlight 2>&1 | tee /tmp/ml-0003.log
```

**Stream settings:**
- 1920×1200, 120 fps, AV1, HDR on, V-Sync on, frame pacing on, fullscreen.
- **Until plan 0004 is merged, turn frame pacing off** for tests A–E and H. With it on, a
  Wayland commit race in upstream's frame pacing disconnects the stream within seconds once
  the GPU is busy (EWA, RAVU; see Execution notes).
- Moving content (scrolling UFO test), plus a game scene with foliage or smoke for the visual
  checks.
- Host in HDR.
- Settings → `moonlight-intel`: desktop HDR "During HDR streams", Upscaler "Bilinear (off)",
  debanding off.
- **Start on AC.** Stats overlay on (Ctrl+Alt+Shift+S).

**Tests:**
- **A. Cycle (U, then Y back):**
  - Every press shows a toast. The Scaling line names the scaler and shows its ms, and the
    `GPU state` line shows `AC`.
  - Expect at most one hitch on the first use of each scaler.
  - Check: no colour cast, no brightness shift, no image offset against bilinear (use B).
    Look closely at FSR 1 on fine, high-contrast detail (foliage, text): in the headless check
    its PQ mode moved such detail by up to 2 % of the code range, while smooth areas matched
    bilinear.
  - Fill in the subjective table below on the game scene.
- **A2. Visual recheck after deviation 10** (done 2026-10-26: the Lanczos family looks a lot
  better):
  - Capture the same crop as before for Lanczos / EWA. Their ringing should be clearly smaller
    than in the first captures.
  - NIS is as in the first captures; try it at 0.00–0.10 on HDR.
- **B. A/B:** on FSR 1, press B repeatedly. It toggles bilinear ↔ FSR 1 instantly, with no
  hitch after the first time.
- **C. Sharpness:**
  - On FSR 1, press `]` and `[` several times: the value changes in 0.05 steps, with no hitch.
  - On NIS, the same, with possibly a single-frame hitch the first time each value is used.
  - On Spline36, `[` toasts "no sharpness".
- **D. Benchmark on AC:** with the UFO test scrolling, press Ctrl+Alt+Shift+T and keep the
  content moving for about 45 s until "Benchmark done".
- **E. Benchmark on battery:** unplug, wait about 60 s, and repeat D. The `GPU state` line must
  show `battery … W`.
- **F. Debanding:**
  - Quit the stream. Tick Debanding in the settings and start a stream: the Scaling line gains
    `deband … ms`.
  - Look at a dark gradient (sky, fog) with B toggling.
  - Untick it afterwards.
- **G. SDR stream:** untick *Enable HDR* in Basic Settings, stream, and cycle to FSR 1 and NIS.
  The log shows `pq=0`, and the overlay shows `Output: … BT.1886`. Re-tick HDR afterwards.
- **H. Settings persistence:**
  - Pick FSR 1 in the settings, set FSR sharpness to 0.80, and restart Moonlight. The stream
    starts on FSR 1 (`source=settings` in the log), with no `(session)` on the Scaling line.
  - Press `]`: `fsr1 s0.85 (session)`. Press `[`: back to `fsr1 s0.80`, no marker.
  - Press U: `nis s0.25 (session)`. Press B: `bilinear, A/B`. Press B again: back to
    `nis s0.25 (session)`.
  - Quit and start a new stream: it starts on FSR 1, s0.80, with no marker.
  - Set the setting back to Bilinear.
- **I. Hotkeys don't leak:** on the host, focus a text editor and press each fork hotkey. No
  characters may appear. Also check that the upstream combos (S, X, Z, M) still work.
- **J. EGL (0002 test G, second half):**
  - Untick *Enable HDR* and "Use Vulkan (libplacebo) renderer for SDR streams", and stream.
  - The log shows `Renderer 'EGL/GLES' …`, and U toasts "Upscaler needs the Vulkan renderer".
  - Re-tick both afterwards.
- **K. 0001 Run C (carry-over from 0002 test F):**
  - Set desktop HDR to Off, put GNOME in SDR Native, and start an HDR stream.
  - Report whether the second `frame` line reads `BT.2020/PQ hdr=on`.
  - Restore "During HDR streams".

**Collect (no IPs or hostnames):**
```bash
grep -E "moonlight-intel:|Renderer '" /tmp/ml-0003.log
```

**Results to fill in:**

| Scaler | AC video p99 | AC Δp99 | battery video p99 | battery Δp99 | GT0 AC / battery (MHz) | battery W | Visual (foliage/smoke, 1–5) |
|---|---|---|---|---|---|---|---|
| bilinear | 0.53 | 0 | 0.46 | 0 | 800 / 800 | 9.0 | reference |
| spline36 | 4.23 | +3.70 | 4.25 | +3.79 | 1107 / 1165 | 10.3 | 2nd best |
| lanczos | 4.21 | +3.68 | 3.79 | +3.33 | 1003 / 1530 | 12.7 | minor gain for the cost |
| ewa_lanczossharp | 8.19 | +7.66 | 8.24 | +7.78 | 1950 / 1950 | 15.1 | minor gain for the cost |
| fsr1 (s 0.90) | 3.41 | +2.89 | 3.75 | +3.30 | 1345 / 1204 | 17.0 | best overall, at s0.70 |
| nis (s 0.25) | 3.28 | +2.75 | 3.27 | +2.81 | 800 / 800 | 16.6 | best at s0.00, still some ringing |
| ravu-zoom-ar-r2 | 5.98 | +5.45 | 5.86 | +5.40 | 1215 / 850 | 13.9 | natural (first check) |

**Expected:**
- Every scaler renders without errors on HDR and SDR.
- Hook scalers and Spline36/Lanczos meet Δp99 ≤ 2 ms on AC. Battery may exclude EWA, and
  possibly more; that is a result, not a failure.

## Risks and upstream-merge impact

- **`keyboard.cpp`** is upstream-busy around the combo block. The hook is one line at the top
  of that `if`; on conflict, re-insert it as the first statement of the Ctrl+Alt+Shift block.
- **plvk.cpp:** two new marked lines, at the `logFrameInfo` anchor and in the destructor's
  `m_Vulkan` block. Everything else is fork code already at the end of the file.
- **libplacebo `//!PARAM` semantics:** verified at v7.360.1. Re-check after a libplacebo update
  (step 8 of a future sync).
- **Hitches:** parsing RAVU (about 3 MB of hex LUTs) and compiling each hook's pipeline cost
  one or more frames on first use. Acceptable for A/B. A shader cache can come later.
- **Quality on PQ:** RAVU is trained on SDR gamma content. FSR/NIS PQ modes follow the ports'
  instructions. The headless check and test A catch casts or shifts.
- **Toast vs. upstream status overlay:** the fork never hides a message it didn't set.
- **Licences:** MIT and LGPL-3.0 into GPL-3.0 are compatible. The obligation is to keep
  notices, mark modifications and ship source; the public repo does the latter.

## Execution notes

Executed 2026-10-26 on the target laptop (Arch, libplacebo 7.360.1, Mesa 26.2.5).

**Step 1 re-checks.**
- The Verified facts hold at libplacebo v7.360.1 (`cee9b076`): fast params are
  `PL_RENDER_DEFAULTS`, HDR hard-disables linearisation and sigmoid in `pass_scale_main()`, and
  the `DYNAMIC` / `DEFINE` / `CONSTANT` handling is as described. A hook's signature is a hash of
  its shader text (custom_mpv.c L1611), so "disabled by signature" can be matched against the
  parsed hook.
- New fact: libplacebo starts parsing at the **first `//!` anywhere in the text**
  (custom_mpv.c L1627), not at the first line that starts with one (see Deviations 1).
- Shaders: agyild's gists at their latest revisions (FSR `2623d743`, 2022-03-06; NIS
  `05f00864`, 2022-02-24) are byte-identical to the mirror at `9ab0195e`. RAVU matches bjin's
  blob at `b3f0a59d`.
- sysfs: one xe card (`card0`). `gt0` has the rcs/ccs/bcs engines and `gt1` has vcs/vecs, so
  `gt0` is the render GT. The in-stream comparison with gputop (test A) still applies.
  `freq0/throttle/reasons` reads `none`. Supplies: `AC` (Mains), `BAT0` (Battery: `power_now`,
  no `current_now`), two ucsi USB supplies (ignored). Platform profiles: low-power, balanced,
  performance.
- Read cost: `BAT0/power_now` takes up to 14 ms about once a second (ACPI refresh), and
  `AC/online` about 1.3 ms (see Deviations 3).

**Headless check (step 8).** Harness, not committed: the real `PlvkScaler`, `scaler.cpp`,
`systemstate.cpp` and rcc'd shaders render a synthetic 1920×1200 frame to a 2880×1800 offscreen
target. The frame has a ramp, a zone plate, a checkerboard and thin lines. Frames are paced at
120 Hz; 1 s settle, then 5 s (601 frames) per row. Conditions: on AC, profile balanced, GT0
running at 1347 MHz on average. Δ smooth and Δ detail: mean output change against bilinear in
the ramp and the zone-plate area, in 10-bit code values. Every row had no render errors (0x0, no
disabled hooks), and the best match against bilinear was at offset 0,0, so no image shift.

| Scaler | P010 PQ → PQ avg / p99 ms | Δp99 | NV12 BT.709 → sRGB avg / p99 ms | Δp99 | Δ smooth PQ / SDR | Δ detail PQ / SDR |
|---|---|---|---|---|---|---|
| bilinear | 0.69 / 0.73 | 0 | 0.66 / 0.69 | 0 | 0 / 0 | 0 / 0 |
| spline36 | 3.24 / 4.19 | +3.46 | 3.54 / 4.25 | +3.56 | −0.4 / −0.5 | −0.1 / −0.2 |
| lanczos | 4.22 / 4.25 | +3.52 | 4.23 / 4.25 | +3.56 | −0.4 / −0.5 | −0.1 / −0.2 |
| ewa_lanczossharp | 7.69 / 8.76 | +8.03 | 7.75 / 8.92 | +8.23 | −0.4 / −0.4 | 0.0 / 0.0 |
| fsr1 s0.90 | 3.15 / 4.11 | +3.38 | 3.00 / 3.87 | +3.18 | −0.3 / −1.1 | **−20.6** / 0.0 |
| nis s0.25 | 3.62 / 3.96 | +3.24 | 2.69 / 3.60 | +2.90 | −0.2 / −0.3 | −10.3 / −4.1 |
| ravu-zoom-ar-r2 | 3.99 / 5.40 | +4.68 | 4.22 / 6.89 | +6.20 | −0.2 / −0.3 | −0.1 / −0.1 |

- PQ define swapped (deliberately wrong): FSR with `FSR_PQ=0` on PQ gives Δ detail 0.0, and
  `FSR_PQ=1` on SDR gives −20.4. NIS mode 0 on PQ gives −4.1, mode 1 on SDR −10.3. So the large
  detail shifts come from the shaders' PQ modes. FSR's PQ mode runs EASU and RCAS on pow(x, 4)
  and returns with the fourth root. NIS's HDR mode only changes its sharpening thresholds.
  Smooth areas match bilinear within ~1 code value in every case: no colour cast and no
  brightness shift. Test A asks for a look at FSR on fine detail.
- Passes (PQ, one frame):
  - fsr1: EASU 1.08 + RCAS 0.51 + main 0.82 ms;
  - nis: 2.73 + 0.87 ms;
  - ravu: 3.12 + 0.86 ms;
  - spline36 / lanczos: three passes (two `ortho (horiz)`, one `ortho (vert)`);
  - ewa_lanczossharp: two `polar upscaling` passes, plus a 1 ms pass without a description,
    which counts in the video time but not the scaler time.
- Debanding: bilinear + deband 1.50 ms (deband passes 0.69); fsr1 + deband: deband passes
  0.51 ms. Debanding fused with colour decoding counts as deband, so the deband ms is an upper
  bound.
- Parse times: FSR < 0.1 ms, NIS 0.1–1 ms, RAVU 10–12 ms, plus pipeline compiles on first
  render.
- Run-to-run spread is large (e.g. spline36 PQ 2.53 vs 3.24 ms avg in two runs): GPU clock
  scaling at a 120 Hz duty cycle. Headless bilinear is 0.69 ms where 0002's AC streams measured
  0.33–0.47 ms, so headless numbers run high. **The in-stream sweep (tests D, E) is the
  measurement**. Headless, every non-bilinear scaler misses Δp99 ≤ 2 ms.
- GT0 `act_freq` sampled at 4 Hz while rendering at 120 Hz: 47 % of samples read 0 (RC6
  between frames).

**First hardware results (user, battery, 2026-10-26).** Spline36, Lanczos, FSR 1 and NIS
rendered; RAVU (pass 6.8–7.1 ms, video p99 8.22 ms of 8.33) and EWA Lanczos Sharp (video p99
8.25 ms) stuttered, and both times the stream died about 5 s later with `Wayland display
connection closed by server (fatal)`.
- Not a GPU fault: the kernel log is clean. GNOME logged `error in client communication`.
- A standalone repro of plvk's setup (fullscreen SDL window, libplacebo FIFO swapchain of depth
  1, HDR10, EWA at 120 Hz) gave the cause:
  - Without upstream's `WaylandVsyncSource` it ran for 25 s.
  - With it, it died within 1–3 s at any load: `wp_linux_drm_syncobj_surface_v1` error 3,
    "Release or Acquire point set but no buffer attached".
  - The vsync source's `wl_surface_commit()` (main thread) lands between Mesa's
    `set_acquire_point` / `set_release_point` and its `attach` (render thread, inside
    `vkQueuePresentKHR`).
- Mesa 26 uses explicit sync with Mutter 51, which makes that interleaving fatal. In real
  streams the render thread mostly presents after the vsync source's commit; heavy GPU work
  lines both up on the same vblank, so the race hits within seconds.
- This is an upstream bug in frame pacing, not in this plan's code, so the fix is plan 0004
  (a commit lock shared by the vsync source and plvk's present; 25 s clean in the repro). An
  overload guard was considered and dropped.
- Workaround until 0004 is merged: frame pacing off (upstream then creates no vsync source).

**Test E, battery sweep (user, 2026-10-26).** Run on the local merge with plan 0004, with
frame pacing on. All 7 steps measured at least 540 frames (EWA 544, the rest 600–601), so none
was flagged `lowframes`. The table under Hardware test has the battery columns; the AC sweep
(test D) is still to do. Two things to keep in mind when reading it:
- **Nothing meets Δp99 ≤ 2 ms on battery.** The cheapest is NIS at +2.81.
- **The GT0 clock moves with the load.** It was 800–1950 MHz per step, under the `power_saving`
  SLPC profile, so a step's time mixes the scaler's cost with the clock the firmware chose.
  - Lanczos measured faster than Spline36 only because it ran at 1530 MHz versus 1165.
  - GPU time × clock is closer to the real cost (ms·GHz): bilinear 0.35, nis 2.6, fsr1 3.4,
    spline36 3.6, lanczos 4.0, ravu 4.9, ewa 14.3. This is approximate, since memory-bound
    passes don't scale linearly with the clock.
  - The results PR should compare clocks before comparing scalers. Repeated sweeps help.

**Visual check (user, 2026-10-26).** The user captured the same TestUFO crop with each scaler,
plus a native 2880×1800 stream. Native needed a different host display scale, so it isn't
pixel-aligned with the others. FSR 1 s0.90 and RAVU looked the most natural; Spline36, Lanczos
and EWA rang; NIS s0.25 overdid the sharpening.

*Measured on the captures.* The seven upscaled crops are pixel-aligned. A halo is a pixel beyond
the local range of the bilinear crop (7×7) next to an edge.

| Scaler | Dark halos, avg | Edge pixels with a halo > 8 levels | Gradient vs bilinear |
|---|---|---|---|
| ravu-zoom-ar-r2 | 0.24 | 8.5 % | 1.22 |
| fsr1 s0.90 | 0.55 | 8.6 % | 1.21 |
| spline36 / lanczos / ewa | 0.87–1.00 | 10.8–13.1 % | 1.24–1.26 |
| nis s0.25 | 4.12 | 36.8 % | 1.64 |

*Ground-truth test (scratch harness, the real `PlvkScaler`).*
- **Method:** `native.png` was cropped to 726×405 and downscaled 1.5× (box, and separately
  bicubic) to a 484×270 synthetic source. It was encoded like the stream (8-bit BT.709 4:2:0,
  and 10-bit BT.2020 PQ 4:2:0 with white at 203 nits), upscaled with each configuration and
  compared with native.
- **Results, robust across both sources and both transfers:**
  - **RAVU is the closest to native** in every case (SSIM 0.910 / 0.948 on PQ; halos 1.4–2.8 %).
  - **NIS on PQ** oversharpens at every setting. At s0.25 it scores below bilinear in SSIM with
    39–40 % halo pixels. Even s0.00 has 22–24 %. On SDR input, NIS is best at s0.10–0.15.
  - *Wrong lead, corrected below:* in this sRGB comparison, NIS with `NIS_HDR_MODE` forced to 0
    on PQ input looked much better (s0.00: 5–6 % halo).
  - **FSR on PQ:** halos fall with sharpness (s0.90 3–9 %, s0.80 2–7 %, s0.70 0.6–6 %) for
    almost no loss in gradient (0.92 → 0.90 of native).
    - With `FSR_PQ` forced to 0 it is sharper but rings much more (s0.90: 19–22 %). So its PQ mode
      is what makes it look clean.
  - **Spline36 / Lanczos / EWA** carry 6–8 % halo pixels. libplacebo's
    `pl_filter_config.antiring = 0.8` cuts that to 1–2.6 % at about the same SSIM.
- **Decided with the user afterwards** (Deviation 10): the three libplacebo filters use
  `antiring = 0.8`.
  - The image harness confirmed the shipped code gives exactly the measured rows: `lanczos` =
    `lanczos-ar0.8`.
  - A headless A/B (on AC) found no measurable GPU cost for anti-ringing: on and off alternated
    within the clock noise.
- **NIS's SDR mode on PQ was tried and reverted.** The user's recheck (test A2, s0.25) showed
  more ringing than before.
  - The sRGB comparison had clipped everything above the page's white, which hid NIS's bright
    fringes. Those are the ones that stand out on an HDR panel.
  - The reason is NIS's luma ramp. In HDR mode, sharpening fades out between PQ 0.35 and 0.55
    (white text, ~160 nits and up, gets little). In SDR mode it fades between 0.45 and 0.90, so
    bright PQ text keeps about ⅔ of full strength and a higher limit.
  - The same renders, measured in 10-bit PQ code values without clipping, at page white 203 and
    400 nits:

| NIS on PQ | Halo px | Over-white px | Peak over white |
|---|---|---|---|
| s0.25, HDR mode | 46 % / 39 % | 1.0 % / 1.0 % | 113 / 126 |
| s0.25, SDR mode | 45 % / 44 % | 3.3 % / 2.8 % | 202 / 198 |
| s0.10, HDR mode | 38 % / 33 % | 0.8 % / 0.8 % | 81 / 91 |
| s0.10, SDR mode | 31 % / 31 % | 1.4 % / 1.3 % | 126 / 123 |
| s0.00, HDR mode | 33 % / 30 % | 0.8 % / 0.8 % | 81 / 91 |
| s0.00, SDR mode | 18 % / 19 % | 0.8 % / 0.9 % | 85 / 93 |

  - In the same metric, every other scaler stays at or below 2.9 % halo, except FSR s0.90 at
    17–18 %, mostly dark, and s0.70 at 6–10 %. Over-white pixels stay at or below 0.02 %.
  - So NIS keeps its HDR mode on PQ, as the port specifies. On HDR, sharpness 0.00–0.10 is the
    least bad setting, and NIS remains the scaler that rings most.
- **Caveats:** the source is a downscaled SDR screenshot of text, not a host render at
  1920×1200; metrics are on luma only (the hooks scale luma, chroma is bilinear); games may
  rank differently.

**Test D, AC sweep (user, 2026-10-26).** Run on the local merge with plan 0004, with frame
pacing on, after deviation 10's revert. 7 + 7 lines, config restored, no disconnect. All steps
at least 540 frames (EWA 574). The AC columns are in the results table.
- **AC barely differs from battery here:** bilinear 0.47 vs 0.44 ms avg, and GT0 again
  800–1950 MHz per step. 0002's AC/battery gap did not show up in these runs.
- **GPU time × clock (ms·GHz) is consistent between the two sweeps:** bilinear 0.38 / 0.35,
  nis 2.5 / 2.6, fsr1 3.3 / 3.4, spline36 3.6 / 3.6, lanczos 3.8 / 4.0, ravu 5.1 / 4.9,
  ewa 13.2 / 14.3. So it is a usable cost measure despite the clock swings.
- **No scaler meets Δp99 ≤ 2 ms on AC either.** Closest are NIS (+2.75) and FSR 1 at s0.90
  (+2.89; +1.98 ms avg).

**User ranking after the anti-ringing change (tests A / A2, 2026-10-26).**
1. **FSR 1 at sharpness 0.70.** In the PQ-space metric, s0.70 has 5.5–9.6 % halo pixels against
   16.7–18.5 % at the default 0.90.
2. **Spline36.**
3. **NIS** at sharpness 0.00 was the best of its range, still with some ringing.
4. **Lanczos and EWA Lanczos Sharp** cost a lot for a minor improvement.

**Findings for the results PR.**
- GT0's SLPC power profile (`tile0/gt0/freq0/power_profile`) was `[power_saving]` on battery and
  `[base]` after the charger was connected, during this session. It likely explains much of the
  AC/battery GPU-time gap from 0002. On request it is not logged by this plan.
- FSR's PQ mode on fine HDR detail (above): worth an A/B look before choosing FSR as the
  default.
- **Candidate defaults from the user's ranking:** FSR 1 with sharpness 0.70. That is outside the
  2 ms budget (about +2.9 ms p99 at s0.90 on both power sources), so the results PR has to accept
  that, or revisit the budget.

**Harnesses (step 9),** scratch, against the real sources:
- logic, 124 checks: hotkey matching on QWERTY / Dvorak / AZERTY / Cyrillic through
  `keyboard.cpp`'s order (fork, then upstream keycodes, then upstream scancodes); config
  generations and A/B; the session marker (set by a scaler or sharpness change, cleared on
  return, untouched by A/B and bench steps, reset at session start); the benchmark with a fake
  clock (settle discarded, stale generations ignored, waits without frames, `lowframes`, 7 + 7
  lines, restore on done / T / renderer gone / stream end).
- sysfs, 31 checks: fake trees with files missing one by one, garbage contents, a non-xe card,
  no supplies at all; RC6 zeros; union of throttle reasons; the sampler thread on the real
  `/sys`.
- An offscreen load of `IntelSettingsGroup.qml` with the real `IntelPreferences` (scratch
  config directory): 0 QML warnings, and the sliders show for FSR 1 / NIS only.

**Buffer.** Worst-case overlay text is 955 of 1024 bytes: upstream 572 (from 0002) plus fork 383,
with every suffix at once, the widest numbers and a desktop-HDR failure reason. The longest line
is 79 characters.

**Builds.** The Arch build is clean, apart from upstream's two `lock_queue` / `unlock_queue`
deprecation warnings in `plvk.cpp`, which were there before. The scratch
`CONFIG+=disable-libplacebo` build is clean, with 0 warnings and no `plvkscaler` or
`qrc_shaders` objects.

**Deviations.**
1. **Shader header lines.** The first "Modified by" lines mentioned `//!PARAMs`, and FSR / NIS
   failed to parse ("Missing variable type!"; libplacebo starts at the first `//!` anywhere). They
   now say "PARAM blocks". The shader README warns about it.
2. **Compression.** rcc only stores a compressed copy that saves at least 70 %. RAVU's hex LUTs
   save about 55 %, so the plan's "about 1.5 MB after rcc" didn't happen (3.0 MB). The qrc sets
   `threshold="30"`, giving 1.4 MB.
3. **Files and threads.**
   - Toasts and the sysfs sampler are in their own files, `toast.*` and `systemstate.*`, so
     `scaler.cpp` has no `Session` dependency and the parts can be tested alone.
   - The sampler runs on its own thread at 4 Hz, not an SDL timer: SDL runs every timer on one
     thread (upstream's gamepad mouse emulation polls every 10 ms), and the ACPI battery read
     takes up to 14 ms.
4. **"Needs Vulkan".** Hotkeys check for an attached `PlvkScaler` (it has rendered a frame and
   still exists), not for a fresh stats snapshot. A static picture sends no frames, so the
   snapshot goes stale after 3 s even on Vulkan.
5. **Benchmark.**
   - It aborts when the renderer is destroyed for any reason, including a Vulkan → Vulkan
     recreation: new pipelines compile inside the measure window.
   - During a sweep the other hotkeys toast "Benchmark running (T aborts)".
   - The step toasts are two lines (`Benchmark 4/7: ewa_lanczossharp` / `(keep content
     moving)`): the status overlay's 36 pt font wraps at ~47 characters, and a wrapped overlay
     loses its outline.
   - `benchRecordFrame()` has no deband argument (the sweep runs without debanding).
6. **Overlay width.** Fork overlay lines wrap at `, ` before 85 characters, the 1024 px wrap at
   20 pt. The plan's full Scaling line reaches ~99 characters. The 0002 `Output` line, up to ~110
   with a desktop-HDR failure, wraps the same way.
7. **Marker during A/B and bench** (decided with the user): `(session)` is hidden on the
   Scaling line while bilinear is on screen for A/B, or a bench step runs. That gives Test H's
   `bilinear, A/B`. The config's flag and the `session=` log field still follow the plan's rule.
8. **Small additions.**
   - `[` / `]` leave A/B, so the change is visible.
   - B on bilinear toasts "A/B needs an upscaler other than bilinear".
   - The scaler log line ends with ` ab=1` while bypassed.
   - New log lines: `scaler <name> hook parsed in X ms (N passes)`, and bench start / done /
     aborted.
   - GT0 is the average of the samples where it was running (RC6 reads 0); `idle` when every
     sample was 0.
   - A machine with no battery counts as AC.
9. **Shared helper.** `summarizeGpuTimes()` moved from `plvk.cpp` to `renderstats` for the
   benchmark; `plvk.cpp` lost the fork's `<algorithm>` include with it.
10. **After the visual check** (decided with the user): Spline36, Lanczos and EWA get a fixed
    `antiring = 0.8`. Scope excluded an anti-ringing *slider*; there is still none. NIS's SDR
    mode on PQ was tried and reverted after the user's recheck (Visual check above), so NIS
    follows the source as planned.
