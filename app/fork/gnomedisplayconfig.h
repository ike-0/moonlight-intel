#pragma once

#include "desktophdrbackend.h"

// moonlight-intel: desktop-HDR backend for GNOME, over Mutter's org.gnome.Mutter.DisplayConfig
// D-Bus interface (colour modes need GNOME 48+). Only built on Linux with QtDBus.
class GnomeDisplayConfig : public DesktopHdrBackend
{
public:
    GnomeDisplayConfig();

    bool probe(QString* reason) override;
    quint32 hdrColorMode() const override;
    bool currentState(QList<DesktopHdrMonitor>* monitors, QString* error) override;
    bool setColorMode(const DesktopHdrMonitor& target, quint32 mode, QString* detail) override;
};
