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

    Q_PROPERTY(DesktopHdrMode desktopHdrMode READ desktopHdrMode WRITE setDesktopHdrMode NOTIFY desktopHdrModeChanged)
    Q_PROPERTY(bool logGpuTiming READ logGpuTiming WRITE setLogGpuTiming NOTIFY logGpuTimingChanged)
    Q_PROPERTY(bool vulkanForSdr READ vulkanForSdr WRITE setVulkanForSdr NOTIFY vulkanForSdrChanged)
    Q_PROPERTY(bool desktopHdrAvailable READ desktopHdrAvailable NOTIFY desktopHdrAvailabilityChanged)
    Q_PROPERTY(QString desktopHdrUnavailableReason READ desktopHdrUnavailableReason NOTIFY desktopHdrAvailabilityChanged)

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

signals:
    void desktopHdrModeChanged();
    void logGpuTimingChanged();
    void vulkanForSdrChanged();
    void desktopHdrAvailabilityChanged();

private:
    IntelPreferences();

    std::atomic<DesktopHdrMode> m_DesktopHdrMode;
    std::atomic<bool> m_LogGpuTiming;
    std::atomic<bool> m_VulkanForSdr;
    bool m_DesktopHdrAvailable = false;
    QString m_DesktopHdrUnavailableReason;
};
