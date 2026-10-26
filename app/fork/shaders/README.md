# Vendored upscaler shaders

mpv-format user shaders (`//!HOOK LUMA`) that `fork/plvkscaler.cpp` parses with
`pl_mpv_user_shader_parse()` on first use. They are built into the binary from `shaders.qrc`
(only in builds with libplacebo).

Each file keeps its full licence header and has one `// Modified by moonlight-intel (2026-10): …`
line after it. Edits are limited to the list below; every `//!DESC` line is unchanged, because
the stats overlay finds the scaler's passes by those descriptions. The first commit that added
these files holds the unmodified originals, so `git log -p` on this directory shows the edits.

## Files

### `FSR.glsl`: AMD FidelityFX Super Resolution 1.0.2 (EASU + RCAS)

| | |
|---|---|
| Source | agyild's mpv port, gist `82219c545228d70c5604f865ce0b0ce5` (https://gist.github.com/agyild/82219c545228d70c5604f865ce0b0ce5) |
| Revision | `2623d743b9c23f500ba086f05b385dcb1557e15d` (2022-03-06) |
| Also checked | identical to `shaders/FSR.glsl` in `iwalton3/default-shader-pack` at `9ab0195e` |
| Licence | MIT, © 2021 Advanced Micro Devices, Inc. |
| SHA-256, original | `56d8597fc6b7bf6d13f8c3b2bdf1cdc43b06175d51746aab44cf1dca16929b9e` |
| SHA-256, vendored | `6d08c993ef023e6443a89775a4ebb8cabfa7e8d2c23cd2568201fb9a5abf7785` |

Edits:
- Removed `#define SHARPNESS 0.2` (RCAS pass) and both `#define FSR_PQ 0` lines (EASU and RCAS
  passes).
- Added, before the first `//!HOOK`: `//!PARAM SHARPNESS` (`DYNAMIC float`, 0.0–2.0 stops,
  default 0.2) and `//!PARAM FSR_PQ` (`DEFINE`, 0–1, default 0). Parameters apply to both passes.
- Added the "Modified by" line.

### `NVScaler.glsl`: NVIDIA Image Scaling 1.0.2

| | |
|---|---|
| Source | agyild's mpv port, gist `7e8951915b2bf24526a9343d951db214` (https://gist.github.com/agyild/7e8951915b2bf24526a9343d951db214) |
| Revision | `05f00864228871ffd157daa9beb2db8fa7412cfa` (2022-02-24) |
| Also checked | identical to `shaders/NVScaler.glsl` in `iwalton3/default-shader-pack` at `9ab0195e` |
| Licence | MIT, © 2022 NVIDIA CORPORATION & AFFILIATES |
| SHA-256, original | `bac778ec0108d272e4dad97794a3c257e65bbebd52aa762f95bce930cbd6b73c` |
| SHA-256, vendored | `d20020f8b17acc615b79c337b27188c5915222d9a7a3706e5f74082c83a8b8ee` |

Edits:
- `#define SHARPNESS 0.25` became `#define SHARPNESS (float(NIS_SHARPNESS_PCT) / 100.0)`. It feeds
  file-scope `const float`s, so it has to stay a compile-time constant (hence `DEFINE`, not
  `DYNAMIC` or `CONSTANT`).
- Removed `#define NIS_HDR_MODE 0`.
- Added, before `//!HOOK`: `//!PARAM NIS_SHARPNESS_PCT` (`DEFINE`, 0–100, default 25) and
  `//!PARAM NIS_HDR_MODE` (`DEFINE`, 0–1, default 0).
- Added the "Modified by" line.

### `ravu-zoom-ar-r2.hook`: RAVU-Zoom-AR, radius 2, compute variant

| | |
|---|---|
| Source | `compute/ravu-zoom-ar-r2.hook` in https://github.com/bjin/mpv-prescalers |
| Revision | `b3f0a59d68f33b7162051ea5970a5169558f0ea2` (2024-01-11); git blob `c8470739397c2e5c7f8e0dd81f024fac41bd866f` |
| Licence | LGPL-3.0-or-later (bjin) |
| SHA-256, original | `2a577b00274f90df2dc2258daf59fbd62d27c733680bec1ddaf567baaf651dbb` |
| SHA-256, vendored | `f04aa24ba5c784116ee9eb3b73687e22908902e89bec845a9a472a8d3251bed6` |

Edits: the "Modified by" line only. The trained weights (two 18×2592 `rgba16f` LUTs, about 3 MB
of hex) are unchanged.

## Licences

The fork is GPL-3.0, inherited from upstream Moonlight. MIT and LGPL-3.0-or-later code may be
combined into a GPL-3.0 work. The obligations are to keep the notices (kept in each file), mark
modifications (the "Modified by" lines and this README) and provide the source (this public
repository).

## Updating a shader

1. Copy the new original over the file and commit it alone.
2. Re-apply the edits above, and check that every `//!DESC` line is unchanged.
3. Update this README (revision, both hashes).
4. Re-run the headless check from plan 0003 (step 8) and a stream with each changed scaler.
