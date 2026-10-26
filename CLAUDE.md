# moonlight-intel

Personal fork of `moonlight-stream/moonlight-qt`, tuned for Linux on Intel Arc / Xe graphics.
Primary target: ThinkPad X1 Carbon Gen 13 (Lunar Lake, Arc 140V / Xe2-LPG), Arch Linux,
GNOME on Wayland, VA-API via the `iHD` driver, 2.8K 120 Hz OLED, HDR.
Arch packages, build and diagnostic commands for the target: `docs/arch.md`.

Work is planned in claude.ai chats and handed over as plan files in `docs/plans/`.
Execute the plan you are pointed at; don't widen its scope.

## Repo topology (read first)

| Name | What | Rule |
|---|---|---|
| `origin` | `ike-0/moonlight-intel` (this fork, **public**) | the only repo you write to |
| `upstream` | `moonlight-stream/moonlight-qt` | read-only; fetch from it, never write to it |
| `master` | pristine mirror of `upstream/master` | **never commit here**; only fast-forwarded |
| `intel` | default branch, all fork work | changes land via PR only |
| `claude/<NNNN>-<slug>` | one branch per plan | first commit is the plan itself; PR into `intel` |

Fork delta at any time: `git diff master...intel` (three dots).

## Hard rules

1. **Nothing goes upstream.** Never open PRs, issues, comments, reviews or discussions on any
   `moonlight-stream/*` repo or any repo other than `ike-0/moonlight-intel`. Every `gh` command
   passes `--repo ike-0/moonlight-intel`; every PR has base `intel`. GitHub's PR UI on a fork
   defaults the base repo to upstream — never accept that default.
2. **No cross-references.** Don't write `owner/repo#N`, upstream issue/PR URLs or `@mentions`
   in commits, PRs or docs: on a public repo these post backlinks onto upstream timelines.
   Refer to them as plain text, e.g. "upstream issue 1234".
3. **No history rewriting.** No force-push, no rebase of `intel` or `master`, no deleting
   branches other than your own merged `claude/*` ones. Upstream is integrated by merge.
4. **Public repo hygiene.** Never commit or paste into PRs: `Moonlight.conf`, client or host
   certificates/keys, pairing PINs, IP/MAC addresses, hostnames, Tailscale/WAN details,
   Sunshine credentials, or raw logs containing any of these. Redact as `<HOST_IP>`,
   `<HOST_NAME>`, `<MAC>` etc. Never read the Moonlight config directory.
5. **Submodules are upstream's.** `moonlight-common-c`, `qmdnsengine` and
   `app/SDL_GameControllerDB` point at upstream repos. Don't edit inside them or repoint
   `.gitmodules`. If a plan needs a protocol-level change in `moonlight-common-c`, stop and
   report it: that needs its own fork, which is a planning decision.
6. **Don't touch `.github/`** (workflows, dependabot, bots) unless the plan says to.

## Keep merges cheap

Upstream moves fast (often several commits a week). Every line we change is a future
conflict, so:
- prefer new files and small, well-named hooks over rewriting upstream functions;
- keep each delta self-contained and greppable — mark fork-specific blocks with a
  `// moonlight-intel:` comment;
- record every behavioural delta in `docs/FORK.md` (what, why, files touched).

## Working from a plan

1. The planning chat pushes the plan as the first commit on `claude/<NNNN>-<slug>`. Work on
   that branch (check it out if the session started on `intel`). No such branch → create it
   from `intel`.
2. Read the plan and `docs/FORK.md`.
3. Implement; build (below); commit in logical steps.
4. Before opening the PR: tick the plan's acceptance criteria, set its `Status`, fill in
   `Execution notes` (what changed vs the plan, anything surprising), update `docs/FORK.md`.
5. PR into `intel` with a **Hardware test** section the user can run on the laptop verbatim.

If the plan contradicts the code, stop, write what you found under `Execution notes`,
and ask rather than improvising a redesign.

## Build

```bash
git submodule update --init --recursive
mkdir -p build && cd build
qmake6 ../moonlight-qt.pro && make -j"$(nproc)" release   # binary: build/app/moonlight
```

- **Cloud VM (Ubuntu 24.04):** its libplacebo is older than the v7.349 the Vulkan renderer
  needs, so add `"CONFIG+=disable-libplacebo"` to qmake. `plvk.cpp` changes are therefore only
  compile-checked on the Arch machine; say so in the PR.
- **Arch:** full build including libplacebo/Vulkan; packages in `docs/arch.md`.
- No unit-test suite exists. Verification = clean build with no new warnings in touched files,
  plus the plan's hardware test steps run by the user.

## Code map (video path)

- `app/streaming/video/ffmpeg.cpp` — decoder/renderer selection and fallback chain
- `app/streaming/video/ffmpeg-renderers/vaapi.cpp` — VA-API: iHD/i965 driver quirks, surface
  formats, EGL/DRM-PRIME export, colour handling
- `app/streaming/video/ffmpeg-renderers/plvk.cpp` — Vulkan via libplacebo (main HDR path)
- `app/streaming/video/ffmpeg-renderers/eglvid.cpp`, `drm.cpp` — EGL and KMS renderers
- `app/streaming/video/ffmpeg-renderers/pacer/` — frame pacing
- `app/streaming/session.cpp` — stream setup, decoder capability probing
- `app/settings/` — preferences; `app/gui/` — QML UI
- `app/fork/` — the fork's own code (docs/FORK.md): settings (`intelpreferences`), desktop HDR,
  overlay lines (`renderstats`), upscaler config, hotkeys and benchmark (`scaler`), its
  libplacebo side (`plvkscaler`), toasts, GPU state sampler (`systemstate`). Upstream files
  reach it through `fork/fork.h`; plvk also includes `renderstats.h` and `plvkscaler.h`
- `app/fork/shaders/` — vendored upscaler shaders (FSR 1, NIS, RAVU); provenance, licences and
  edits in its README
- `moonlight-common-c/` — streaming protocol (submodule — don't edit)

## Testing on the target (user runs these; cloud sessions have no Arc GPU)

- Run from the build dir, don't `make install` (avoids clobbering a packaged Moonlight).
- The fork shares `~/.config/Moonlight Game Streaming Project/` with any stock Moonlight
  install — pairing and settings are common to both.
- Useful: `SDL_VIDEODRIVER=wayland`, `LIBVA_DRIVER_NAME=iHD`, `vainfo`, `gputop`,
  Moonlight's in-stream stats overlay (Ctrl+Alt+Shift+S).
- PR test steps state the exact command, stream settings (resolution, fps, codec, HDR on/off)
  and what to look for in the overlay/log.
