# 0005 — GPU-state sampler: no ACPI power reads while streaming

| Field | Value |
|---|---|
| Status | In review |
| Created | 2026-10-26 |
| Branch | `claude/0005-sampler-no-acpi-reads` |
| PR | ike-0/moonlight-intel PR 7 |

## Goal

1. A normal stream (stats overlay hidden, no benchmark running) makes **no sysfs reads at all**.
2. Outside a benchmark sweep, the fork **never reads a file that evaluates ACPI**, overlay shown or not.
3. The overlay's `GPU state` line and the `bench` lines keep their content. The one exception: the battery's draw in watts is measured only during a sweep.

## Context

**Symptom.** Battery reads through ACPI are known to cause stutters on this machine. Plan 0003's
sampler (delta D7) makes them for the whole stream, whether the overlay is shown or not.

**What the merged sampler reads.** `app/fork/systemstate.cpp` runs its own thread at 4 Hz, from
`Fork::sessionStarted()` to `Fork::sessionEnded()`:

| File (under `/sys`) | Kernel path | Read | Cost |
|---|---|---|---|
| `class/power_supply/<Mains>/online` | ACPI AC driver: re-evaluates `_PSR` on every read, no cache | every tick, on AC and on battery | an EC transaction, 4 per second |
| `class/power_supply/<Battery>/power_now` (or `current_now` × `voltage_now`) | ACPI battery driver: `_BST`, cached for 1 s (`battery.cache_time`) | every tick, on battery | one evaluation per second, up to ~14 ms (0003, deviation 3) |
| `firmware/acpi/platform_profile` | thinkpad_acpi returns its cached DYTC profile | every tick | trivial |
| xe `device/tile0/gt0/freq0/act_freq`, `throttle/reasons` | xe MMIO | every tick | trivial |

- **The separate thread doesn't isolate the cost.** The time is spent in the EC, and depending
  on the firmware in SMM, outside the process.
- **Consumers:**
  - `renderstats.cpp`: the `GPU state` line. `appendOverlayStats()` builds it only while the
    stats overlay is shown, about once a second while frames arrive (upstream's
    `submitDecodeUnit()` stats-window flip), plus once in the end-of-stream log.
  - `scaler.cpp`: the benchmark's `bench` lines, via `beginSystemStateWindow()` /
    `systemStateWindow()`.
- **The battery draw is still wanted for the benchmark.** Plan 0003's battery sweep logged 9.0 W
  for bilinear and 17.0 W for FSR 1.

**UPower.**
- `upowerd`, always running under GNOME, tracks the AC state from power-supply uevents.
- It exposes that state as the boolean `OnBattery` of interface `org.freedesktop.UPower`, at
  `/org/freedesktop/UPower` on the system bus.
- A property read returns upowerd's cached value, so neither our process nor upowerd evaluates
  ACPI for it.
- upowerd reads the power supplies on its own schedule, with or without Moonlight running.

## Scope

**In:**
- **Sampler modes** idle / overlay / benchmark:
  - overlay demand comes from `appendOverlayStats()`;
  - benchmark mode comes from `startBench()` / `endBench()`.
- **AC/battery from UPower `OnBattery`**, with sysfs `online` as a fallback, read once per
  wake-up.
- **Battery power** (`power_now`, or `current_now` × `voltage_now`) read only in benchmark mode.
- **Docs:** `docs/FORK.md` D7 and `docs/arch.md`.

**Out:**
- A per-power-source upscaler setting.
- Logging GT0's SLPC `power_profile` (not logged at the user's request, plan 0003).
- Any change to the benchmark's steps, timing or log format beyond what is stated here.
- The shipped default upscaler. It stays Bilinear; plan 0003 records the decision.
- Upstream files: none are touched.

## Design

**Modes** (`systemstate.cpp`):

| Mode | Active while | Reads per tick (4 Hz) | Power source |
|---|---|---|---|
| idle | neither of the others | nothing; waits on the condition variable | — |
| overlay | `systemStateDemand()` was called within the last 3 s | platform profile, GT0 `act_freq` + throttle reasons | UPower `OnBattery`: on wake, then once per published second |
| benchmark | `setSystemStateBenchmark(true)` until `false` | as overlay, plus battery power when on battery | as overlay |

Overlay and benchmark can be on together. Benchmark mode keeps the sampler awake with the
overlay hidden.

**API** (`systemstate.h`):
- `void systemStateDemand();`
  - Called as the first statement of `appendOverlayStats()`.
  - Stores a steady-clock timestamp in an atomic and notifies the condition variable.
  - It never starts a stopped sampler, so the end-of-stream log call after `sessionEnded()` is
    harmless.
- `void setSystemStateBenchmark(bool running);`
  - Called from `startBench()` (true) and `endBench()` (false) in `scaler.cpp`.
  - `endBench()` covers done, T abort, renderer destroyed and stream ended.
  - Lock order is scaler `s_Lock` → systemstate `s_Lock`, as `beginSystemStateWindow()` already
    does. systemstate never calls into scaler.
- `startSystemSampler()` starts the thread idle: benchmark mode off, no demand.

**Sampler loop:**
- **Idle:** waits without a timeout for demand, benchmark mode or stop.
  - Entering idle clears `s_HaveLatest` and `s_Second`, so a later overlay never shows minutes-old
    state.
  - `stopSystemSampler()` while idle joins promptly.
- **On wake:** the first sample is published at once (today's tick-1 rule), then once a second.
- **First overlay refresh:** it has no `GPU state` line; the line appears from the next refresh,
  about 1 s later. Accepted.

**Reader:**
- `SystemStateReader::read()` takes whether to read battery power; the executing session may
  split it instead.
- `online` is no longer read per tick (see Power source).
- Battery power is read only in benchmark mode, and only when the power source says battery.
- The constructor is unchanged. Power-supply `type` and the DRM driver link are static
  attributes, read once per sampler start.

**Power source:**
- **With `HAVE_UPOWER`** (a new define in the fork's `linux:qtHaveModule(dbus)` block of
  `app.pro`, next to `HAVE_GNOME_DISPLAY_CONFIG`):
  - `org.freedesktop.DBus.Properties.Get("org.freedesktop.UPower", "OnBattery")` on
    `QDBusConnection::systemBus()`;
  - `QDBus::Block`, 250 ms timeout, called from the sampler thread;
  - read on wake and once per published second.
- **A failed call** (no reply, an error, not a bool) skips UPower until the next wake and uses
  the fallback.
- **Fallback** (no UPower, or a failed call): sysfs `online` is read once per wake, with today's
  rule: no online Mains plus a battery means battery.
  - Known limitation: without UPower, an AC change during one wake shows only at the next wake.
- **QtDBus from a `std::thread`.** A blocking call from outside QtDBus's own thread waits on a
  condition, so it should work. If it doesn't, move the sampler to a `QThread`, as the
  desktop-HDR worker does, and note it in Execution notes.

**Output formats:**
- `GPU state: battery, <profile>, GT0 … MHz`.
  - The ` … W` suffix appears only while a benchmark runs.
  - This follows from `batteryW < 0`; `formatSystemState()` is unchanged.
- `bench` lines are unchanged, including `battery=… W`.

**Files:**
- `app/fork/systemstate.*`;
- `app/fork/renderstats.cpp` (one call);
- `app/fork/scaler.cpp` (two calls);
- `app/app.pro`: one define inside the fork block.

No upstream file is touched.

## Steps

1. Commit this plan (planning chat). Commit 2 sets plan 0003 to Done with its default-scaler
   decision.
2. `systemstate`: modes, the demand and benchmark API, the UPower power source with the
   fallback, battery power in benchmark mode only.
3. Wire it up:
   - `renderstats.cpp`: `systemStateDemand()`;
   - `scaler.cpp`: `setSystemStateBenchmark()` in `startBench()` / `endBench()`;
   - `app.pro`: `HAVE_UPOWER`.
4. Scratch harness, extending 0003's sysfs harness. Fake trees, with file opens observed (e.g.
   `strace -f -e trace=openat` on the harness, or an instrumented reader). It checks that:
   - idle opens nothing;
   - overlay mode with UPower available (real or mocked) opens no
     `power_supply/*/{online,power_now,current_now,voltage_now}`;
   - without UPower, overlay mode opens `online` exactly once per wake;
   - benchmark mode on battery opens battery power each tick;
   - mode transitions work: demand expires after 3 s, benchmark keeps the sampler awake, idle
     clears the latest state;
   - stop while idle joins within 100 ms.
5. Builds:
   - Arch;
   - scratch `CONFIG+=disable-libplacebo`;
   - a compile check of the path without `HAVE_UPOWER` (e.g. a scratch build with the define
     removed).
6. Docs:
   - `docs/FORK.md` D7: replace the sentence about the 4 Hz sampler with the modes and UPower.
   - `docs/arch.md`: the `GPU state` line (battery draw only during a sweep), the UPower source,
     and `busctl get-property …` under Diagnostics.
7. Validate the Hardware test commands, fill in Execution notes, set the status, open the PR.

## Acceptance criteria

- [x] Builds cleanly on Arch and with `CONFIG+=disable-libplacebo`; the path without
      `HAVE_UPOWER` compiles.
- [x] No new warnings in touched files.
- [x] No upstream file changed. `git diff --stat origin/intel... -- ':!app/fork' ':!docs'`
      lists only `app/app.pro`, with the change inside the fork block.
- [x] Harness checks of step 4 pass.
- [x] Hardware A: on battery, overlay never shown, 60 s: 0 power-supply value reads and 0 GT0
      reads.
- [x] Hardware B: overlay shown: 0 power-supply value reads, GT0 reads > 0. `GPU state` has no
      W and follows the charger within 2 s. (The user saw it follow the charger; not timed.)
- [x] Hardware C (changed with the user after review, see Execution notes): sweep on battery:
      0 power-supply value reads; the 7 `bench N/7` lines carry `power=battery`, the profile,
      GT0 and throttle reasons, and no `battery=` field.
- [ ] Hardware D: 5 min on battery with the overlay hidden: no periodic hitch.
- [x] `docs/FORK.md` and `docs/arch.md` updated.

## Hardware test (user runs on the laptop)

```bash
cd ~/repos/moonlight-intel
git fetch origin
git switch claude/0005-sampler-no-acpi-reads
git submodule update --init --recursive
mkdir -p build && cd build && qmake6 ../moonlight-qt.pro && make -j"$(nproc)" release
# UPower answers: "b false" on AC, "b true" on battery
busctl get-property org.freedesktop.UPower /org/freedesktop/UPower org.freedesktop.UPower OnBattery
```

**Tracing.** strace isn't installed on the laptop: `sudo pacman -S --needed strace`. With
`--seccomp-bpf` only `openat` stops the process, so the stream runs at normal speed. Run each
test separately and quit Moonlight between runs:

```bash
ml0005() { strace -f --seccomp-bpf -e trace=openat -o /tmp/ml-0005-$1.strace ~/repos/moonlight-intel/build/app/moonlight 2>&1 | tee /tmp/ml-0005-$1.log; }
ml0005 a    # test A; then "ml0005 b" for B and "ml0005 c" for C
```

**Stream settings:**
- 1920×1200, 120 fps, AV1, HDR on, V-Sync on, frame pacing on, fullscreen.
- The UFO test scrolling.
- Any upscaler.
- **On battery** unless stated otherwise.

**Tests:**
- **A.** Stream for 60 s without opening the stats overlay, then quit.
- **B.** Stream with the overlay (Ctrl+Alt+Shift+S) open for 60 s, watching the `GPU state` line.
  While it is open, plug the charger in, wait 10 s, then unplug. Quit.
- **C.** Stream with the overlay open, on battery. Press Ctrl+Alt+Shift+T, wait for
  `Benchmark done`, then quit.
- **D.** Launch the usual way (normal power profile), on battery, overlay hidden, 5 min of
  scrolling content or a game. If unsure, compare against the `intel` build.

**Collect** (counts only; the trace files stay local):

```bash
for X in a b c; do
  printf '%s power=%s gt0=%s\n' "$X" \
    "$(grep -cE '/power_supply/[^"]+/(online|power_now|current_now|voltage_now)"' /tmp/ml-0005-$X.strace)" \
    "$(grep -cE '/gt0/freq0/(act_freq|throttle/reasons)"' /tmp/ml-0005-$X.strace)"
done
grep -E 'moonlight-intel: bench ' /tmp/ml-0005-c.log
```

**Expected:**

| Test | power | gt0 | Overlay / feel |
|---|---|---|---|
| A | 0 | 0 | — |
| B | 0 | > 0 (about 8 per second while shown) | `GPU state: battery, …` without W; `AC` within 2 s of plugging in, `battery` again after unplugging |
| C | 0 | > 0 | `GPU state: battery, …` without W throughout; the 7 `bench N/7` lines show `power=battery`, `profile=`, `gt0=` and `throttle=`, and no `battery=` (the grep also shows the `bench start` and `bench done` lines) |
| D | — | — | no periodic (~1 s) hitch |

## Risks and upstream-merge impact

- **UPower missing or slow.** The fallback applies. The 250 ms timeout only ever blocks the
  sampler thread, never the render or main thread.
- **QtDBus outside a Qt thread:** see Design.
- **First overlay refresh.** It lacks the `GPU state` line; accepted.
- **A sweep still evaluates `_BST`** about once a second on battery, so stutters during a sweep
  are possible and accepted. The benchmark's numbers are GPU timer queries, so a CPU-side stall
  doesn't change them.
- **Merge impact:** fork files only, plus one define in the fork block of `app.pro`.

## Execution notes

- **Changed with the user after the first review (2026-10-26).** The user asked to keep the
  clock, throttle reasons, profile and power source and to drop the battery watts, as long as
  nothing is sampled with the overlay hidden in normal use. Two changes on top of the plan:
  - **Battery draw dropped entirely.** These are gone: `SystemState::batteryW`, the reads of
    `power_now` / `current_now` × `voltage_now`, the average and the `bench` line's `battery=`
    field. The battery is never read now.
    - Goal 3's exception and the Design table's "plus battery power" no longer apply: a sweep
      reads the same files as the overlay.
    - The `bench` log loses one field, against Out's "no log format change".
    - The only ACPI evaluation left is the `online` fallback when UPower fails.
  - **Mid-stream renderer recreation can no longer wake the sampler.** `appendOverlayStats()`
    calls `systemStateDemand()` only while the stats overlay (`Overlay::OverlayDebug`) is
    enabled. To check that, `renderstats.cpp` includes `streaming/session.h`, as `toast.cpp`
    does. This closes the exception in the finding below.
  - **A sweep still samples with the overlay hidden.** It is started on purpose (Ctrl+Alt+Shift+T)
    and lasts about 42 s, and none of its reads evaluates ACPI.
  - Hardware test C and its criterion now expect 0 power-supply reads and no `battery=` field.
- **As designed otherwise.** The session ran on the laptop, so the Arch build, the harness and
  a run against the real `/sys` and system bus all happened there. Implementation choices
  within the plan:
  - **Reader:** split as allowed. `readOnAc()` reads the Mains `online` files (the fallback) and
    `read(onAc)` reads the platform profile and GT0. The constructor is unchanged.
  - **Demand timestamp:** a `steady_clock` time point under the sampler's mutex, not an atomic.
    `systemStateDemand()` has to notify under that lock, or an idle sampler can miss the wake-up
    between its check and its wait. The cost is one uncontended lock per overlay refresh.
  - **Power source:** a `PowerSource` object lives for the whole stream; `wake()` resets its
    per-wake state (UPower trusted again, sysfs allowed once more).
  - **One log line, not in the plan:** the first UPower failure in a stream logs
    `moonlight-intel: UPower OnBattery unavailable (<reason>), power source from sysfs`.
    Without it, only an strace would show that the fallback is in use. `docs/arch.md`
    explains it.
  - **QtDBus from the `std::thread` works,** so no `QThread` was needed. The harness made
    blocking calls on the real system bus and on a private bus with no Qt warnings, and
    timeouts and errors came back as expected.
- **Finding: renderer recreation mid-stream (fixed after review, see above).**
  - `FFmpegVideoDecoder`'s destructor logs "Global video stats" through
    `stringifyVideoStats()`, which calls `appendOverlayStats()`.
  - At stream end that happens after `sessionEnded()`, so it is harmless, as the plan says.
  - It also happens whenever `Session` recreates the renderer during a stream: window events
    that `notifyWindowChanged()` doesn't take, a display change with another refresh rate, or
    `SDL_RENDER_DEVICE_RESET`. In the first version the sampler then woke for 3 s with the
    overlay hidden: about 24 GT0 / profile reads and 4 UPower calls, but no ACPI evaluation.
  - On the laptop, the Vulkan and EGL renderers handle size and display changes themselves,
    so this needs a display with another refresh rate or a device reset. That is not practical
    as a hardware step, so the harness checks the guard instead.
  - The recreation that every stream logs at start (`Recreating renderer for window event: 1`)
    doesn't count. It comes before the first frame, and `logVideoStats()` skips a decoder that
    rendered no frames. Logs from plans 0003 and 0004 have one recreation per stream, always
    there.
- **Stream end with a hung upowerd:** `stopSystemSampler()` joins the thread, so the main
  thread in `sessionEnded()` can wait for an in-flight UPower call, up to 250 ms. Before this
  plan it could wait for a battery read, up to about 15 ms. This only happens if upowerd
  stops answering, and only once per stream.
- **Builds** (run again after the review changes, same results):
  - Arch, in a scratch build directory: clean. The only warnings are upstream's two
    `lock_queue` / `unlock_queue` deprecations in `plvk.cpp`, which were there before.
  - Scratch `CONFIG+=disable-libplacebo`: 0 warnings, and no `plvk*` or `qrc_shaders` objects.
  - Without `HAVE_UPOWER`: `systemstate.cpp` compiled with the Arch build's exact command line
    minus the define gives 0 warnings and an object with no QtDBus symbols. The harness's
    `-Wall -Wextra` build without the define is also clean.
- **Harness (step 4),** scratch, not committed. It runs the real `systemstate.cpp`, plus the
  real `renderstats.cpp` against a stub `Session` for the guard.
  - Every `/sys` access is recorded with a timestamp by interposing `fopen`, `open`, `openat`
    and `opendir` (strace isn't installed on the laptop).
  - The fake trees are bind-mounted on `/sys` in a user + mount namespace. A nested user
    namespace maps back to uid 1000, so D-Bus authentication still matches.
  - UPower is a Python mock on a private bus that can answer battery or AC, fail with an error,
    return a string, or reply after 1 s.
  - The first version passed 178 checks, including the battery reads that were later dropped.
    After the review changes, 184 checks pass, and every run ends with 0 `power_now`,
    `current_now` or `voltage_now` opens:

| Run | Checks | Covers |
|---|---|---|
| fake `/sys`, mock UPower | 63 | idle: 0 opens in 2 s; start reads each supply's `type` once. Overlay: 0 power-supply value opens, about 8 GT0 opens a second, `battery, balanced, GT0 1450 MHz` with sysfs saying AC, 5 UPower calls in 4 s. Charger in / out seen after 0.76 / 1.00 s. Demand expiry: reads stop 3.00 s after the last call, idle clears the latest state, a new wake publishes after 5 ms. Benchmark on battery with the overlay shown: 0 power-supply value opens, 4 Hz, no W. Benchmark with the overlay hidden: awake for 4.5 s, idle within 300 ms after it. Guard: `appendOverlayStats()` with no session, with the stats overlay hidden (three calls, 0 opens) or with only the status overlay shown doesn't wake the sampler. With the stats overlay shown it does, and the second refresh has `GPU state: battery, balanced, GT0 1450 MHz`. Benchmark on AC: no battery reads. UPower failing mid-wake: `online` read once for the rest of the wake, UPower not asked again, logged once per stream, retried on the next wake. Not a bool and the 250 ms timeout (first sample after about 245 ms): `online` read once. Stop while idle joins in 0.2 ms; demand after stop starts nothing |
| fake `/sys`, bus without UPower | 35 | `online` read exactly once per wake (two overlay wakes, and a sweep's own wake), `ServiceUnknown` logged once; an AC change shows only at the next wake; the expiry, benchmark and stop checks as above |
| fake `/sys`, no system bus | 35 | the same, logged as `Disconnected` |
| fake `/sys`, built without `HAVE_UPOWER` | 35 | the same, and no UPower log |
| real `/sys` and system bus (on battery) | 16 | overlay: 0 power-supply value opens, 24 GT0 opens in 3 s, `battery, balanced, GT0 idle`. Benchmark: 8 samples, no battery reads, `online` never read. No Qt warnings |

- **Hardware test commands, checked on the laptop before the PR:**
  - `busctl get-property …` answers `b true` (on battery).
  - strace isn't installed, so the install line now says `sudo pacman -S --needed strace`.
  - The trace command was `./app/moonlight` with `X` to replace by hand. It is now a shell
    function with the absolute binary path, so it works from any directory.
  - The Collect patterns were run with GNU grep on strace-format lines, including `-f`'s
    `<unfinished ...>` split. They count the AC and USB `online`, `power_now` and the GT0 files,
    and ignore `type` and directory opens.
  - The `bench` grep also prints the `bench start` and `bench done` lines, which are useful
    context, so the expected text now names the `bench N/7` lines.
  - Nothing else in the app or `moonlight-common-c` reads power supplies, so the counts are
    the fork's alone.

**Hardware test, first run (user, 2026-10-26, on battery, static host image).**

| Test | power | gt0 | Other |
|---|---|---|---|
| A | 0 | 0 | — |
| B | 0 | 244 | `GPU state` switched to `AC` when the charger was plugged in and back after unplugging |
| C | 0 | 398 | 7 `bench N/7` lines with `power=battery profile=balanced`, GT0 800–1532 MHz, `throttle=none`, no `battery=` |

- **The traces are sound.** Each has 4 power-supply `type` reads (the sampler start, one per
  supply). In B and C, `gt0` is exactly twice the `platform_profile` count (122 and 199
  samples). No run logged `UPower OnBattery unavailable`.
- **A's trace** followed 279 threads and processes. After the start it has no profile, GT0 or
  power-supply value reads.
- **The host image was static,** so the host sent frames only when the picture changed. The
  overlay refreshes only when a frame arrives, so in B the sampler was awake for about 30 s of
  the 60 s and idled in between, as designed.
- **In C,** 6 of 7 steps were flagged `lowframes` (82–596 frames). That doesn't affect this
  test, which checks the fields, but these GPU times aren't comparable with other sweeps.
- **Still open:** D (5 min on battery with the overlay hidden, moving content).
