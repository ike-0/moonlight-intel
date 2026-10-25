#include "gnomedisplayconfig.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusVariant>

#include "SDL_compat.h"

// Mutter's interface: data/dbus-interfaces/org.gnome.Mutter.DisplayConfig.xml
#define DC_SERVICE "org.gnome.Mutter.DisplayConfig"
#define DC_PATH "/org/gnome/Mutter/DisplayConfig"
#define DC_INTERFACE "org.gnome.Mutter.DisplayConfig"
#define DC_STATE_SIGNATURE "ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv}"

// ApplyMonitorsConfig replies only after the modeset
#define DBUS_TIMEOUT_MS 10000

#define SHELL_SERVICE "org.gnome.Shell"
#define SHELL_PATH "/org/gnome/Shell"
#define SHELL_INTERFACE "org.gnome.Shell"

// OverviewActive is polled while a switch waits, so it must answer quickly
#define SHELL_TIMEOUT_MS 1000

#define UNAVAILABLE_REASON "Requires GNOME 48+ with an HDR-capable display"

namespace GnomeDbus
{

// Per-monitor "color-mode" values
enum ColorMode : quint32
{
    COLOR_MODE_DEFAULT = 0,
    COLOR_MODE_BT2100 = 1,
    COLOR_MODE_SDR_NATIVE = 2,
};

// ApplyMonitorsConfig methods. Temporary isn't written to monitors.xml and, unlike
// persistent, doesn't show the "Keep these display settings?" dialog.
enum ApplyMethod : quint32
{
    METHOD_VERIFY = 0,
    METHOD_TEMPORARY = 1,
    METHOD_PERSISTENT = 2,
};

// GetCurrentState() structures

struct MonitorId // (ssss)
{
    QString connector, vendor, product, serial;

    bool operator==(const MonitorId& other) const
    {
        return connector == other.connector && vendor == other.vendor &&
               product == other.product && serial == other.serial;
    }
};

struct Mode // (siiddada{sv})
{
    QString id;
    int width, height;
    double refreshRate, preferredScale;
    QList<double> supportedScales;
    QVariantMap properties;
};

struct Monitor // ((ssss)a(siiddada{sv})a{sv})
{
    MonitorId id;
    QList<Mode> modes;
    QVariantMap properties;
};

struct LogicalMonitor // (iiduba(ssss)a{sv})
{
    int x, y;
    double scale;
    quint32 transform;
    bool primary;
    QList<MonitorId> monitors;
    QVariantMap properties;
};

struct State
{
    quint32 serial;
    QList<Monitor> monitors;
    QList<LogicalMonitor> logicalMonitors;
    QVariantMap properties;
};

// ApplyMonitorsConfig() structures

struct MonitorConfig // (ssa{sv})
{
    QString connector;
    QString modeId;
    QVariantMap properties;
};

struct LogicalMonitorConfig // (iiduba(ssa{sv}))
{
    int x, y;
    double scale;
    quint32 transform;
    bool primary;
    QList<MonitorConfig> monitors;
};

const QDBusArgument& operator>>(const QDBusArgument& arg, MonitorId& id)
{
    arg.beginStructure();
    arg >> id.connector >> id.vendor >> id.product >> id.serial;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, Mode& mode)
{
    arg.beginStructure();
    arg >> mode.id >> mode.width >> mode.height >> mode.refreshRate >> mode.preferredScale
        >> mode.supportedScales >> mode.properties;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, Monitor& monitor)
{
    arg.beginStructure();
    arg >> monitor.id >> monitor.modes >> monitor.properties;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, LogicalMonitor& logicalMonitor)
{
    arg.beginStructure();
    arg >> logicalMonitor.x >> logicalMonitor.y >> logicalMonitor.scale >> logicalMonitor.transform
        >> logicalMonitor.primary >> logicalMonitor.monitors >> logicalMonitor.properties;
    arg.endStructure();
    return arg;
}

QDBusArgument& operator<<(QDBusArgument& arg, const MonitorConfig& config)
{
    arg.beginStructure();
    arg << config.connector << config.modeId << config.properties;
    arg.endStructure();
    return arg;
}

QDBusArgument& operator<<(QDBusArgument& arg, const LogicalMonitorConfig& config)
{
    arg.beginStructure();
    arg << config.x << config.y << config.scale << config.transform << config.primary << config.monitors;
    arg.endStructure();
    return arg;
}

// qDBusRegisterMetaType() needs both directions, though the config types are only sent

const QDBusArgument& operator>>(const QDBusArgument& arg, MonitorConfig& config)
{
    arg.beginStructure();
    arg >> config.connector >> config.modeId >> config.properties;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, LogicalMonitorConfig& config)
{
    arg.beginStructure();
    arg >> config.x >> config.y >> config.scale >> config.transform >> config.primary >> config.monitors;
    arg.endStructure();
    return arg;
}

}

Q_DECLARE_METATYPE(GnomeDbus::MonitorConfig)
Q_DECLARE_METATYPE(GnomeDbus::LogicalMonitorConfig)

using namespace GnomeDbus;

static QString describeError(const QDBusMessage& reply)
{
    if (reply.type() == QDBusMessage::ErrorMessage) {
        return reply.errorName().section('.', -1) + ": " + reply.errorMessage();
    }

    return "unexpected reply signature " + reply.signature();
}

static bool readState(State* state, QString* error)
{
    QDBusMessage call = QDBusMessage::createMethodCall(DC_SERVICE, DC_PATH, DC_INTERFACE, "GetCurrentState");
    QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, DBUS_TIMEOUT_MS);

    // Checking the signature first makes API drift fail cleanly instead of demarshalling garbage
    if (reply.type() != QDBusMessage::ReplyMessage || reply.signature() != DC_STATE_SIGNATURE) {
        *error = describeError(reply);
        return false;
    }

    const QList<QVariant> args = reply.arguments();
    state->serial = args[0].toUInt();
    state->monitors = qdbus_cast<QList<Monitor>>(args[1]);
    state->logicalMonitors = qdbus_cast<QList<LogicalMonitor>>(args[2]);
    state->properties = qdbus_cast<QVariantMap>(args[3]);
    return true;
}

static const Monitor* findMonitor(const State& state, const MonitorId& id)
{
    for (const Monitor& monitor : state.monitors) {
        if (monitor.id == id) {
            return &monitor;
        }
    }

    return nullptr;
}

static QString currentModeId(const Monitor& monitor)
{
    for (const Mode& mode : monitor.modes) {
        if (mode.properties.value("is-current").toBool()) {
            return mode.id;
        }
    }

    return QString();
}

GnomeDisplayConfig::GnomeDisplayConfig()
{
    static bool s_Registered = false;
    if (!s_Registered) {
        qDBusRegisterMetaType<MonitorConfig>();
        qDBusRegisterMetaType<QList<MonitorConfig>>();
        qDBusRegisterMetaType<LogicalMonitorConfig>();
        qDBusRegisterMetaType<QList<LogicalMonitorConfig>>();
        s_Registered = true;
    }
}

bool GnomeDisplayConfig::probe(QString* reason)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || !bus.interface()->isServiceRegistered(DC_SERVICE).value()) {
        *reason = UNAVAILABLE_REASON;
        return false;
    }

    QDBusMessage call = QDBusMessage::createMethodCall(DC_SERVICE, DC_PATH,
                                                       "org.freedesktop.DBus.Properties", "Get");
    call << DC_INTERFACE << "ApplyMonitorsConfigAllowed";
    QDBusMessage reply = bus.call(call, QDBus::Block, DBUS_TIMEOUT_MS);
    if (reply.type() != QDBusMessage::ReplyMessage ||
            !qvariant_cast<QDBusVariant>(reply.arguments().value(0)).variant().toBool()) {
        *reason = "GNOME doesn't allow display configuration changes in this session";
        return false;
    }

    QList<DesktopHdrMonitor> monitors;
    QString error;
    if (!currentState(&monitors, &error)) {
        *reason = "Can't read the GNOME display configuration (" + error + ")";
        return false;
    }

    for (const DesktopHdrMonitor& monitor : monitors) {
        if (monitor.hdrSupported) {
            return true;
        }
    }

    *reason = UNAVAILABLE_REASON;
    return false;
}

quint32 GnomeDisplayConfig::hdrColorMode() const
{
    return COLOR_MODE_BT2100;
}

bool GnomeDisplayConfig::currentState(QList<DesktopHdrMonitor>* monitors, QString* error)
{
    State state;
    if (!readState(&state, error)) {
        return false;
    }

    monitors->clear();
    for (const Monitor& monitor : state.monitors) {
        DesktopHdrMonitor result;
        result.connector = monitor.id.connector;
        result.vendor = monitor.id.vendor;
        result.product = monitor.id.product;
        result.serial = monitor.id.serial;
        result.displayName = monitor.properties.value("display-name").toString();
        result.colorMode = monitor.properties.value("color-mode").toUInt();

        // Absent before GNOME 48
        QList<uint> supportedModes = qdbus_cast<QList<uint>>(monitor.properties.value("supported-color-modes"));
        result.hdrSupported = supportedModes.contains(COLOR_MODE_BT2100);

        monitors->append(result);
    }

    return true;
}

bool GnomeDisplayConfig::setColorMode(const DesktopHdrMonitor& target, quint32 mode, QString* detail)
{
    MonitorId targetId = { target.connector, target.vendor, target.product, target.serial };

    // A stale serial means the configuration changed between our read and our apply,
    // so it is worth one retry on fresh state
    for (int attempt = 0;; attempt++) {
        State state;
        if (!readState(&state, detail)) {
            return false;
        }

        // ApplyMonitorsConfig replaces the whole configuration: resend every logical monitor
        // with its current values and change only the target's colour mode
        QList<LogicalMonitorConfig> logicalMonitors;
        bool targetFound = false;
        for (const LogicalMonitor& logicalMonitor : state.logicalMonitors) {
            LogicalMonitorConfig logicalConfig = {
                logicalMonitor.x, logicalMonitor.y, logicalMonitor.scale,
                logicalMonitor.transform, logicalMonitor.primary, {}
            };

            for (const MonitorId& id : logicalMonitor.monitors) {
                const Monitor* monitor = findMonitor(state, id);
                QString modeId = monitor != nullptr ? currentModeId(*monitor) : QString();
                if (modeId.isEmpty()) {
                    *detail = "no current mode for " + id.connector;
                    return false;
                }

                MonitorConfig config = { id.connector, modeId, {} };
                const QVariantMap& properties = monitor->properties;
                if (properties.contains("is-underscanning")) {
                    config.properties["underscanning"] = properties["is-underscanning"].toBool();
                }
                if (properties.contains("rgb-range")) {
                    config.properties["rgb-range"] = properties["rgb-range"];
                }
                if (properties.contains("color-mode")) {
                    config.properties["color-mode"] = properties["color-mode"];
                }
                if (id == targetId) {
                    config.properties["color-mode"] = QVariant::fromValue<quint32>(mode);
                    targetFound = true;
                }

                logicalConfig.monitors.append(config);
            }

            logicalMonitors.append(logicalConfig);
        }

        if (!targetFound) {
            *detail = target.connector + " is not active";
            return false;
        }

        QVariantMap properties;
        if (state.properties.value("supports-changing-layout-mode").toBool() &&
                state.properties.contains("layout-mode")) {
            properties["layout-mode"] = state.properties["layout-mode"];
        }

        QDBusMessage call = QDBusMessage::createMethodCall(DC_SERVICE, DC_PATH, DC_INTERFACE, "ApplyMonitorsConfig");
        call << state.serial << (quint32)METHOD_TEMPORARY << QVariant::fromValue(logicalMonitors) << properties;
        QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, DBUS_TIMEOUT_MS);
        if (reply.type() == QDBusMessage::ReplyMessage) {
            *detail = QString("serial %1").arg(state.serial);
            return true;
        }

        if (attempt == 0 && reply.errorName() == "org.freedesktop.DBus.Error.AccessDenied" &&
                reply.errorMessage().contains("stale")) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "moonlight-intel: desktop-hdr serial %u was stale, retrying",
                        state.serial);
            continue;
        }

        *detail = QString("%1 (serial %2)").arg(describeError(reply)).arg(state.serial);
        return false;
    }
}

bool GnomeDisplayConfig::overviewActive()
{
    QDBusMessage call = QDBusMessage::createMethodCall(SHELL_SERVICE, SHELL_PATH,
                                                       "org.freedesktop.DBus.Properties", "Get");
    call << SHELL_INTERFACE << "OverviewActive";
    QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, SHELL_TIMEOUT_MS);

    // Without an answer, don't hold switches back
    return reply.type() == QDBusMessage::ReplyMessage &&
           qvariant_cast<QDBusVariant>(reply.arguments().value(0)).variant().toBool();
}
