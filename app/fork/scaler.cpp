#include "scaler.h"
#include "renderstats.h"
#include "systemstate.h"
#include "toast.h"

#include "SDL_compat.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <vector>

namespace Fork
{

struct ScalerInfo
{
    const char* name;
    const char* label;
    bool hasSharpness;
    int defaultSharpness;
};

// Indexed by ScalerId
static const ScalerInfo k_Scalers[kScalerCount] = {
    { "bilinear", "Bilinear", false, 0 },
    { "spline36", "Spline36", false, 0 },
    { "lanczos", "Lanczos", false, 0 },
    { "ewa_lanczossharp", "EWA Lanczos Sharp", false, 0 },
    { "fsr1", "FSR 1", true, (int)(IntelPreferences::kDefaultFsrSharpness * 100 + 0.5) },
    { "nis", "NIS", true, (int)(IntelPreferences::kDefaultNisSharpness * 100 + 0.5) },
    { "ravu-zoom-ar-r2", "RAVU-Zoom-AR r2", false, 0 },
};

static const int k_SharpnessStep = 5;          // hotkey step, hundredths
static const uint32_t k_BenchSettleMs = 1000;  // discarded after each switch
static const uint32_t k_BenchMeasureMs = 5000;
static const int k_BenchMinFrames = 540;       // 90 % of 5 s at 120 Hz

const char* scalerName(ScalerId id)
{
    return k_Scalers[id].name;
}

const char* scalerLabel(ScalerId id)
{
    return k_Scalers[id].label;
}

bool scalerHasSharpness(ScalerId id)
{
    return k_Scalers[id].hasSharpness;
}

int defaultSharpness(ScalerId id)
{
    return k_Scalers[id].defaultSharpness;
}

float fsrSharpnessStops(int sharpness)
{
    return 2.0f * (1.0f - sharpness / 100.0f);
}

void formatSharpness(ScalerId id, int sharpness, char* text, size_t length)
{
    if (id == IntelPreferences::SC_FSR1) {
        snprintf(text, length, "%.2f (%.2f stops)", sharpness / 100.0, fsrSharpnessStops(sharpness));
    }
    else if (id == IntelPreferences::SC_NIS) {
        snprintf(text, length, "%.2f (%d%%)", sharpness / 100.0, sharpness);
    }
    else {
        snprintf(text, length, "-");
    }
}

const char* scalerSourceName(ScalerSource source)
{
    switch (source) {
    case ScalerSource::Settings:
        return "settings";
    case ScalerSource::Hotkey:
        return "hotkey";
    case ScalerSource::Bench:
        return "bench";
    }

    return "?";
}

struct BenchResult
{
    int frames;
    double avgMs, p99Ms;
};

// Everything below is guarded by s_Lock
static std::mutex s_Lock;
static ScalerConfig s_Config;  // live
static ScalerConfig s_Saved;   // the settings the stream started from
static uint64_t s_Generation = 0;
static const void* s_Renderer = nullptr;

static struct
{
    bool running;
    int step;               // 0-based, also the ScalerId
    ScalerConfig restore;   // the live config at the start
    uint64_t generation;    // of the current step's config
    bool started;           // the step has had a frame
    uint32_t startTicks;    // of that frame
    bool measuring;         // past the settle time
    uint32_t measureTicks;
    std::vector<uint64_t> videoNs;
    uint64_t scalerNs;
    uint64_t debandNs;
    BenchResult results[kScalerCount];
} s_Bench;

// Call after every change to s_Config
static void commitChange(ScalerSource source)
{
    // The marker compares the chosen scaler and its sharpness with the saved settings, so A/B
    // bypass doesn't set it. During a sweep the config from before the sweep stands in.
    const ScalerConfig& chosen = s_Bench.running ? s_Bench.restore : s_Config;
    s_Config.session = chosen.scaler != s_Saved.scaler ||
                       chosen.sharpness[chosen.scaler] != s_Saved.sharpness[chosen.scaler];
    s_Config.source = source;
    s_Config.generation = ++s_Generation;
}

void currentScalerConfig(ScalerConfig* config)
{
    std::lock_guard<std::mutex> lock(s_Lock);
    *config = s_Config;
}

void scalerSessionStarted(ScalerId scaler, int fsrSharpness, int nisSharpness, bool debanding)
{
    std::lock_guard<std::mutex> lock(s_Lock);
    s_Bench.running = false;

    s_Config = {};
    s_Config.scaler = scaler;
    s_Config.sharpness[IntelPreferences::SC_FSR1] = std::clamp(fsrSharpness, 0, 100);
    s_Config.sharpness[IntelPreferences::SC_NIS] = std::clamp(nisSharpness, 0, 100);
    s_Config.debanding = debanding;
    s_Saved = s_Config;
    commitChange(ScalerSource::Settings);
}

static void beginBenchStep(int step, char* toast, size_t length)
{
    ScalerId scaler = (ScalerId)step;

    s_Config.scaler = scaler;
    for (int i = 0; i < kScalerCount; i++) {
        s_Config.sharpness[i] = defaultSharpness((ScalerId)i);
    }
    s_Config.debanding = false;
    s_Config.bypass = false;
    s_Config.benchStep = step + 1;
    s_Config.benchTotal = kScalerCount;
    commitChange(ScalerSource::Bench);

    s_Bench.step = step;
    s_Bench.generation = s_Config.generation;
    s_Bench.started = false;
    s_Bench.measuring = false;
    s_Bench.videoNs.clear();
    s_Bench.scalerNs = 0;
    s_Bench.debandNs = 0;

    snprintf(toast, length, "Benchmark %d/%d: %s\n(keep content moving)",
             step + 1, kScalerCount, scalerName(scaler));
}

static void startBench(char* toast, size_t length)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "moonlight-intel: bench start: %d scalers, %u ms settle + %u ms measure each, debanding off, default sharpness",
                kScalerCount, k_BenchSettleMs, k_BenchMeasureMs);

    s_Bench.running = true;
    s_Bench.restore = s_Config;
    beginBenchStep(0, toast, length);
}

static void endBench(const char* reason)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "moonlight-intel: bench %s, restoring %s", reason, scalerName(s_Bench.restore.scaler));

    s_Bench.running = false;
    s_Config = s_Bench.restore;
    commitChange(ScalerSource::Bench);
}

static void logBenchSummary()
{
    const BenchResult& bilinear = s_Bench.results[IntelPreferences::SC_BILINEAR];
    for (int i = 0; i < kScalerCount; i++) {
        const BenchResult& result = s_Bench.results[i];
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "moonlight-intel: bench-summary scaler=%s dp99=%+.2f davg=%+.2f ms vs bilinear%s",
                    scalerName((ScalerId)i), result.p99Ms - bilinear.p99Ms, result.avgMs - bilinear.avgMs,
                    result.frames < k_BenchMinFrames ? " lowframes" : "");
    }
}

// The measure window is over: log the step, then move to the next one or finish
static void finishBenchStep(char* toast, size_t length)
{
    ScalerId scaler = (ScalerId)s_Bench.step;
    BenchResult& result = s_Bench.results[s_Bench.step];

    double maxMs;
    result.frames = (int)s_Bench.videoNs.size();
    summarizeGpuTimes(s_Bench.videoNs, &result.avgMs, &result.p99Ms, &maxMs);
    double scalerAvgMs = s_Bench.scalerNs / 1e6 / result.frames;

    SystemState state;
    if (!systemStateWindow(&state)) {
        state = {};
        state.gt0MHz = -1;
        state.batteryW = -1;
        snprintf(state.profile, sizeof(state.profile), "n/a");
        snprintf(state.throttle, sizeof(state.throttle), "n/a");
    }

    char sharpness[32] = "-";
    if (scalerHasSharpness(scaler)) {
        snprintf(sharpness, sizeof(sharpness), "%.2f", s_Config.sharpness[scaler] / 100.0);
    }

    char gt0[24];
    if (state.gt0MHz > 0) {
        snprintf(gt0, sizeof(gt0), "%d MHz", state.gt0MHz);
    }
    else {
        snprintf(gt0, sizeof(gt0), "%s", state.gt0MHz == 0 ? "idle" : "n/a");
    }

    // On AC the battery's power is its charge rate, so it is left out
    char battery[32] = "";
    if (state.samples > 0 && !state.onAc) {
        if (state.batteryW >= 0) {
            snprintf(battery, sizeof(battery), " battery=%.1f W", state.batteryW);
        }
        else {
            snprintf(battery, sizeof(battery), " battery=n/a");
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "moonlight-intel: bench %d/%d scaler=%s sharpness=%s power=%s profile=%s frames=%d video avg=%.2f p99=%.2f max=%.2f ms scaler avg=%.2f ms gt0=%s%s throttle=%s%s",
                s_Bench.step + 1, kScalerCount, scalerName(scaler), sharpness,
                state.samples == 0 ? "n/a" : state.onAc ? "AC" : "battery", state.profile,
                result.frames, result.avgMs, result.p99Ms, maxMs, scalerAvgMs,
                gt0, battery, state.throttle,
                result.frames < k_BenchMinFrames ? " lowframes" : "");

    if (s_Bench.step + 1 < kScalerCount) {
        beginBenchStep(s_Bench.step + 1, toast, length);
    }
    else {
        logBenchSummary();
        endBench("done");
        snprintf(toast, length, "Benchmark done: see log");
    }
}

bool benchRecordFrame(uint64_t generation, uint64_t videoNs, uint64_t scalerNs, uint64_t debandNs)
{
    char toast[128] = "";
    bool firstMeasured = false;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        if (!s_Bench.running || generation != s_Bench.generation) {
            return false;
        }

        // Steps run on frame time: without frames, the sweep waits
        uint32_t now = SDL_GetTicks();
        if (!s_Bench.started) {
            s_Bench.started = true;
            s_Bench.startTicks = now;
        }

        if (!s_Bench.measuring) {
            if (now - s_Bench.startTicks < k_BenchSettleMs) {
                return false;
            }

            s_Bench.measuring = true;
            s_Bench.measureTicks = now;
            beginSystemStateWindow();
            firstMeasured = true;
        }

        s_Bench.videoNs.push_back(videoNs);
        s_Bench.scalerNs += scalerNs;
        s_Bench.debandNs += debandNs;

        if (now - s_Bench.measureTicks >= k_BenchMeasureMs) {
            finishBenchStep(toast, sizeof(toast));
        }
    }

    if (toast[0] != 0) {
        showToast("%s", toast);
    }
    return firstMeasured;
}

void scalerRendererAttached(const void* renderer)
{
    std::lock_guard<std::mutex> lock(s_Lock);
    s_Renderer = renderer;
}

void scalerRendererDetached(const void* renderer)
{
    std::lock_guard<std::mutex> lock(s_Lock);
    if (s_Renderer != renderer) {
        return;
    }
    s_Renderer = nullptr;

    // Numbers across a renderer change aren't comparable (new pipelines, maybe another renderer)
    if (s_Bench.running) {
        endBench("aborted (renderer destroyed)");
    }
}

void scalerSessionEnded()
{
    std::lock_guard<std::mutex> lock(s_Lock);
    if (s_Bench.running) {
        endBench("aborted (stream ended)");
    }
}

enum class Hotkey
{
    None,
    Next,
    Previous,
    AB,
    Sharper,
    Softer,
    Bench,
};

static Hotkey matchHotkey(int sym, int scancode)
{
    switch (sym) {
    case SDLK_u:
        return Hotkey::Next;
    case SDLK_y:
        return Hotkey::Previous;
    case SDLK_b:
        return Hotkey::AB;
    case SDLK_RIGHTBRACKET:
        return Hotkey::Sharper;
    case SDLK_LEFTBRACKET:
        return Hotkey::Softer;
    case SDLK_t:
        return Hotkey::Bench;
    }

    // As upstream, the key the user sees wins. Key positions only count on non-Latin layouts;
    // otherwise e.g. Dvorak's physical B key (sym x) would take upstream's Ctrl+Alt+Shift+X.
    if ((sym >= SDLK_a && sym <= SDLK_z) || sym == SDLK_LEFTBRACKET || sym == SDLK_RIGHTBRACKET) {
        return Hotkey::None;
    }

    switch (scancode) {
    case SDL_SCANCODE_U:
        return Hotkey::Next;
    case SDL_SCANCODE_Y:
        return Hotkey::Previous;
    case SDL_SCANCODE_B:
        return Hotkey::AB;
    case SDL_SCANCODE_RIGHTBRACKET:
        return Hotkey::Sharper;
    case SDL_SCANCODE_LEFTBRACKET:
        return Hotkey::Softer;
    case SDL_SCANCODE_T:
        return Hotkey::Bench;
    default:
        return Hotkey::None;
    }
}

static const char* sessionSuffix()
{
    return s_Config.session ? " (session)" : "";
}

// U/Y, B, [ and ] on the live config. Hotkeys never write the settings.
static void applyHotkey(Hotkey key, char* toast, size_t length)
{
    ScalerId scaler = s_Config.scaler;

    switch (key) {
    case Hotkey::Next:
    case Hotkey::Previous:
        scaler = (ScalerId)((scaler + (key == Hotkey::Next ? 1 : kScalerCount - 1)) % kScalerCount);
        s_Config.scaler = scaler;
        s_Config.bypass = false;
        commitChange(ScalerSource::Hotkey);

        if (scalerHasSharpness(scaler)) {
            snprintf(toast, length, "Upscaler: %s, sharpness %.2f%s",
                     scalerLabel(scaler), s_Config.sharpness[scaler] / 100.0, sessionSuffix());
        }
        else {
            snprintf(toast, length, "Upscaler: %s%s", scalerLabel(scaler), sessionSuffix());
        }
        break;

    case Hotkey::AB:
        if (scaler == IntelPreferences::SC_BILINEAR) {
            snprintf(toast, length, "A/B needs an upscaler other than bilinear");
            break;
        }

        s_Config.bypass = !s_Config.bypass;
        commitChange(ScalerSource::Hotkey);
        snprintf(toast, length, "A/B: %s", scalerLabel(s_Config.activeScaler()));
        break;

    case Hotkey::Sharper:
    case Hotkey::Softer: {
        if (!scalerHasSharpness(scaler)) {
            snprintf(toast, length, "No sharpness for %s", scalerLabel(scaler));
            break;
        }

        // Leaves A/B, so the change is on screen
        int step = key == Hotkey::Sharper ? k_SharpnessStep : -k_SharpnessStep;
        int sharpness = std::clamp(s_Config.sharpness[scaler] + step, 0, 100);
        if (sharpness != s_Config.sharpness[scaler] || s_Config.bypass) {
            s_Config.sharpness[scaler] = sharpness;
            s_Config.bypass = false;
            commitChange(ScalerSource::Hotkey);
        }

        snprintf(toast, length, "%s sharpness %.2f%s", scalerLabel(scaler), sharpness / 100.0, sessionSuffix());
        break;
    }

    case Hotkey::None:
    case Hotkey::Bench:
        break;
    }
}

bool scalerHandleHotkey(int sym, int scancode)
{
    Hotkey key = matchHotkey(sym, scancode);
    if (key == Hotkey::None) {
        return false;
    }

    char toast[128] = "";
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        if (s_Renderer == nullptr) {
            snprintf(toast, sizeof(toast), "Upscaler needs the Vulkan renderer");
        }
        else if (key == Hotkey::Bench) {
            if (s_Bench.running) {
                endBench("aborted (T)");
                snprintf(toast, sizeof(toast), "Benchmark aborted");
            }
            else {
                startBench(toast, sizeof(toast));
            }
        }
        else if (s_Bench.running) {
            snprintf(toast, sizeof(toast), "Benchmark running (T aborts)");
        }
        else {
            applyHotkey(key, toast, sizeof(toast));
        }
    }

    showToast("%s", toast);
    return true;
}

}
