#pragma once

#include <cstdint>

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

    // Average GPU time per frame of scaler passes; 0 while scaling is plain texture sampling
    double scalingAvgMs;

    int sourceWidth, sourceHeight;  // source crop
    int targetWidth, targetHeight;  // video rect on the swapchain, after aspect-ratio fitting
    char scaler[32];

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

}
