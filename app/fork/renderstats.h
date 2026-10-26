#pragma once

#include <cstdint>
#include <vector>

namespace Fork
{

// moonlight-intel: render statistics for the stats overlay and the desktop-HDR controller,
// published by PlVkRenderer about once a second. Plain data, so app/fork/ needs no libplacebo
// headers: plvk fills in the colour-space short names itself.
struct RenderStatsSnapshot
{
    // Video-only GPU time over the window ("overlay" passes excluded). gpuSamples == 0 means
    // libplacebo had no timer data yet.
    int gpuSamples;
    double gpuAvgMs, gpuP99Ms, gpuMaxMs;

    // Average GPU time per frame of scaler passes (0 while scaling is plain texture sampling)
    // and of debanding passes. Both are part of the video time, not on top of it.
    double scalingAvgMs;
    double debandAvgMs;

    int sourceWidth, sourceHeight;  // source crop
    int targetWidth, targetHeight;  // video rect on the swapchain, after aspect-ratio fitting
    char scaler[40];                // name and parameters, e.g. "fsr1 s0.90", "nis failed -> bilinear"
    bool session;                   // scaler or sharpness differs from the saved settings
    bool bypass;                    // A/B bypass to bilinear
    int benchStep, benchTotal;      // running benchmark step, 0 = none

    int swapchainWidth, swapchainHeight;
    char swapchainTransfer[16];
    char swapchainPrimaries[16];
    bool swapchainHdr;
    bool sourceHdr;

    uint64_t generation;  // set by publish(); increases with every publish
};

// Owned by a renderer. Destroying it clears the shared snapshot if it was the last writer,
// so a renderer that no longer exists never shows.
class RenderStatsPublisher
{
public:
    RenderStatsPublisher() = default;
    ~RenderStatsPublisher();

    RenderStatsPublisher(const RenderStatsPublisher&) = delete;
    RenderStatsPublisher& operator=(const RenderStatsPublisher&) = delete;

    void publish(const RenderStatsSnapshot& snapshot);
};

// Copies the latest snapshot if one was published in the last maxAgeMs. Thread-safe.
bool latestRenderStats(RenderStatsSnapshot* snapshot, uint32_t maxAgeMs = 3000);

// Average, p99 and max in ms of GPU times in ns. Reorders the samples, which must not be empty.
void summarizeGpuTimes(std::vector<uint64_t>& samplesNs, double* avgMs, double* p99Ms, double* maxMs);

}
