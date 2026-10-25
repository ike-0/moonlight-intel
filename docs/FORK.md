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
| D1 | 0001 | video / libplacebo | `plvk.cpp`, `plvk.h` | Video frames render with member `m_RenderParams` (a copy of `pl_render_fast_params`, same defaults) so later plans tune one place. Logs `moonlight-intel: frame …` (swapchain + source size and colour space) on the first frame and on change. Opt-in `MOONLIGHT_INTEL_GPU_TIMING=1` installs a libplacebo `info_callback` and logs `moonlight-intel: gpu-time …` every 5 s plus a one-time `gpu-pass[i]` breakdown. Measures plan 0002's GPU budget. Merge note: in `renderFrame()` keep `&m_RenderParams` in the `pl_render_image()` call and the hook lines around it; the rest is appended at the end of the file / class |

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
