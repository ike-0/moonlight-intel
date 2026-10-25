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

Fork-specific environment variables:

| Variable | Effect |
|---|---|
| `MOONLIGHT_INTEL_GPU_TIMING=1` | logs libplacebo GPU time per frame every 5 s, plus a one-time pass breakdown |

The `moonlight-intel: frame …` lines (swapchain and source size and colour space, logged on the
first frame and whenever they change) are always on.

## Diagnostics

```bash
# Stack versions
pacman -Q mesa vulkan-intel intel-media-driver libva ffmpeg libplacebo sdl2-compat qt6-base gnome-shell
vainfo 2>/dev/null | grep -E 'Driver version|VAProfileHEVCMain10|VAProfileAV1Profile0'
vulkaninfo --summary 2>/dev/null | grep -E 'driverName|driverInfo|apiVersion' | head -3

# Logged run (from build/)
MOONLIGHT_INTEL_GPU_TIMING=1 ./app/moonlight 2>&1 | tee /tmp/ml.log

# GPU engine load during a stream (needs root)
sudo intel_gpu_top -s 1000 -o - | head -35

# Pull the relevant lines out of a log without IPs or hostnames
grep -E 'moonlight-intel:|renderer|Renderer|HDR|present mode|libplacebo: (Using|Vulkan)|swapchain' /tmp/ml.log
```

Useful while streaming: the stats overlay (Ctrl+Alt+Shift+S) shows decode and render time,
frame drops and network latency. More libplacebo detail: `PLVK_LOG_LEVEL=4` (info), which also
logs the surface format and colour space libplacebo picks ("Picked surface configuration").

Logs can contain the host's IP and name. Only share them through the `grep` above, and redact
anything else before posting.
