#pragma once

#include <QObject>
#include <QString>

#include <atomic>

// moonlight-intel: fork-owned settings. They live in the [moonlight-intel] group of the shared
// Moonlight.conf, which stock Moonlight ignores, and are written through on every change.
// Getters are atomic loads, so renderer and decoder code may read them from any thread.
class IntelPreferences : public QObject
{
    Q_OBJECT

public:
    // Persisted as integers: append new values at the end
    enum DesktopHdrMode
    {
        DHM_OFF,
        DHM_HDR_STREAMS,
        DHM_HDR_STREAMS_FULLSCREEN
    };
    Q_ENUM(DesktopHdrMode)

    // Persisted as integers: append new values at the end. The order is also the hotkey cycle
    // and benchmark order (fork/scaler.h).
    enum Upscaler
    {
        SC_BILINEAR,
        SC_SPLINE36,
        SC_LANCZOS,
        SC_EWA_LANCZOSSHARP,
        SC_FSR1,
        SC_NIS,
        SC_RAVU_ZOOM_AR_R2
    };
    Q_ENUM(Upscaler)

    // Sharpness is 0-1, higher is sharper. The defaults are the shaders' own defaults.
    static constexpr double kDefaultFsrSharpness = 0.90;
    static constexpr double kDefaultNisSharpness = 0.25;

    Q_PROPERTY(DesktopHdrMode desktopHdrMode READ desktopHdrMode WRITE setDesktopHdrMode NOTIFY desktopHdrModeChanged)
    Q_PROPERTY(bool logGpuTiming READ logGpuTiming WRITE setLogGpuTiming NOTIFY logGpuTimingChanged)
    Q_PROPERTY(bool vulkanForSdr READ vulkanForSdr WRITE setVulkanForSdr NOTIFY vulkanForSdrChanged)
    Q_PROPERTY(bool desktopHdrAvailable READ desktopHdrAvailable NOTIFY desktopHdrAvailabilityChanged)
    Q_PROPERTY(QString desktopHdrUnavailableReason READ desktopHdrUnavailableReason NOTIFY desktopHdrAvailabilityChanged)
    Q_PROPERTY(Upscaler upscaler READ upscaler WRITE setUpscaler NOTIFY upscalerChanged)
    Q_PROPERTY(double fsrSharpness READ fsrSharpness WRITE setFsrSharpness NOTIFY fsrSharpnessChanged)
    Q_PROPERTY(double nisSharpness READ nisSharpness WRITE setNisSharpness NOTIFY nisSharpnessChanged)
    Q_PROPERTY(bool debanding READ debanding WRITE setDebanding NOTIFY debandingChanged)

    static IntelPreferences* get();

    DesktopHdrMode desktopHdrMode() const { return m_DesktopHdrMode; }
    void setDesktopHdrMode(DesktopHdrMode mode);

    bool logGpuTiming() const { return m_LogGpuTiming; }
    void setLogGpuTiming(bool enabled);

    bool vulkanForSdr() const { return m_VulkanForSdr; }
    void setVulkanForSdr(bool enabled);

    // Fed by the desktop-HDR controller once its backend probe has run (GUI thread only)
    bool desktopHdrAvailable() const { return m_DesktopHdrAvailable; }
    QString desktopHdrUnavailableReason() const { return m_DesktopHdrUnavailableReason; }
    void setDesktopHdrAvailability(bool available, const QString& reason);

    // Upscaler defaults for each stream; in-stream hotkeys never write them
    Upscaler upscaler() const { return m_Upscaler; }
    void setUpscaler(Upscaler upscaler);

    double fsrSharpness() const { return m_FsrSharpness; }
    void setFsrSharpness(double sharpness);

    double nisSharpness() const { return m_NisSharpness; }
    void setNisSharpness(double sharpness);

    bool debanding() const { return m_Debanding; }
    void setDebanding(bool enabled);

signals:
    void desktopHdrModeChanged();
    void logGpuTimingChanged();
    void vulkanForSdrChanged();
    void desktopHdrAvailabilityChanged();
    void upscalerChanged();
    void fsrSharpnessChanged();
    void nisSharpnessChanged();
    void debandingChanged();

private:
    IntelPreferences();

    std::atomic<DesktopHdrMode> m_DesktopHdrMode;
    std::atomic<bool> m_LogGpuTiming;
    std::atomic<bool> m_VulkanForSdr;
    std::atomic<Upscaler> m_Upscaler;
    std::atomic<double> m_FsrSharpness;
    std::atomic<double> m_NisSharpness;
    std::atomic<bool> m_Debanding;
    bool m_DesktopHdrAvailable = false;
    QString m_DesktopHdrUnavailableReason;
};
