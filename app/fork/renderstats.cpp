#include "renderstats.h"
#include "desktophdr.h"
#include "fork.h"
#include "systemstate.h"

#include "streaming/session.h"

#include "SDL_compat.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace Fork
{

static std::mutex s_Lock;
static RenderStatsSnapshot s_Snapshot;
static const RenderStatsPublisher* s_Owner = nullptr;
static uint32_t s_PublishedTicks;
static uint64_t s_Generation;

RenderStatsPublisher::~RenderStatsPublisher()
{
    std::lock_guard<std::mutex> lock(s_Lock);
    if (s_Owner == this) {
        s_Owner = nullptr;
    }
}

void RenderStatsPublisher::publish(const RenderStatsSnapshot& snapshot)
{
    std::lock_guard<std::mutex> lock(s_Lock);
    s_Snapshot = snapshot;
    s_Snapshot.generation = ++s_Generation;
    s_Owner = this;
    s_PublishedTicks = SDL_GetTicks();
}

bool latestRenderStats(RenderStatsSnapshot* snapshot, uint32_t maxAgeMs)
{
    std::lock_guard<std::mutex> lock(s_Lock);
    if (s_Owner == nullptr || SDL_GetTicks() - s_PublishedTicks > maxAgeMs) {
        return false;
    }

    *snapshot = s_Snapshot;
    return true;
}

void summarizeGpuTimes(std::vector<uint64_t>& samplesNs, double* avgMs, double* p99Ms, double* maxMs)
{
    uint64_t sumNs = 0, maxNs = 0;
    for (uint64_t ns : samplesNs) {
        sumNs += ns;
        maxNs = std::max(maxNs, ns);
    }

    auto p99 = samplesNs.begin() + (samplesNs.size() - 1) * 99 / 100;
    std::nth_element(samplesNs.begin(), p99, samplesNs.end());

    *avgMs = sumNs / 1e6 / samplesNs.size();
    *p99Ms = *p99 / 1e6;
    *maxMs = maxNs / 1e6;
}

// Appends one formatted line, or nothing if the whole line doesn't fit,
// so a short buffer never ends in a cut-off line
static void appendLine(char* output, int length, int* offset,
                       SDL_PRINTF_FORMAT_STRING const char* format, ...) SDL_PRINTF_VARARG_FUNC(4);
static void appendLine(char* output, int length, int* offset, const char* format, ...)
{
    if (*offset >= length) {
        return;
    }

    va_list args;
    va_start(args, format);
    int ret = vsnprintf(&output[*offset], length - *offset, format, args);
    va_end(args);

    if (ret < 0 || ret >= length - *offset) {
        output[*offset] = 0;
        return;
    }

    *offset += ret;
}

// The overlay wraps a line wider than 1024 px and then draws all of its text without the
// outline. At the stats overlay's 20 pt that is 85 characters.
static const size_t k_OverlayLineChars = 85;

// Appends text as one line or, if it is too wide for the overlay, as several: each break goes
// after the last ", " that fits, and continuation lines are indented
static void appendWrappedLine(char* output, int length, int* offset, const char* text)
{
    std::string rest = text;
    const char* indent = "";
    while (strlen(indent) + rest.size() > k_OverlayLineChars) {
        size_t split = rest.rfind(", ", k_OverlayLineChars - strlen(indent) - 1);
        if (split == std::string::npos || split == 0) {
            break;
        }

        appendLine(output, length, offset, "%s%s\n", indent, rest.substr(0, split + 1).c_str());
        rest.erase(0, split + 2);
        indent = "  ";
    }

    appendLine(output, length, offset, "%s%s\n", indent, rest.c_str());
}

// "Scaling: 1920x1200 -> 2880x1800 (x1.50), fsr1 s0.90 (session), 0.52 ms, deband 0.21 ms, A/B, bench 5/7"
static void appendScalingLine(char* output, int length, int* offset, const RenderStatsSnapshot& stats)
{
    double ratio = (double)stats.targetWidth / stats.sourceWidth;
    char step[80];
    if (stats.targetWidth == stats.sourceWidth && stats.targetHeight == stats.sourceHeight) {
        snprintf(step, sizeof(step), "1:1");
    }
    else if (ratio < 1.0) {
        snprintf(step, sizeof(step), "downscale (x%.2f)", ratio);
    }
    else {
        // The marker belongs to the chosen scaler, so it is hidden while another one is on
        // screen (A/B bypass, a benchmark step)
        bool session = stats.session && !stats.bypass && stats.benchStep == 0;
        snprintf(step, sizeof(step), "(x%.2f), %s%s", ratio, stats.scaler, session ? " (session)" : "");
    }

    char scalingTime[24] = "";
    if (stats.scalingAvgMs > 0) {
        snprintf(scalingTime, sizeof(scalingTime), ", %.2f ms", stats.scalingAvgMs);
    }

    char debandTime[32] = "";
    if (stats.debandAvgMs > 0) {
        snprintf(debandTime, sizeof(debandTime), ", deband %.2f ms", stats.debandAvgMs);
    }

    char bench[24] = "";
    if (stats.benchStep > 0) {
        snprintf(bench, sizeof(bench), ", bench %d/%d", stats.benchStep, stats.benchTotal);
    }

    char line[256];
    snprintf(line, sizeof(line), "Scaling: %dx%d -> %dx%d %s%s%s%s%s",
             stats.sourceWidth, stats.sourceHeight, stats.targetWidth, stats.targetHeight,
             step, scalingTime, debandTime, stats.bypass ? ", A/B" : "", bench);
    appendWrappedLine(output, length, offset, line);
}

int appendOverlayStats(char* output, int length, int streamFps)
{
    // Keeps the GPU-state sampler awake while the overlay is shown. The decoder's stats log also
    // comes here when the renderer is recreated mid-stream, which must not wake it.
    Session* session = Session::get();
    if (session != nullptr && session->getOverlayManager().isOverlayEnabled(Overlay::OverlayDebug)) {
        systemStateDemand();
    }

    int offset = 0;
    RenderStatsSnapshot stats;
    bool haveStats = latestRenderStats(&stats);

    if (haveStats) {
        if (stats.gpuSamples == 0) {
            appendLine(output, length, &offset, "GPU render (video): no timer data\n");
        }
        else if (streamFps > 0) {
            appendLine(output, length, &offset,
                       "GPU render (video): %.2f ms avg / %.2f p99 / %.2f max of %.2f ms frame\n",
                       stats.gpuAvgMs, stats.gpuP99Ms, stats.gpuMaxMs, 1000.0 / streamFps);
        }
        else {
            appendLine(output, length, &offset,
                       "GPU render (video): %.2f ms avg / %.2f p99 / %.2f max\n",
                       stats.gpuAvgMs, stats.gpuP99Ms, stats.gpuMaxMs);
        }
    }

    // The power and GPU state are shown whichever renderer is active
    SystemState system;
    if (latestSystemState(&system)) {
        char state[96];
        formatSystemState(system, state, sizeof(state));
        appendLine(output, length, &offset, "GPU state: %s\n", state);
    }

    if (haveStats && stats.sourceWidth > 0) {
        appendScalingLine(output, length, &offset, stats);
    }

    // The desktop-HDR state is shown whichever renderer is active
    QByteArray desktopHdr = DesktopHdrController::statusText().toLatin1();
    if (haveStats) {
        char line[192];
        snprintf(line, sizeof(line), "Output: %dx%d %s %s%s%s",
                 stats.swapchainWidth, stats.swapchainHeight,
                 stats.swapchainTransfer, stats.swapchainPrimaries,
                 desktopHdr.isEmpty() ? "" : ", desktop HDR: ",
                 desktopHdr.constData());
        appendWrappedLine(output, length, &offset, line);
    }
    else if (!desktopHdr.isEmpty()) {
        appendLine(output, length, &offset, "Desktop HDR: %s\n", desktopHdr.constData());
    }

    return offset;
}

}
