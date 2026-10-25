#include "intelpreferences.h"

#include <QSettings>

#define SER_GROUP "moonlight-intel"
#define SER_DESKTOPHDRMODE "desktopHdrMode"
#define SER_LOGGPUTIMING "logGpuTiming"
#define SER_VULKANFORSDR "vulkanForSdr"

static void writeSetting(const char* key, const QVariant& value)
{
    QSettings settings;
    settings.beginGroup(SER_GROUP);
    settings.setValue(key, value);
}

IntelPreferences::IntelPreferences()
{
    QSettings settings;
    settings.beginGroup(SER_GROUP);

    int mode = settings.value(SER_DESKTOPHDRMODE, DHM_HDR_STREAMS).toInt();
    if (mode < DHM_OFF || mode > DHM_HDR_STREAMS_FULLSCREEN) {
        mode = DHM_HDR_STREAMS;
    }

    m_DesktopHdrMode = (DesktopHdrMode)mode;
    m_LogGpuTiming = settings.value(SER_LOGGPUTIMING, false).toBool();
    m_VulkanForSdr = settings.value(SER_VULKANFORSDR, true).toBool();
}

IntelPreferences* IntelPreferences::get()
{
    // Created by Fork::initialize() on the GUI thread, never destroyed
    static IntelPreferences* s_Instance = new IntelPreferences();
    return s_Instance;
}

void IntelPreferences::setDesktopHdrMode(DesktopHdrMode mode)
{
    if (m_DesktopHdrMode.exchange(mode) != mode) {
        writeSetting(SER_DESKTOPHDRMODE, (int)mode);
        emit desktopHdrModeChanged();
    }
}

void IntelPreferences::setLogGpuTiming(bool enabled)
{
    if (m_LogGpuTiming.exchange(enabled) != enabled) {
        writeSetting(SER_LOGGPUTIMING, enabled);
        emit logGpuTimingChanged();
    }
}

void IntelPreferences::setVulkanForSdr(bool enabled)
{
    if (m_VulkanForSdr.exchange(enabled) != enabled) {
        writeSetting(SER_VULKANFORSDR, enabled);
        emit vulkanForSdrChanged();
    }
}

void IntelPreferences::setDesktopHdrAvailability(bool available, const QString& reason)
{
    if (available != m_DesktopHdrAvailable || reason != m_DesktopHdrUnavailableReason) {
        m_DesktopHdrAvailable = available;
        m_DesktopHdrUnavailableReason = reason;
        emit desktopHdrAvailabilityChanged();
    }
}
