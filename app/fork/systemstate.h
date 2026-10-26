#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace Fork
{

// moonlight-intel: power and GPU state for the stats overlay's "GPU state" line and the
// benchmark log. A sampler thread runs during a stream but only reads while the overlay is shown
// or a benchmark runs. The battery is never read: ACPI battery reads stutter on the target. A
// missing file reads as n/a; nothing here fails.
struct SystemState
{
    int samples;          // 0 = no data
    bool onAc;            // at the last sample
    char profile[24];     // ACPI platform profile at the last sample, or "n/a"
    int gt0MHz;           // average GT0 act_freq of the samples where it was running;
                          // 0 = idle (RC6) in every sample, -1 = n/a
    char throttle[96];    // union of GT0 throttle reasons: "none", e.g. "pl1 thermal", or "n/a"
};

// "AC, balanced, GT0 1450 MHz" or "battery, low-power, GT0 1100 MHz"
void formatSystemState(const SystemState& state, char* text, size_t length);

// One sysfs reading. The root is a parameter so the readers can run against a fake tree.
class SystemStateReader
{
public:
    struct Sample
    {
        bool onAc;
        std::string profile;  // empty = n/a
        int gt0MHz;           // -1 = n/a
        bool throttleKnown;
        std::vector<std::string> throttle;
    };

    // Finds the power supplies and the xe GT0 once
    explicit SystemStateReader(const std::string& sysRoot = "/sys");

    // AC or battery from the Mains supplies' online files. The ACPI AC driver evaluates _PSR on
    // every read, so this is only the fallback for UPower.
    bool readOnAc() const;

    // One sample of the platform profile and GT0, on the given power source
    Sample read(bool onAc) const;

private:
    std::string m_SysRoot;
    std::vector<std::string> m_Mains;      // power_supply directories by type
    std::vector<std::string> m_Batteries;
    std::string m_Gt0FreqDir;              // .../device/tile0/gt0/freq0 of the xe card, or empty
};

// Sampler lifetime: one stream (fork.cpp). It starts idle and reads nothing until one of the
// two calls below wakes it.
void startSystemSampler();
void stopSystemSampler();

// The overlay is being built (renderstats.cpp): sample for the next 3 s. Never starts a stopped
// sampler.
void systemStateDemand();

// A benchmark sweep runs (scaler.cpp): sample with the overlay hidden too
void setSystemStateBenchmark(bool running);

// The last completed second. False before the first second, while idle or without a running
// sampler.
bool latestSystemState(SystemState* state);

// Benchmark measure window: begin, then read everything sampled since
void beginSystemStateWindow();
bool systemStateWindow(SystemState* state);

}
