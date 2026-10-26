#pragma once

#include "scaler.h"

#include <libplacebo/renderer.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Fork
{

// moonlight-intel: the upscaler inside PlVkRenderer. Before each pl_render_image() it applies the
// live ScalerConfig to the renderer's params: a libplacebo filter or a user hook (parsed from the
// qrc on first use), debanding, and per frame the hook's PQ and sharpness parameters. It also
// classifies passes for the GPU timing and feeds the benchmark.
//
// Render thread only, except destroy(), which plvk's destructor calls while the GPU is alive.
class PlvkScaler
{
public:
    PlvkScaler() = default;
    ~PlvkScaler();

    PlvkScaler(const PlvkScaler&) = delete;
    PlvkScaler& operator=(const PlvkScaler&) = delete;

    void apply(pl_gpu gpu, pl_renderer renderer, pl_render_params* params, const pl_frame* source);

    // Frees the parsed hooks, which own GPU textures (LUTs)
    void destroy();

    // Pass classification for the GPU timing callback
    bool isScalerPass(pl_shader_info shader) const;  // a pass of the active hook
    static bool isDebandPass(pl_shader_info shader);

    // After each pl_render_image() that has GPU timer data. Feeds a running benchmark; returns
    // true when the renderer should log this frame's pass list.
    bool recordFrame(uint64_t videoNs, uint64_t scalerNs);
    bool benchActive() const { return m_Config.benchStep > 0; }

    // The config on screen, and its overlay name: "fsr1 s0.90", "nis failed -> bilinear"
    const ScalerConfig& config() const { return m_Config; }
    void describe(char* text, size_t length) const;

private:
    struct Hook
    {
        const pl_hook* hook = nullptr;
        bool loaded = false;                 // parse attempted
        bool failed = false;                 // didn't parse, or libplacebo disabled it
        std::vector<std::string> passNames;  // the //!DESC of each pass
        pl_var_data* sharpness = nullptr;    // //!PARAM values, written every frame
        pl_var_data* pq = nullptr;
    };

    Hook* loadHook(pl_gpu gpu, ScalerId id);
    void selectScaler(pl_gpu gpu, pl_render_params* params);
    void checkHookErrors(pl_renderer renderer, pl_render_params* params);
    void logConfig(int pq) const;

    Hook m_Hooks[kScalerCount];
    Hook* m_ActiveHook = nullptr;  // the hook in params, if any
    ScalerConfig m_Config = {};    // last applied
    int m_LoggedPq = -1;
    bool m_Attached = false;
};

}
