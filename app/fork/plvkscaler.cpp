#include "plvkscaler.h"

#include <libplacebo/shaders/custom.h>
#include <libplacebo/shaders/sampling.h>

#include <QFile>

#include "SDL_compat.h"

#include <cstring>

namespace Fork
{

// The vendored mpv hooks (fork/shaders/) and the names of their //!PARAMs
struct HookInfo
{
    ScalerId id;
    const char* resource;
    const char* sharpnessParam;  // FSR: RCAS stops (float); NIS: percent (int)
    const char* pqParam;         // 1 for a PQ-encoded source
};

static const HookInfo k_Hooks[] = {
    { IntelPreferences::SC_FSR1, ":/fork/shaders/FSR.glsl", "SHARPNESS", "FSR_PQ" },
    { IntelPreferences::SC_NIS, ":/fork/shaders/NVScaler.glsl", "NIS_SHARPNESS_PCT", "NIS_HDR_MODE" },
    { IntelPreferences::SC_RAVU_ZOOM_AR_R2, ":/fork/shaders/ravu-zoom-ar-r2.hook", nullptr, nullptr },
};

// libplacebo's own filters; nullptr for bilinear (plain texture sampling) and the hooks
static const pl_filter_config* builtinFilter(ScalerId id)
{
    switch (id) {
    case IntelPreferences::SC_SPLINE36:
        return &pl_filter_spline36;
    case IntelPreferences::SC_LANCZOS:
        return &pl_filter_lanczos;
    case IntelPreferences::SC_EWA_LANCZOSSHARP:
        return &pl_filter_ewa_lanczossharp;
    default:
        return nullptr;
    }
}

// The //!DESC of every //!HOOK block. libplacebo describes each hook pass by it, which is how
// the GPU timing finds the hook's passes.
static std::vector<std::string> hookPassNames(const QByteArray& text)
{
    std::vector<std::string> names;
    bool inHeader = false, isHook = false;
    std::string description;

    for (const QByteArray& rawLine : text.split('\n')) {
        QByteArray line = rawLine.trimmed();
        if (line.startsWith("//!")) {
            if (!inHeader) {
                inHeader = true;
                isHook = false;
                description.clear();
            }
            if (line.startsWith("//!HOOK")) {
                isHook = true;
            }
            else if (line.startsWith("//!DESC")) {
                description = line.mid(7).trimmed().toStdString();
            }
        }
        else if (inHeader) {
            if (isHook && !description.empty()) {
                names.push_back(description);
            }
            inHeader = false;
        }
    }

    return names;
}

PlvkScaler::~PlvkScaler()
{
    // destroy() must have run while the GPU was alive
    for (const Hook& hook : m_Hooks) {
        SDL_assert(hook.hook == nullptr);
    }

    if (m_Attached) {
        scalerRendererDetached(this);
    }
}

void PlvkScaler::destroy()
{
    for (Hook& hook : m_Hooks) {
        pl_mpv_user_shader_destroy(&hook.hook);
        hook = Hook();
    }
    m_ActiveHook = nullptr;

    if (m_Attached) {
        scalerRendererDetached(this);
        m_Attached = false;
    }
}

// Parses a hook on first use. Returns nullptr for scalers that aren't hooks.
PlvkScaler::Hook* PlvkScaler::loadHook(pl_gpu gpu, ScalerId id)
{
    const HookInfo* info = nullptr;
    for (const HookInfo& candidate : k_Hooks) {
        if (candidate.id == id) {
            info = &candidate;
        }
    }
    if (info == nullptr) {
        return nullptr;
    }

    Hook& hook = m_Hooks[id];
    if (hook.loaded) {
        return &hook;
    }
    hook.loaded = true;
    hook.failed = true;

    QFile file(info->resource);
    if (!file.open(QIODevice::ReadOnly)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "moonlight-intel: scaler %s failed (can't open %s)", scalerName(id), info->resource);
        return &hook;
    }
    QByteArray text = file.readAll();

    Uint64 start = SDL_GetPerformanceCounter();
    hook.hook = pl_mpv_user_shader_parse(gpu, text.constData(), text.size());
    double parseMs = (SDL_GetPerformanceCounter() - start) * 1000.0 / SDL_GetPerformanceFrequency();
    if (hook.hook == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "moonlight-intel: scaler %s failed (shader didn't parse)", scalerName(id));
        return &hook;
    }

    for (int i = 0; i < hook.hook->num_parameters; i++) {
        const pl_hook_par& parameter = hook.hook->parameters[i];
        if (info->sharpnessParam != nullptr && strcmp(parameter.name, info->sharpnessParam) == 0) {
            hook.sharpness = parameter.data;
        }
        else if (info->pqParam != nullptr && strcmp(parameter.name, info->pqParam) == 0) {
            hook.pq = parameter.data;
        }
    }

    if ((info->sharpnessParam != nullptr && hook.sharpness == nullptr) ||
            (info->pqParam != nullptr && hook.pq == nullptr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "moonlight-intel: scaler %s failed (shader lacks its //!PARAMs)", scalerName(id));
        pl_mpv_user_shader_destroy(&hook.hook);
        return &hook;
    }

    hook.passNames = hookPassNames(text);
    hook.failed = false;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "moonlight-intel: scaler %s hook parsed in %.1f ms (%d passes)",
                scalerName(id), parseMs, (int)hook.passNames.size());
    return &hook;
}

void PlvkScaler::selectScaler(pl_gpu gpu, pl_render_params* params)
{
    ScalerId id = m_Config.activeScaler();

    // A hook that failed renders as bilinear. Hooks are upscale-only by their own WHEN
    // conditions, so a smaller window also falls back to bilinear, inside libplacebo.
    Hook* hook = loadHook(gpu, id);
    m_ActiveHook = hook != nullptr && !hook->failed ? hook : nullptr;

    params->upscaler = builtinFilter(id);
    params->hooks = m_ActiveHook != nullptr ? &m_ActiveHook->hook : nullptr;
    params->num_hooks = m_ActiveHook != nullptr ? 1 : 0;
    params->deband_params = m_Config.debanding ? &pl_deband_default_params : nullptr;
}

// libplacebo disables a hook that fails at render time (by its signature) and carries on
// without it. Fall back to bilinear for the rest of this renderer's life.
void PlvkScaler::checkHookErrors(pl_renderer renderer, pl_render_params* params)
{
    if (m_ActiveHook == nullptr) {
        return;
    }

    pl_render_errors errors = pl_renderer_get_errors(renderer);
    if (!(errors.errors & PL_RENDER_ERR_HOOKS)) {
        return;
    }

    for (int i = 0; i < errors.num_disabled_hooks; i++) {
        if (errors.disabled_hooks[i] == m_ActiveHook->hook->signature) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "moonlight-intel: scaler %s failed (libplacebo disabled the hook)",
                         scalerName(m_Config.activeScaler()));
            m_ActiveHook->failed = true;
            m_ActiveHook = nullptr;
            params->hooks = nullptr;
            params->num_hooks = 0;
            return;
        }
    }
}

void PlvkScaler::logConfig(int pq) const
{
    ScalerId id = m_Config.activeScaler();
    char sharpness[32];
    formatSharpness(id, m_Config.sharpness[id], sharpness, sizeof(sharpness));

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "moonlight-intel: scaler %s sharpness=%s deband=%s pq=%d source=%s session=%d%s",
                scalerName(id), sharpness, m_Config.debanding ? "on" : "off", pq,
                scalerSourceName(m_Config.source), m_Config.session ? 1 : 0,
                m_Config.bypass ? " ab=1" : "");
}

void PlvkScaler::apply(pl_gpu gpu, pl_renderer renderer, pl_render_params* params, const pl_frame* source)
{
    if (!m_Attached) {
        scalerRendererAttached(this);
        m_Attached = true;
    }

    checkHookErrors(renderer, params);

    ScalerConfig config;
    currentScalerConfig(&config);
    bool changed = config.generation != m_Config.generation;
    if (changed) {
        m_Config = config;
        selectScaler(gpu, params);
    }

    // Every scaler works on PQ-encoded values for HDR: libplacebo never linearises HDR for
    // scaling. A DEFINE change compiles a new shader once; libplacebo caches it.
    int pq = source->color.transfer == PL_COLOR_TRC_PQ ? 1 : 0;
    if (m_ActiveHook != nullptr) {
        ScalerId id = m_Config.activeScaler();
        if (m_ActiveHook->pq != nullptr) {
            m_ActiveHook->pq->i = pq;
        }
        if (id == IntelPreferences::SC_FSR1) {
            m_ActiveHook->sharpness->f = fsrSharpnessStops(m_Config.sharpness[id]);
        }
        else if (id == IntelPreferences::SC_NIS) {
            m_ActiveHook->sharpness->i = m_Config.sharpness[id];
        }
    }

    if (changed || pq != m_LoggedPq) {
        logConfig(pq);
        m_LoggedPq = pq;
    }
}

static bool hasStep(pl_shader_info shader, const char* step)
{
    for (int i = 0; shader != nullptr && i < shader->num_steps; i++) {
        if (strcmp(shader->steps[i], step) == 0) {
            return true;
        }
    }
    return false;
}

bool PlvkScaler::isScalerPass(pl_shader_info shader) const
{
    if (m_ActiveHook == nullptr) {
        return false;
    }

    for (const std::string& name : m_ActiveHook->passNames) {
        if (hasStep(shader, name.c_str())) {
            return true;
        }
    }
    return false;
}

bool PlvkScaler::isDebandPass(pl_shader_info shader)
{
    return hasStep(shader, "debanding");
}

bool PlvkScaler::recordFrame(uint64_t videoNs, uint64_t scalerNs, uint64_t debandNs)
{
    return benchActive() && benchRecordFrame(m_Config.generation, videoNs, scalerNs, debandNs);
}

void PlvkScaler::describe(char* text, size_t length) const
{
    ScalerId id = m_Config.activeScaler();
    if (m_Hooks[id].failed) {
        snprintf(text, length, "%s failed -> bilinear", scalerName(id));
    }
    else if (scalerHasSharpness(id)) {
        snprintf(text, length, "%s s%.2f", scalerName(id), m_Config.sharpness[id] / 100.0);
    }
    else {
        snprintf(text, length, "%s", scalerName(id));
    }
}

}
