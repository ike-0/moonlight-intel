#pragma once

#include "intelpreferences.h"

#include <cstddef>
#include <cstdint>

namespace Fork
{

// moonlight-intel: upscaler ids and names, and the session's live upscaler config, which the
// in-stream hotkeys and the benchmark sweep change. No libplacebo here: PlvkScaler
// (plvkscaler.h) turns a config into render params on the render thread.

// Persisted, append-only ids (IntelPreferences::upscaler). Their order is also the hotkey cycle
// and benchmark order.
using ScalerId = IntelPreferences::Upscaler;
constexpr int kScalerCount = IntelPreferences::SC_RAVU_ZOOM_AR_R2 + 1;

const char* scalerName(ScalerId id);   // overlay and log name, e.g. "fsr1"
const char* scalerLabel(ScalerId id);  // toast name, e.g. "FSR 1"
bool scalerHasSharpness(ScalerId id);  // FSR 1 and NIS
int defaultSharpness(ScalerId id);     // hundredths; 0 without sharpness

// Sharpness s in hundredths (0-100, higher is sharper) in the shader's own unit
float fsrSharpnessStops(int sharpness);  // RCAS reduction in stops: 2 * (1 - s)
// "0.90 (0.20 stops)", "0.25 (25%)" or "-" for scalers without sharpness
void formatSharpness(ScalerId id, int sharpness, char* text, size_t length);

enum class ScalerSource
{
    Settings,  // session start
    Hotkey,
    Bench,     // a benchmark step, or the restore after the sweep
};
const char* scalerSourceName(ScalerSource source);

struct ScalerConfig
{
    ScalerId scaler;              // the chosen scaler; bypass renders bilinear instead
    int sharpness[kScalerCount];  // hundredths, used by FSR 1 and NIS
    bool debanding;
    bool bypass;                  // A/B: render bilinear
    bool session;                 // scaler or its sharpness differs from the saved settings
    ScalerSource source;          // what made the last change
    int benchStep, benchTotal;    // 1-based step of a running benchmark; 0 = none
    uint64_t generation;          // increases with every change

    ScalerId activeScaler() const { return bypass ? IntelPreferences::SC_BILINEAR : scaler; }
};

// Copies the live config. Thread-safe; the renderer reads it every frame.
void currentScalerConfig(ScalerConfig* config);

// A renderer that applies the config attaches when it renders its first frame and detaches when
// it is destroyed. Hotkeys need an attached renderer; a benchmark aborts when it detaches.
void scalerRendererAttached(const void* renderer);
void scalerRendererDetached(const void* renderer);

// Benchmark feed (render thread): one call per frame that has GPU timer data, tagged with the
// config generation the frame was rendered with. Returns true for the first measured frame of a
// step, so the renderer can log that frame's pass list.
bool benchRecordFrame(uint64_t generation, uint64_t videoNs, uint64_t scalerNs, uint64_t debandNs);

// Session hooks (fork.cpp). Each stream starts from the settings passed in, which are also what
// the session marker compares against.
void scalerSessionStarted(ScalerId scaler, int fsrSharpness, int nisSharpness, bool debanding);
void scalerSessionEnded();

// Ctrl+Alt+Shift+<key> down (SDL main thread). Returns true if the key is an upscaler hotkey,
// which is then never sent to the host.
bool scalerHandleHotkey(int sym, int scancode);

}
