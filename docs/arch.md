# Arch Linux: build and diagnostics

Target: X1 Carbon Gen 13 (Lunar Lake, Arc 140V / Xe2-LPG), Arch Linux, GNOME on Wayland.

## Packages

Checked on the target on 2026-10-25: all of these are installed and the full build (with
libplacebo) succeeds. Not yet tried on a clean install, so a missing transitive dependency
would only show up there.

**Build and run:**

```bash
pacman -S --needed base-devel git qt6-base qt6-declarative qt6-svg qt6-wayland \
    sdl2-compat sdl2_ttf ffmpeg libva intel-media-driver libplacebo \
    vulkan-icd-loader vulkan-intel vulkan-headers opus openssl libdrm \
    wayland wayland-protocols libxkbcommon
```

**Diagnostics:**

```bash
pacman -S --needed libva-utils vulkan-tools intel-gpu-tools
```

**Don't install `vpl-gpu-rt` for Moonlight.** It is the oneVPL/QSV runtime. On Linux, QSV runs
on top of VA-API/iHD anyway, Moonlight has no QSV path on Linux, and FFmpeg's `*_qsv` decoders
add async buffering. VA-API through iHD is already the lowest-latency Intel decode path. If
another application already installed it, leave it: Moonlight never loads it. `libvpl`, the
dispatcher only, is a hard dependency of Arch's `ffmpeg` and is harmless.

Minimum versions that matter for HDR (Arch ships newer ones):

| Component | Needed | Why |
|---|---|---|
| Mesa (`vulkan-intel`) | ≥ 25.1 | Vulkan WSI Wayland colour management, so HDR10 swapchains work |
| GNOME / Mutter | ≥ 48 | `wp_color_management_v1`; HDR enabled in Settings → Displays |
| libplacebo | ≥ 7.349 | needed for the Vulkan renderer to be built at all |

## Build

Out of tree in `build/` (gitignored), with full libplacebo/Vulkan:

```bash
git submodule update --init --recursive
mkdir -p build && cd build
qmake6 ../moonlight-qt.pro && make -j"$(nproc)" release
```

The build output must include `Project MESSAGE: Vulkan support enabled via libplacebo`. It is
printed by `make`, not by the top-level `qmake6`, because `app.pro` is only processed when
`make` descends into `app/`. Binary: `build/app/moonlight`.

## Run

From `build/`, without `make install` (so a packaged Moonlight is not clobbered):

```bash
./app/moonlight
```

Settings and pairing are shared with any stock Moonlight install.

Fork options are in Settings → `moonlight-intel` (stored under `[moonlight-intel]` in the shared
`Moonlight.conf`). The one environment variable is an override:

| Variable | Effect |
|---|---|
| `MOONLIGHT_INTEL_GPU_TIMING=1` | forces "Log GPU render timing" on: video GPU time (`gpu-time(video)`) every 5 s, plus a one-time pass breakdown |

The `moonlight-intel: frame …` lines (swapchain and source size and colour space, logged on the
first frame and whenever they change) are always on.

## Desktop HDR

With "Switch desktop to HDR during HDR streams" on, Moonlight switches the panel to HDR
(BT.2100) when the host sends HDR, and back to the previous colour mode (e.g. SDR Native)
afterwards. It uses Mutter's temporary display configuration: nothing is written to
`~/.config/monitors.xml`, and there is no confirmation dialog. A marker in
`$XDG_RUNTIME_DIR/moonlight-intel/desktop-hdr.json` lets the next launch restore the panel after
a crash. Each switch logs a `moonlight-intel: desktop-hdr …` line.

```bash
# Inspect the current colour mode (read-only)
gdctl show --properties | grep -E 'Monitor |color-mode'
```

## Upscaler and benchmark

With the Vulkan (libplacebo) renderer (HDR streams, or SDR with "Use Vulkan (libplacebo)
renderer for SDR streams"), Settings → `moonlight-intel` → Upscaler picks how the stream is
scaled up to the window: Bilinear (the default), Spline36, Lanczos, EWA Lanczos Sharp, FSR 1,
NIS or RAVU-Zoom-AR r2, plus FSR 1 / NIS sharpness and debanding. Spline36, Lanczos and EWA run
with libplacebo's anti-ringing at 0.8; NIS runs in its SDR mode on HDR streams too, because its
HDR mode oversharpens (on HDR text, NIS sharpness 0.00–0.10 measured closest to native). Each
stream starts from these settings. In-stream hotkeys change them for that stream only:

| Ctrl+Alt+Shift + | Action |
|---|---|
| U / Y | next / previous upscaler |
| B | A/B: toggle between the current upscaler and bilinear |
| `]` / `[` | sharper / softer (FSR 1 and NIS, steps of 0.05) |
| T | start or abort the benchmark sweep |

The stats overlay (Ctrl+Alt+Shift+S) shows the choice on its `Scaling` line, e.g.
`(x1.50), fsr1 s0.90 (session), 0.52 ms`: `(session)` means the upscaler or its sharpness differs
from the settings, and the ms are its share of the video GPU time. The `GPU state` line shows the
power source (with the battery's draw), the ACPI platform profile and the GT0 (render) clock.

**Benchmark sweep (T).** It runs every upscaler for 1 s (discarded) + 5 s (measured), about
42 s in all, then returns to the previous choice. The host only sends frames when the picture
changes, so keep the content moving (a scrolling UFO test, or a game scene) and leave the window
alone: a renderer change aborts the sweep. Each upscaler gets a `bench` line, and the sweep ends
with one `bench-summary` line per upscaler: Δp99 and Δavg of the video GPU time against
bilinear, from the same sweep. A step with fewer than 540 measured frames is flagged
`lowframes`.

GPU time depends on the power source, so run the sweep once on AC and once on battery (unplug,
wait about a minute, check that the `GPU state` line says `battery`), and compare the two:

```bash
grep -E 'moonlight-intel: bench' /tmp/ml.log
```

GT0's `act_freq` reads 0 while the GPU idles between frames (RC6), so the GT0 clock is the
average of the samples where it was running; `idle` means every sample was 0.

## Diagnostics

```bash
# Stack versions
pacman -Q mesa vulkan-intel intel-media-driver libva ffmpeg libplacebo sdl2-compat qt6-base gnome-shell
vainfo 2>/dev/null | grep -E 'Driver version|VAProfileHEVCMain10|VAProfileAV1Profile0'
vulkaninfo --summary 2>/dev/null | grep -E 'driverName|driverInfo|apiVersion' | head -3

# Logged run (from build/)
MOONLIGHT_INTEL_GPU_TIMING=1 ./app/moonlight 2>&1 | tee /tmp/ml.log

# Per-client GPU engine load during a stream: rcs = render/3D, vcs = video decode.
# gputop (intel-gpu-tools) reads DRM fdinfo, so your own processes need no root.
gputop -d 1 -n 5 | grep -aE 'Frequency|PID|moonlight|gnome-shell'

# Pull the relevant lines out of a log without IPs or hostnames
grep -E 'moonlight-intel:|renderer|Renderer|HDR|present mode|libplacebo: (Using|Vulkan)|swapchain' /tmp/ml.log
```

GPU timings depend on the power source: the same libplacebo pass measured ~0.4 ms on the
charger and ~0.66 ms on battery (plan 0002). Always note AC or battery when comparing numbers.

Useful while streaming: the stats overlay (Ctrl+Alt+Shift+S) shows decode and render time,
frame drops and network latency, plus the fork's lines: video GPU time, the scaling step and
upscaler, and the output colour space when libplacebo renders; the GPU state (power source,
platform profile, GT0 clock) and the desktop-HDR state with any renderer. More libplacebo detail: `PLVK_LOG_LEVEL=4` (info), which also
logs the surface format and colour space libplacebo picks ("Picked surface configuration").

Logs can contain the host's IP and name. Only share them through the `grep` above, and redact
anything else before posting.
