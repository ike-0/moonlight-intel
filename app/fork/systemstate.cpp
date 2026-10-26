#include "systemstate.h"

#include <QDir>
#include <QFileInfo>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

namespace Fork
{

// First line of a sysfs file, without the newline. False if it can't be read.
static bool readLine(const std::string& path, std::string* value)
{
    FILE* file = fopen(path.c_str(), "r");
    if (file == nullptr) {
        return false;
    }

    char buffer[256];
    bool ok = fgets(buffer, sizeof(buffer), file) != nullptr;
    fclose(file);
    if (!ok) {
        return false;
    }

    buffer[strcspn(buffer, "\n")] = 0;
    *value = buffer;
    return true;
}

static bool readNumber(const std::string& path, long long* value)
{
    std::string text;
    if (!readLine(path, &text)) {
        return false;
    }

    char* end;
    *value = strtoll(text.c_str(), &end, 10);
    return end != text.c_str();
}

SystemStateReader::SystemStateReader(const std::string& sysRoot) :
    m_SysRoot(sysRoot)
{
    QString supplyRoot = QString::fromStdString(sysRoot + "/class/power_supply");
    for (const QString& name : QDir(supplyRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        std::string dir = (supplyRoot + "/" + name).toStdString();
        std::string type;
        if (!readLine(dir + "/type", &type)) {
            continue;
        }

        if (type == "Mains") {
            m_Mains.push_back(dir);
        }
        else if (type == "Battery") {
            m_Batteries.push_back(dir);
        }
    }

    // GT0 of the xe card is the render GT (rcs, ccs and bcs engines); GT1 is the media GT
    QString drmRoot = QString::fromStdString(sysRoot + "/class/drm");
    for (const QString& name : QDir(drmRoot).entryList({ "card*" }, QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (name.contains('-')) {
            continue; // a connector, e.g. card0-eDP-1
        }

        QString device = drmRoot + "/" + name + "/device";
        if (QFileInfo(QFileInfo(device + "/driver").canonicalFilePath()).fileName() == "xe") {
            m_Gt0FreqDir = (device + "/tile0/gt0/freq0").toStdString();
            break;
        }
    }
}

SystemStateReader::Sample SystemStateReader::read() const
{
    Sample sample = {};

    // No online Mains supply means battery, unless there is no battery at all (a desktop)
    sample.onAc = m_Batteries.empty();
    for (const std::string& dir : m_Mains) {
        long long online;
        if (readNumber(dir + "/online", &online) && online == 1) {
            sample.onAc = true;
            break;
        }
    }

    // On AC the battery's power is its charge rate, so it is only read on battery
    sample.batteryW = -1;
    if (!sample.onAc) {
        double totalW = 0;
        bool known = false;
        for (const std::string& dir : m_Batteries) {
            long long powerUw, currentUa, voltageUv;
            if (readNumber(dir + "/power_now", &powerUw)) {
                totalW += llabs(powerUw) / 1e6;
                known = true;
            }
            else if (readNumber(dir + "/current_now", &currentUa) && readNumber(dir + "/voltage_now", &voltageUv)) {
                totalW += llabs(currentUa) / 1e6 * (voltageUv / 1e6);
                known = true;
            }
        }
        if (known) {
            sample.batteryW = totalW;
        }
    }

    readLine(m_SysRoot + "/firmware/acpi/platform_profile", &sample.profile);

    sample.gt0MHz = -1;
    if (!m_Gt0FreqDir.empty()) {
        long long mhz;
        if (readNumber(m_Gt0FreqDir + "/act_freq", &mhz)) {
            sample.gt0MHz = (int)mhz;
        }

        // Space-separated reason names ("pl1 thermal"), or "none"
        std::string reasons;
        if (readLine(m_Gt0FreqDir + "/throttle/reasons", &reasons)) {
            sample.throttleKnown = true;
            size_t start = reasons.find_first_not_of(" ,\t");
            while (start != std::string::npos) {
                size_t end = reasons.find_first_of(" ,\t", start);
                std::string reason = reasons.substr(start, end == std::string::npos ? std::string::npos : end - start);
                if (reason.compare(0, 7, "reason_") == 0) {
                    reason.erase(0, 7);
                }
                if (reason != "none") {
                    sample.throttle.push_back(reason);
                }
                start = reasons.find_first_not_of(" ,\t", end);
            }
        }
    }

    return sample;
}

void formatSystemState(const SystemState& state, char* text, size_t length)
{
    char power[32];
    if (state.onAc) {
        snprintf(power, sizeof(power), "AC");
    }
    else if (state.batteryW >= 0) {
        snprintf(power, sizeof(power), "battery %.1f W", state.batteryW);
    }
    else {
        snprintf(power, sizeof(power), "battery");
    }

    char gt0[24];
    if (state.gt0MHz > 0) {
        snprintf(gt0, sizeof(gt0), "%d MHz", state.gt0MHz);
    }
    else {
        snprintf(gt0, sizeof(gt0), "%s", state.gt0MHz == 0 ? "idle" : "n/a");
    }

    snprintf(text, length, "%s, %s, GT0 %s", power, state.profile, gt0);
}

namespace
{

// Folds samples into a SystemState
class Accumulator
{
public:
    void add(const SystemStateReader::Sample& sample)
    {
        m_Samples++;
        m_Last = sample;

        if (sample.batteryW >= 0) {
            m_BatterySumW += sample.batteryW;
            m_BatterySamples++;
        }

        // act_freq reads 0 while the GT is in RC6 between frames, so only running samples count
        if (sample.gt0MHz >= 0) {
            m_FreqKnown = true;
            if (sample.gt0MHz > 0) {
                m_FreqSumMHz += sample.gt0MHz;
                m_FreqSamples++;
            }
        }

        if (sample.throttleKnown) {
            m_ThrottleKnown = true;
            m_Throttle.insert(sample.throttle.begin(), sample.throttle.end());
        }
    }

    bool result(SystemState* state) const
    {
        *state = {};
        state->samples = m_Samples;
        if (m_Samples == 0) {
            return false;
        }

        state->onAc = m_Last.onAc;
        state->batteryW = !m_Last.onAc && m_BatterySamples > 0 ? m_BatterySumW / m_BatterySamples : -1;
        snprintf(state->profile, sizeof(state->profile), "%s",
                 m_Last.profile.empty() ? "n/a" : m_Last.profile.c_str());

        if (!m_FreqKnown) {
            state->gt0MHz = -1;
        }
        else {
            state->gt0MHz = m_FreqSamples > 0 ? (int)((m_FreqSumMHz + m_FreqSamples / 2) / m_FreqSamples) : 0;
        }

        std::string throttle;
        for (const std::string& reason : m_Throttle) {
            throttle += (throttle.empty() ? "" : " ") + reason;
        }
        snprintf(state->throttle, sizeof(state->throttle), "%s",
                 !m_ThrottleKnown ? "n/a" : throttle.empty() ? "none" : throttle.c_str());
        return true;
    }

private:
    int m_Samples = 0;
    SystemStateReader::Sample m_Last = {};
    double m_BatterySumW = 0;
    int m_BatterySamples = 0;
    bool m_FreqKnown = false;
    long long m_FreqSumMHz = 0;
    int m_FreqSamples = 0;
    bool m_ThrottleKnown = false;
    std::set<std::string> m_Throttle;
};

}

// A thread of its own rather than an SDL timer: battery reads go through ACPI and take up to
// ~15 ms about once a second, which would hold up every other SDL timer
static std::mutex s_Lock;
static std::condition_variable s_Wake;
static std::thread s_Thread;
static bool s_Running = false;
static Accumulator s_Second;
static Accumulator s_Window;
static SystemState s_Latest;
static bool s_HaveLatest = false;

static void samplerMain()
{
    SystemStateReader reader;

    std::unique_lock<std::mutex> lock(s_Lock);
    for (int tick = 1; s_Running; tick++) {
        lock.unlock();
        SystemStateReader::Sample sample = reader.read();
        lock.lock();

        s_Second.add(sample);
        s_Window.add(sample);

        // Publish the first sample right away, then once a second
        if (tick == 1 || tick % 4 == 0) {
            s_HaveLatest = s_Second.result(&s_Latest);
            s_Second = Accumulator();
        }

        s_Wake.wait_for(lock, std::chrono::milliseconds(250), [] { return !s_Running; });
    }
}

void startSystemSampler()
{
    stopSystemSampler();

    std::lock_guard<std::mutex> lock(s_Lock);
    s_Running = true;
    s_Second = Accumulator();
    s_Window = Accumulator();
    s_HaveLatest = false;
    s_Thread = std::thread(samplerMain);
}

void stopSystemSampler()
{
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        s_Running = false;
        s_HaveLatest = false;
    }

    s_Wake.notify_all();
    if (s_Thread.joinable()) {
        s_Thread.join();
    }
}

bool latestSystemState(SystemState* state)
{
    std::lock_guard<std::mutex> lock(s_Lock);
    if (!s_HaveLatest) {
        return false;
    }

    *state = s_Latest;
    return true;
}

void beginSystemStateWindow()
{
    std::lock_guard<std::mutex> lock(s_Lock);
    s_Window = Accumulator();
}

bool systemStateWindow(SystemState* state)
{
    std::lock_guard<std::mutex> lock(s_Lock);
    return s_Window.result(state);
}

}
