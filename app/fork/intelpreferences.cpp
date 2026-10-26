#include "intelpreferences.h"

#include <QSettings>
#include <QtGlobal>

#define SER_GROUP "moonlight-intel"
#define SER_DESKTOPHDRMODE "desktopHdrMode"
#define SER_LOGGPUTIMING "logGpuTiming"
#define SER_VULKANFORSDR "vulkanForSdr"
#define SER_UPSCALER "upscaler"
#define SER_FSRSHARPNESS "fsrSharpness"
#define SER_NISSHARPNESS "nisSharpness"
#define SER_DEBANDING "debanding"

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

    int upscaler = settings.value(SER_UPSCALER, SC_BILINEAR).toInt();
    if (upscaler < SC_BILINEAR || upscaler > SC_RAVU_ZOOM_AR_R2) {
        upscaler = SC_BILINEAR;
    }

    m_Upscaler = (Upscaler)upscaler;
    m_FsrSharpness = qBound(0.0, settings.value(SER_FSRSHARPNESS, kDefaultFsrSharpness).toDouble(), 1.0);
    m_NisSharpness = qBound(0.0, settings.value(SER_NISSHARPNESS, kDefaultNisSharpness).toDouble(), 1.0);
    m_Debanding = settings.value(SER_DEBANDING, false).toBool();
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

void IntelPreferences::setUpscaler(Upscaler upscaler)
{
    if (m_Upscaler.exchange(upscaler) != upscaler) {
        writeSetting(SER_UPSCALER, (int)upscaler);
        emit upscalerChanged();
    }
}

void IntelPreferences::setFsrSharpness(double sharpness)
{
    // Hundredths, the resolution the stream uses
    sharpness = qRound(qBound(0.0, sharpness, 1.0) * 100) / 100.0;
    if (m_FsrSharpness.exchange(sharpness) != sharpness) {
        writeSetting(SER_FSRSHARPNESS, sharpness);
        emit fsrSharpnessChanged();
    }
}

void IntelPreferences::setNisSharpness(double sharpness)
{
    // Hundredths, the resolution the stream uses
    sharpness = qRound(qBound(0.0, sharpness, 1.0) * 100) / 100.0;
    if (m_NisSharpness.exchange(sharpness) != sharpness) {
        writeSetting(SER_NISSHARPNESS, sharpness);
        emit nisSharpnessChanged();
    }
}

void IntelPreferences::setDebanding(bool enabled)
{
    if (m_Debanding.exchange(enabled) != enabled) {
        writeSetting(SER_DEBANDING, enabled);
        emit debandingChanged();
    }
}
