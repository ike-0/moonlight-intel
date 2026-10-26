#include "systemstate.h"

#include <QDir>
#include <QFileInfo>

#ifdef HAVE_UPOWER
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#endif

#include "SDL_compat.h"

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

bool SystemStateReader::readOnAc() const
{
    // No online Mains supply means battery, unless there is no battery at all (a desktop)
    for (const std::string& dir : m_Mains) {
        long long online;
        if (readNumber(dir + "/online", &online) && online == 1) {
            return true;
        }
    }

    return m_Batteries.empty();
}

SystemStateReader::Sample SystemStateReader::read(bool onAc, bool batteryPower) const
{
    Sample sample = {};
    sample.onAc = onAc;

    // On AC the battery's power is its charge rate, so it is only read on battery
    sample.batteryW = -1;
    if (batteryPower && !onAc) {
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

#ifdef HAVE_UPOWER
#define UPOWER_SERVICE "org.freedesktop.UPower"
#define UPOWER_PATH "/org/freedesktop/UPower"
#define UPOWER_INTERFACE "org.freedesktop.UPower"
#define UPOWER_TIMEOUT_MS 250

// upowerd's cached OnBattery: neither this process nor upowerd evaluates ACPI for it
static bool readUPowerOnBattery(bool* onBattery, QString* error)
{
    QDBusMessage call = QDBusMessage::createMethodCall(UPOWER_SERVICE, UPOWER_PATH,
                                                       "org.freedesktop.DBus.Properties", "Get");
    call << UPOWER_INTERFACE << "OnBattery";
    QDBusMessage reply = QDBusConnection::systemBus().call(call, QDBus::Block, UPOWER_TIMEOUT_MS);
    if (reply.type() != QDBusMessage::ReplyMessage) {
        *error = reply.errorName().section('.', -1) + ": " + reply.errorMessage();
        return false;
    }

    QVariant value = qvariant_cast<QDBusVariant>(reply.arguments().value(0)).variant();
    if (value.userType() != QMetaType::Bool) {
        *error = QString("OnBattery is not a bool (%1)").arg(value.typeName());
        return false;
    }

    *onBattery = value.toBool();
    return true;
}
#endif

namespace
{

// AC or battery: UPower, or sysfs once per wake-up of the sampler if UPower fails
class PowerSource
{
public:
    explicit PowerSource(const SystemStateReader& reader) :
        m_Reader(reader)
    {
    }

    // A new wake-up: UPower gets another chance, and sysfs may be read once more
    void wake()
    {
        m_Read = false;
        m_SysfsRead = false;
#ifdef HAVE_UPOWER
        m_UPower = true;
#endif
    }

    // The first call after wake() always reads; later ones ask UPower again only with refresh set
    bool onAc(bool refresh)
    {
        if (m_Read && !refresh) {
            return m_OnAc;
        }
        m_Read = true;

#ifdef HAVE_UPOWER
        if (m_UPower) {
            bool onBattery;
            QString error;
            if (readUPowerOnBattery(&onBattery, &error)) {
                m_OnAc = !onBattery;
                return m_OnAc;
            }

            // Logged once per stream; each overlay toggle would repeat it
            m_UPower = false;
            if (!m_LoggedFailure) {
                m_LoggedFailure = true;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "moonlight-intel: UPower OnBattery unavailable (%s), power source from sysfs",
                            qPrintable(error));
            }
        }
#endif

        // Without UPower an AC change only shows at the next wake-up
        if (!m_SysfsRead) {
            m_SysfsRead = true;
            m_OnAc = m_Reader.readOnAc();
        }
        return m_OnAc;
    }

private:
    const SystemStateReader& m_Reader;
    bool m_Read = false;
    bool m_SysfsRead = false;
    bool m_OnAc = true;
#ifdef HAVE_UPOWER
    bool m_UPower = true;
    bool m_LoggedFailure = false;
#endif
};

}

// How long one overlay build keeps the sampler awake
static const std::chrono::milliseconds k_DemandTimeout(3000);

// A thread of its own rather than an SDL timer: the UPower call can block for up to 250 ms, and
// a benchmark's battery reads go through ACPI and take up to ~15 ms about once a second. Either
// would hold up every other SDL timer.
static std::mutex s_Lock;
static std::condition_variable s_Wake;
static std::thread s_Thread;
static bool s_Running = false;
static bool s_Benchmark = false;
static std::chrono::steady_clock::time_point s_LastDemand;
static Accumulator s_Second;
static Accumulator s_Window;
static SystemState s_Latest;
static bool s_HaveLatest = false;

// Called with s_Lock held
static bool samplerWanted()
{
    return s_Benchmark ||
           (s_LastDemand != std::chrono::steady_clock::time_point() &&
            std::chrono::steady_clock::now() - s_LastDemand < k_DemandTimeout);
}

static void samplerMain()
{
    SystemStateReader reader;
    PowerSource power(reader);

    std::unique_lock<std::mutex> lock(s_Lock);
    while (s_Running) {
        // Idle: no reads. A later overlay must not show minutes-old state.
        s_HaveLatest = false;
        s_Second = Accumulator();
        s_Wake.wait(lock, [] { return !s_Running || samplerWanted(); });

        power.wake();
        for (int tick = 1; s_Running && samplerWanted(); tick++) {
            // Publish the first sample right away, then once a second
            bool publish = tick == 1 || tick % 4 == 0;
            bool benchmark = s_Benchmark;

            lock.unlock();
            bool onAc = power.onAc(publish);
            SystemStateReader::Sample sample = reader.read(onAc, benchmark);
            lock.lock();

            s_Second.add(sample);
            s_Window.add(sample);

            if (publish) {
                s_HaveLatest = s_Second.result(&s_Latest);
                s_Second = Accumulator();
            }

            s_Wake.wait_for(lock, std::chrono::milliseconds(250), [] { return !s_Running; });
        }
    }
}

void startSystemSampler()
{
    stopSystemSampler();

    std::lock_guard<std::mutex> lock(s_Lock);
    s_Running = true;
    s_Benchmark = false;
    s_LastDemand = std::chrono::steady_clock::time_point();
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

void systemStateDemand()
{
    // Under the lock, so an idle sampler can't miss the wake-up between its check and its wait
    std::lock_guard<std::mutex> lock(s_Lock);
    if (s_Running) {
        s_LastDemand = std::chrono::steady_clock::now();
        s_Wake.notify_all();
    }
}

void setSystemStateBenchmark(bool running)
{
    std::lock_guard<std::mutex> lock(s_Lock);
    s_Benchmark = running;
    s_Wake.notify_all();
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
