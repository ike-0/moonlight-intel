#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace Fork
{

// moonlight-intel: power and GPU state for the stats overlay's "GPU state" line and the
// benchmark log. A sampler thread reads sysfs at 4 Hz while a stream runs. A missing file reads
// as n/a; nothing here fails.
struct SystemState
{
    int samples;          // 0 = no data
    bool onAc;            // at the last sample
    double batteryW;      // average discharge power on battery; < 0 = n/a or on AC
    char profile[24];     // ACPI platform profile at the last sample, or "n/a"
    int gt0MHz;           // average GT0 act_freq of the samples where it was running;
                          // 0 = idle (RC6) in every sample, -1 = n/a
    char throttle[96];    // union of GT0 throttle reasons: "none", e.g. "pl1 thermal", or "n/a"
};

// "AC, balanced, GT0 1450 MHz" or "battery 8.4 W, low-power, GT0 1100 MHz"
void formatSystemState(const SystemState& state, char* text, size_t length);

// One sysfs reading. The root is a parameter so the readers can run against a fake tree.
class SystemStateReader
{
public:
    struct Sample
    {
        bool onAc;
        double batteryW;      // < 0 = n/a
        std::string profile;  // empty = n/a
        int gt0MHz;           // -1 = n/a
        bool throttleKnown;
        std::vector<std::string> throttle;
    };

    // Finds the power supplies and the xe GT0 once
    explicit SystemStateReader(const std::string& sysRoot = "/sys");

    Sample read() const;

private:
    std::string m_SysRoot;
    std::vector<std::string> m_Mains;      // power_supply directories by type
    std::vector<std::string> m_Batteries;
    std::string m_Gt0FreqDir;              // .../device/tile0/gt0/freq0 of the xe card, or empty
};

// Sampler lifetime: one stream (fork.cpp)
void startSystemSampler();
void stopSystemSampler();

// The last completed second. False before the first second or without a running sampler.
bool latestSystemState(SystemState* state);

// Benchmark measure window: begin, then read everything sampled since
void beginSystemStateWindow();
bool systemStateWindow(SystemState* state);

}
