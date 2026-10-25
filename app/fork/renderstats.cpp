#include "renderstats.h"
#include "desktophdr.h"
#include "fork.h"

#include "SDL_compat.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

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

int appendOverlayStats(char* output, int length, int streamFps)
{
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

        if (stats.sourceWidth > 0) {
            double ratio = (double)stats.targetWidth / stats.sourceWidth;
            char step[64];
            if (stats.targetWidth == stats.sourceWidth && stats.targetHeight == stats.sourceHeight) {
                snprintf(step, sizeof(step), "1:1");
            }
            else if (ratio < 1.0) {
                snprintf(step, sizeof(step), "downscale (x%.2f)", ratio);
            }
            else {
                snprintf(step, sizeof(step), "(x%.2f), %s", ratio, stats.scaler);
            }

            char scalingTime[32] = "";
            if (stats.scalingAvgMs > 0) {
                snprintf(scalingTime, sizeof(scalingTime), ", %.2f ms", stats.scalingAvgMs);
            }

            appendLine(output, length, &offset, "Scaling: %dx%d -> %dx%d %s%s\n",
                       stats.sourceWidth, stats.sourceHeight,
                       stats.targetWidth, stats.targetHeight,
                       step, scalingTime);
        }
    }

    // The desktop-HDR state is shown whichever renderer is active
    QByteArray desktopHdr = DesktopHdrController::statusText().toLatin1();
    if (haveStats) {
        appendLine(output, length, &offset, "Output: %dx%d %s %s%s%s\n",
                   stats.swapchainWidth, stats.swapchainHeight,
                   stats.swapchainTransfer, stats.swapchainPrimaries,
                   desktopHdr.isEmpty() ? "" : ", desktop HDR: ",
                   desktopHdr.constData());
    }
    else if (!desktopHdr.isEmpty()) {
        appendLine(output, length, &offset, "Desktop HDR: %s\n", desktopHdr.constData());
    }

    return offset;
}

}
