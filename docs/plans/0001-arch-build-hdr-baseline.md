# 0001 — Arch build, GNOME HDR baseline and GPU timing

| Field | Value |
|---|---|
| Status | Done |
| Created | 2026-10-25 |
| Branch | `claude/0001-arch-build-hdr-baseline` |
| PR | ike-0/moonlight-intel PR 1 |

## Goal

`intel` builds on the target (Arch, GNOME, Lunar Lake / Arc 140V) with the libplacebo Vulkan
renderer enabled, and we have measured numbers for the existing HDR path at a 1080p-class
16:10 source on the 2880×1800 120 Hz panel. Those numbers decide how much GPU budget plan 0002
(upscaler) can spend.

## Context

**Motivation:** bitrate is the limit. Streaming native 2880×1800 at 120 Hz HDR, even at
200 Mbps, still shows heavy compression on foliage and smoke. At the same bitrate a
1728×1080 stream gets 2.78× more bits per pixel (1920×1200: 2.25×). The client then has to
upscale cheaply enough to hold 120 Hz with HDR.

Findings from planning (verified against the code at `ac296864` and public sources, Oct 2026):

**Current Linux HDR path (no fork changes needed to use it):**
`VAAPIRenderer` (iHD) decodes → frames exported as DRM-PRIME → `PlVkRenderer` imports them via
`pl_map_avframe_ex()` → libplacebo renders → Vulkan swapchain with
`VK_COLOR_SPACE_HDR10_ST2084_EXT` → compositor.
- `FFmpegVideoDecoder::createFrontendRenderer()` (`app/streaming/video/ffmpeg.cpp` ~L338–405):
  for 10-bit streams it tries `PlVkRenderer` first and keeps it only if it reports
  `RENDERER_ATTRIBUTE_HDR_SUPPORT`; otherwise it falls back to `DrmRenderer`, which needs DRM
  master and can't work inside a GNOME session. **So if Vulkan doesn't expose HDR10 on GNOME,
  HDR streaming doesn't work at all.**
- Vulkan Video decode (`AV_HWDEVICE_TYPE_VULKAN`) is only reached if VA-API fails, since
  FFmpeg lists VAAPI before VULKAN in the hwaccel order.
- `plvk.cpp` renders frames with `pl_render_fast_params` (~L1091): bilinear scaling, no
  deband, no sigmoid. This is where plan 0002 hooks in.
- Present mode: FIFO with V-Sync on; IMMEDIATE → FIFO_RELAXED → MAILBOX → FIFO with V-Sync off.
  Already logged.
- Window is created with `SDL_WINDOW_ALLOW_HIGHDPI` (`session.cpp` ~L1830); upstream 6.2 runs on
  SDL 3.4 via sdl2-compat.

**Stack requirements (what is actually needed):**
- Mesa ≥ 25.1: Vulkan WSI implements Wayland color management (`VK_EXT_swapchain_colorspace`
  HDR colour spaces + `VK_EXT_hdr_metadata`) — required for HDR10 swapchains on Wayland.
- GNOME/Mutter ≥ 48: implements `wp_color_management_v1`; HDR must be enabled for the panel in
  Settings → Displays.
- Mesa ≥ 25.2: ANV exposes Vulkan Video AV1 decode on Xe2. Not used by this plan (VA-API is the
  default decode path); noted for a possible later A/B.
- libplacebo: Arch ships 7.360.1, above the v7.349 the Vulkan renderer needs. **No update.**
- **Not needed:** `vpl-gpu-rt` / oneVPL. That is the QSV runtime; on Linux QSV sits on top of
  VA-API/iHD anyway, Moonlight has no QSV path on Linux, and FFmpeg's `*_qsv` decoders add
  async buffering. VA-API is already the lowest-latency Intel decode path.

**Unknowns this plan measures:**
1. Does Vulkan on GNOME expose HDR10 PQ to Moonlight (i.e. does `PlVkRenderer` stay selected)?
2. Is the swapchain at native 2880×1800 under GNOME fractional scaling, or at the logical size
   (which would add a compositor scale pass and blur)?
3. Libplacebo GPU time per frame today, at 120 fps HDR.
4. Decode time, frame drops and GPU engine load at 1728×1080 and 1920×1200 sources.

## Scope

**In:**
- `docs/arch.md`: Arch packages to build and run, build commands, diagnostic commands.
- `CLAUDE.md`: target desktop is now **GNOME** (not KDE Plasma); point to `docs/arch.md`.
- Opt-in GPU timing in `plvk.cpp`, enabled by env `MOONLIGHT_INTEL_GPU_TIMING=1`; zero
  behaviour change when unset.
- One-time log line at first rendered frame: swapchain size, colour space (primaries/transfer),
  HDR on/off, and the source frame size.
- Optional (only if it works within the cloud setup-script limits): build libplacebo 7.360.1
  from source in the cloud environment so `plvk.cpp` compiles there too. Record the result
  either way; don't block the PR on it.

**Out:** upscaler or render-param changes, decoder/renderer selection changes, Vulkan Video
A/B, stats-overlay UI changes, VRR, anything in `moonlight-common-c`.

## Design

**GPU timing (all inside `plvk.cpp` / `plvk.h`, marked `// moonlight-intel:`):**
- Add a member `pl_render_params m_RenderParams` initialised from `pl_render_fast_params`, and use
  it in the frame `pl_render_image()` call instead of the global. Defaults are unchanged.
- If `MOONLIGHT_INTEL_GPU_TIMING=1`, set `m_RenderParams.info_callback` / `info_priv`. In the
  callback, sum `pl_dispatch_info::last` (ns) across passes for the current frame and record
  the per-pass shader descriptions once.
  - Timer results are asynchronous: values may lag a frame or read 0 at first. Skip zeros and
    treat this as a rolling estimate.
- Every 5 s, log one line: `moonlight-intel: gpu-time avg=X.XX p99=Y.YY max=Z.ZZ ms frames=N`.
  Also log once: the pass list with their individual last/avg times.
- Check the actual field names in the installed `libplacebo/renderer.h` and
  `libplacebo/dispatch.h`; don't trust this plan's spelling.

**First-frame log:** read the size and `color_space` from the `pl_swapchain_frame` and log it
together with the mapped source frame's width/height and colour space. Check first what
Moonlight already logs and don't duplicate it.

**Fork hygiene:** keep the diff in `plvk.cpp`/`plvk.h` small and contiguous so upstream merges
stay cheap; record it in `docs/FORK.md`.

## Steps

1. Read `docs/FORK.md` and this plan.
2. Update `CLAUDE.md` (GNOME; link `docs/arch.md`).
3. Write `docs/arch.md`:
   - **Build:** `base-devel` `git` `qt6-base` `qt6-declarative` `qt6-svg` `qt6-wayland`
     `sdl2-compat` `sdl2_ttf` `ffmpeg` `libva` `intel-media-driver` `libplacebo`
     `vulkan-icd-loader` `vulkan-intel` `vulkan-headers` `opus` `openssl` `libdrm` `wayland`
     `wayland-protocols` `libxkbcommon`.
   - **Diagnostics:** `libva-utils` `vulkan-tools` `intel-gpu-tools`.
   - Mark the list as unverified until the user confirms it on the laptop.
   - **Explicitly don't install** `vpl-gpu-rt` / `libvpl`, and say why.
   - Build commands (out-of-tree `build/`, full libplacebo), run command, and the diagnostic
     commands from the Hardware test section.
4. Implement GPU timing and the first-frame log in `plvk.cpp`/`plvk.h`.
5. Build check — depends on where this runs:
   - **Local session on the laptop (preferred for this plan):** build with full libplacebo
     (no `disable-libplacebo`); qmake must print "Vulkan support enabled via libplacebo". The
     cloud-only items (disable-libplacebo build, libplacebo from source) are N/A.
   - **Cloud session:** build with `"CONFIG+=disable-libplacebo"`. Also attempt the optional
     libplacebo-from-source step; if that fails, say in the PR that `plvk.cpp` is only
     compile-checked on Arch.
6. **Local session only:** run Hardware test steps 0 (versions) and 2 (build) yourself and put
   the outputs in `Execution notes`.
   - Steps 1, 3 and 4 belong to the user: GNOME settings, driving the stream, and `sudo`.
   - Never run `sudo`, never read the Moonlight config directory, and only read the stream log
     through the `grep` in "Collect".
7. Update `docs/FORK.md` (delta row) and this plan (status, criteria, execution notes).
8. Open the PR into `intel` with `gh pr create --repo ike-0/moonlight-intel --base intel`,
   copying the Hardware test section below in verbatim.

## Acceptance criteria

- [x] Builds with no new warnings in touched files: on Arch with libplacebo (local session), or
      in the cloud with `CONFIG+=disable-libplacebo`
- [x] `plvk.cpp` compiles before the PR: Arch build (local), or cloud with source-built
      libplacebo, or flagged for the user's Arch build
- [x] With `MOONLIGHT_INTEL_GPU_TIMING` unset, behaviour and default log output are unchanged
      (by code inspection: identical params, no callback. The `moonlight-intel: frame` lines
      are always on; see Execution notes)
- [ ] With it set, a `moonlight-intel: gpu-time …` line appears every ~5 s during streaming
      (implemented; confirm in the hardware test)
- [ ] First-frame line logs swapchain size + colour space and source size + colour space
      (implemented as log-on-change; confirm in the hardware test)
- [x] `docs/arch.md` exists; `CLAUDE.md` says GNOME
- [x] `docs/FORK.md` updated
- [ ] User has filled in the Baseline results table below (after merge is fine)

## Hardware test (user runs on the laptop)

```bash
# 0. Stack versions (paste output)
pacman -Q mesa vulkan-intel intel-media-driver libva ffmpeg libplacebo sdl2-compat qt6-base gnome-shell
vainfo 2>/dev/null | grep -E 'Driver version|VAProfileHEVCMain10|VAProfileAV1Profile0'
vulkaninfo --summary 2>/dev/null | grep -E 'driverName|driverInfo|apiVersion' | head -3

# 1. GNOME: Settings → Displays → HDR on. Note the scale % in use.

# 2. Build
git switch claude/0001-arch-build-hdr-baseline && git pull
git submodule update --init --recursive
mkdir -p build && cd build && qmake6 ../moonlight-qt.pro && make -j"$(nproc)" release
#    qmake output must include: "Vulkan support enabled via libplacebo"

# 3. Run (from build/)
MOONLIGHT_INTEL_GPU_TIMING=1 ./app/moonlight 2>&1 | tee /tmp/ml-0001.log

# 4. In a second terminal during the stream, ~30 s:
sudo intel_gpu_top -s 1000 -o - | head -35 > /tmp/ml-0001-gpu.txt
```

**Stream settings:**
- Run A: custom resolution 1728×1080, 120 fps.
- Run B: custom resolution 1920×1200, 120 fps.
- Both runs: HDR on, HEVC, ~80 Mbps, V-Sync on, frame pacing on, fullscreen.
- Repeat run A with AV1 if the host supports it.

**Collect without IPs or hostnames:**
```bash
grep -E 'moonlight-intel:|renderer|Renderer|HDR|present mode|libplacebo: (Using|Vulkan)|swapchain' /tmp/ml-0001.log
```
Also note from the overlay (Ctrl+Alt+Shift+S): decode time, render time, frames dropped,
network latency.

**Expected:**
- `PlVkRenderer` is selected with HDR active.
- The swapchain is 2880×1800 with PQ transfer.
- GPU time is well under 8.33 ms (frame period at 120 Hz).

If any of these fail, that is the finding. Record it and don't work around it in this plan.

## Baseline results

Measured by the user on 2026-10-25, recorded from the planning chat.

**Scope change (user decision, after merge):** Run A (1728×1080) is dropped. All fork work
targets a **1920×1200** source, which upscales exactly ×1.5 to the 2880×1800 panel. A separate
HEVC run was skipped; the baseline is AV1.

**Conditions:**
- AV1 Main10, 1920×1200 @ 120 fps.
- HDR on: host and GNOME.
- V-Sync and frame pacing on, fullscreen.
- Moving content: UFO test, scrolling.
- `MOONLIGHT_INTEL_GPU_TIMING=1`, run ~90 s.

| Item | Run B 1920×1200 (AV1) |
|---|---|
| Renderer chain (backend → frontend) | VAAPI → Vulkan (libplacebo) |
| HDR active (y/n), swapchain colour space | y; BT.709/sRGB on frame 1 → BT.2020/PQ `hdr=on` from frame 2 |
| Swapchain size | 2880×1800 (native, not the 166% logical size) |
| GNOME scale % | 166 |
| libplacebo gpu-time avg / p99 / max (ms) | 0.35–0.47 / 0.48–0.53 / ≤ 0.57 (per 5 s window, 16 windows) |
| Frames rendered per 5 s window | 594–600, i.e. ≈ 119.4 fps; 0 timer-less frames after the first window |
| libplacebo passes | 1: `color decoding, color encoding` (fused decode + bilinear + PQ encode) |
| Overlay: decode / render (ms), drops | not recorded |
| gputop: render %, video % | moonlight: rcs 7.6 %, vcs 34.2 %; gnome-shell: rcs 5.1 %; GT0 800 MHz, GT1 (media) 400 MHz |

**Reading:**
- **Render budget.** Libplacebo uses ≈ 0.45 ms of the T = 1/f = 8.33 ms frame period at
  f = 120 Hz, about 5–6 %, and that is at a low 800 MHz GT0 clock.
- **Decode budget.** VA-API decode keeps the video engine (vcs) busy 34 % of the time. With U as
  the busy fraction, that is U·T ≈ 0.342 × 8.33 ≈ 2.8 ms per frame, at a 400 MHz media clock.
- **Compositor.** gnome-shell spends ~5 % of render-engine time while the stream is fullscreen
  (≈ 0.4 ms per frame). That suggests Mutter composites the HDR surface instead of scanning it
  out directly. This is noted for a possible later investigation; it isn't a blocker.
- **Run C** (GNOME in SDR Native with an HDR stream) is folded into plan 0002's hardware test F (setting Off, GNOME in SDR Native): check whether the second `frame` line reaches `hdr=on`.

## Risks and upstream-merge impact

- `plvk.cpp` is actively changed upstream (it had a libplacebo error-handling fix on 2026-10-24).
  Keep the patch to a member struct, one callback and one log call.
- A logical-size swapchain or missing HDR10 on GNOME would push 0002 toward fixing
  presentation first (SDL/Mutter interaction), before any upscaler work.

## Execution notes

Run as a local session on the target laptop, 2026-10-25. Cloud-only items (the
`disable-libplacebo` build, libplacebo from source) are N/A.

**Changes from the plan:**
- **First-frame log is log-on-change (agreed with the user during the session).** libplacebo's
  `pl_swapchain_colorspace_hint()` only sets `needs_recreate`; the swapchain is rebuilt at the
  next `start_frame` (checked in libplacebo v7.360.1 `src/vulkan/swapchain.c`). Moonlight calls
  `start_frame` in `waitToRender()` *before* `renderFrame()` applies the hint. So frame 1
  always renders into the initial SDR swapchain, and a one-time line would have reported SDR
  even when HDR works. The `moonlight-intel: frame …` line is now logged on the first frame and
  whenever swapchain size/colour space or source size/colour space change. Expect 2–3 lines per
  stream: SDR, then HDR, then any fullscreen resize or triple-buffer switch.
- **The frame lines are always on, not gated by `MOONLIGHT_INTEL_GPU_TIMING`.** Scope lists them
  separately from the opt-in timing. That is the only default-log change: a few lines per
  stream, with one 32-byte compare per frame. Gating them is a one-line change if preferred.
- **`gpu-time` line has an extra field:** `… frames=N skipped=M`. M counts frames whose timers
  had not read back yet (0 ns). If no frame in a window had timer data, it logs
  `gpu-time no timer data skipped=M` instead.
- **Pass breakdown format:** `moonlight-intel: gpu-pass[i] last=X avg=Y ms: <shader description>`,
  logged once from the first 5 s window that has data. `avg` is libplacebo's own per-pass
  rolling average.
- Source size in the frame line is the mapped frame's crop rect, i.e. what libplacebo
  actually scales from.

**Surprises / things to know:**
- The plan's step 2 says "qmake output must include 'Vulkan support enabled via libplacebo'".
  Top-level `qmake6` only processes the SUBDIRS project. The line is printed during `make`, as
  `Project MESSAGE: Vulkan support enabled via libplacebo`, when `app.pro` is processed. The
  Hardware test section is kept verbatim; `docs/arch.md` says this correctly.
- `libvpl` is a hard dependency of Arch's `ffmpeg`, so it can't be avoided; it is only the
  dispatcher and harmless. `vpl-gpu-rt` is already installed on this laptop, explicitly, and is
  only optional for ffmpeg. It was left alone; Moonlight never loads it. `docs/arch.md` says
  "don't install `vpl-gpu-rt` for Moonlight" instead of a blanket ban.
- libplacebo field names matched the plan: `pl_render_params.info_callback`/`info_priv`,
  `pl_render_info.pass`, `pl_dispatch_info.last`/`average`, `pl_shader_info.description`.
- While the stats overlay (Ctrl+Alt+Shift+S) is shown, its overlay pass is included in
  `gpu-time`. For a clean libplacebo number, read some `gpu-time` lines with the overlay off.
- The macOS-only early render in `waitToRender()` still uses `pl_render_fast_params` and isn't
  timed. It doesn't affect Linux.
- Moonlight already logs the negotiated stream size (`Video stream is WxHxFPS`). Nothing
  logged the swapchain or the actual frame colour space, so the new line duplicates nothing.

**Hardware test step 0 (versions), run by the session:**
```
mesa 1:26.2.5-1
vulkan-intel 1:26.2.5-1
intel-media-driver 26.3.5-1
libva 2.24.1-1
ffmpeg 2:9.0.2-2
libplacebo 7.360.1-7
sdl2-compat 2.32.74-1
qt6-base 6.12.0-3
gnome-shell 1:51.1-1
vainfo: Driver version: Intel iHD driver for Intel(R) Gen Graphics - 26.3.5 ()
      VAProfileHEVCMain10             :	VAEntrypointVLD
      VAProfileHEVCMain10             :	VAEntrypointEncSlice
      VAProfileAV1Profile0            :	VAEntrypointVLD
      VAProfileAV1Profile0            :	VAEntrypointEncSlice
	apiVersion         = 1.4.354
	driverName         = Intel open-source Mesa driver
	driverInfo         = Mesa 26.2.5-arch1.1
```
All above the plan's minimums (Mesa ≥ 25.1, GNOME ≥ 48, libplacebo ≥ 7.349; `PL_API_VER` 360).
Every package in `docs/arch.md`'s build and diagnostics lists is installed.

**Hardware test step 2 (build), run by the session:**
- Fresh `build/`, `qmake6 ../moonlight-qt.pro && make -j6 release`: exit 0, binary
  `build/app/moonlight`.
- Build log contains `Project MESSAGE: Vulkan support enabled via libplacebo`. Compile flags
  include `-DHAVE_LIBPLACEBO_VULKAN`, `-DHAVE_LIBVA_WAYLAND`, `-DHAVE_DRM`.
- Warnings, baseline (unmodified `a769f219`) vs this branch: identical. Both have only the 6
  pre-existing `AVVulkanDeviceContext::lock_queue`/`unlock_queue` deprecations in `plvk.cpp`
  (FFmpeg 9 headers), now 8 lines lower. No new warnings in `plvk.cpp`, `plvk.h` or
  `ffmpeg.cpp`, which was recompiled because it includes `plvk.h`.
- Not run: no stream was started (steps 1, 3 and 4 are the user's), so the new log lines have
  not been seen live yet.
