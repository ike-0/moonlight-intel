#pragma once

#include <QList>
#include <QString>

// moonlight-intel: one display as a desktop-HDR backend sees it
struct DesktopHdrMonitor
{
    // Identity: a monitor is the same only if all four match. Only the connector may be logged.
    QString connector;
    QString vendor;
    QString product;
    QString serial;

    QString displayName;   // user-facing name, e.g. "Built-in display"
    quint32 colorMode = 0; // backend-specific value, restored verbatim
    bool hdrSupported = false;

    bool isSameMonitor(const DesktopHdrMonitor& other) const
    {
        return connector == other.connector && vendor == other.vendor &&
               product == other.product && serial == other.serial;
    }
};

// Switches individual displays between SDR and HDR through the desktop's own display
// configuration. GNOME (Mutter D-Bus) only for now; KDE or Windows backends would implement
// the same interface. Implementations may block and are only called on the controller's
// worker thread.
class DesktopHdrBackend
{
public:
    virtual ~DesktopHdrBackend() = default;

    // Returns nullptr when this platform or build has no backend
    static DesktopHdrBackend* create();

    // Whether switching can work in this session. On failure *reason is a short
    // user-facing explanation for the settings page.
    virtual bool probe(QString* reason) = 0;

    // The colorMode value that means HDR output
    virtual quint32 hdrColorMode() const = 0;

    virtual bool currentState(QList<DesktopHdrMonitor>* monitors, QString* error) = 0;

    // Sets one monitor's colour mode and keeps the rest of the configuration. The change must
    // be temporary: not persisted and without a confirmation prompt. On success *detail holds
    // backend specifics for the log (GNOME: the configuration serial), on failure the error.
    virtual bool setColorMode(const DesktopHdrMonitor& target, quint32 mode, QString* detail) = 0;
};
