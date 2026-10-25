#include "desktophdr.h"
#include "renderstats.h"

#ifdef HAVE_GNOME_DISPLAY_CONFIG
#include "gnomedisplayconfig.h"
#endif

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QSaveFile>
#include <QSemaphore>
#include <QThread>
#include <QTimer>

#include <memory>

#include "SDL_compat.h"

#ifdef Q_OS_UNIX
#include <errno.h>
#include <signal.h>
#endif

// Debounce, in ms. Entering in "during HDR streams" mode needs no hold time.
#define ENTER_DELAY_FULLSCREEN_ONLY_MS 2000
#define LEAVE_DELAY_HOST_SDR_MS 3000
#define LEAVE_DELAY_NOT_IN_FRONT_MS 2000

// How long after a switch the swapchain must be HDR before the renderer is recreated
#define FALLBACK_CHECK_DELAY_MS 2000

#define SESSION_END_TIMEOUT_MS 3000

#define MAX_REASON_LENGTH 48

static QMutex s_StatusLock;
static QString s_Status;

DesktopHdrBackend* DesktopHdrBackend::create()
{
#ifdef HAVE_GNOME_DISPLAY_CONFIG
    return new GnomeDisplayConfig();
#else
    return nullptr;
#endif
}

static bool isProcessRunning(qint64 pid)
{
#ifdef Q_OS_UNIX
    if (pid <= 0 || pid == QCoreApplication::applicationPid()) {
        return false;
    }

    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
#else
    Q_UNUSED(pid);
    return false;
#endif
}

static const DesktopHdrMonitor* findMonitor(const QList<DesktopHdrMonitor>& monitors, const DesktopHdrMonitor& wanted)
{
    for (const DesktopHdrMonitor& monitor : monitors) {
        if (monitor.isSameMonitor(wanted)) {
            return &monitor;
        }
    }

    return nullptr;
}

DesktopHdrController* DesktopHdrController::get()
{
    // Created by Fork::initialize() on the GUI thread, never destroyed
    static DesktopHdrController* s_Instance = new DesktopHdrController();
    return s_Instance;
}

DesktopHdrController::DesktopHdrController()
{
    // Children move to the worker thread together with this object
    m_TransitionTimer = new QTimer(this);
    m_TransitionTimer->setSingleShot(true);
    connect(m_TransitionTimer, &QTimer::timeout, this, &DesktopHdrController::onTransitionTimer);

    m_FallbackTimer = new QTimer(this);
    m_FallbackTimer->setSingleShot(true);
    connect(m_FallbackTimer, &QTimer::timeout, this, &DesktopHdrController::checkRendererFallback);
}

void DesktopHdrController::start()
{
    m_Thread = new QThread();
    m_Thread->setObjectName("DesktopHdr");
    moveToThread(m_Thread);
    m_Thread->start();

    QMetaObject::invokeMethod(this, [this] { initializeWorker(); }, Qt::QueuedConnection);
}

void DesktopHdrController::stop()
{
    if (m_Thread != nullptr) {
        m_Thread->quit();
        m_Thread->wait();
    }
}

void DesktopHdrController::sessionStarted(IntelPreferences::DesktopHdrMode mode, bool fullscreen, bool focused, const QString& displayName)
{
    QMetaObject::invokeMethod(this, [this, mode, fullscreen, focused, displayName] {
        startSession(mode, fullscreen, focused, displayName);
    }, Qt::QueuedConnection);
}

void DesktopHdrController::notifyHostHdr(bool enabled)
{
    QMetaObject::invokeMethod(this, [this, enabled] {
        if (m_Phase != Phase::Idle && m_HostHdr != enabled) {
            m_HostHdr = enabled;
            evaluate();
        }
    }, Qt::QueuedConnection);
}

void DesktopHdrController::notifyWindowState(bool fullscreen, bool focused, const QString& displayName)
{
    QMetaObject::invokeMethod(this, [this, fullscreen, focused, displayName] {
        if (m_Phase == Phase::Idle) {
            return;
        }

        // A new display only matters for the next switch, so it doesn't need an evaluation.
        // An empty name means SDL couldn't tell; keep the last one.
        if (!displayName.isEmpty()) {
            m_DisplayName = displayName;
        }
        if (m_Fullscreen != fullscreen || m_Focused != focused) {
            m_Fullscreen = fullscreen;
            m_Focused = focused;
            evaluate();
        }
    }, Qt::QueuedConnection);
}

void DesktopHdrController::sessionEnded()
{
    // Shared, so a restore that outlives the wait doesn't release a destroyed semaphore
    auto done = std::make_shared<QSemaphore>();
    QMetaObject::invokeMethod(this, [this, done] {
        endSession();
        done->release();
    }, Qt::QueuedConnection);

    if (!done->tryAcquire(1, SESSION_END_TIMEOUT_MS)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "moonlight-intel: desktop-hdr restore still running after %d ms",
                    SESSION_END_TIMEOUT_MS);
    }
}

QString DesktopHdrController::statusText()
{
    QMutexLocker locker(&s_StatusLock);
    return s_Status;
}

void DesktopHdrController::setStatus(const QString& status)
{
    QMutexLocker locker(&s_StatusLock);
    s_Status = status;
}

void DesktopHdrController::initializeWorker()
{
    QString reason;
    QString runtimeDir = qEnvironmentVariable("XDG_RUNTIME_DIR");

    m_Backend = DesktopHdrBackend::create();
    if (m_Backend == nullptr) {
        reason = "Only supported on GNOME";
    }
    else if (runtimeDir.isEmpty()) {
        reason = "Needs XDG_RUNTIME_DIR for crash recovery, which is not set";
    }
    else if (m_Backend->probe(&reason)) {
        m_Available = true;
        m_MarkerPath = runtimeDir + "/moonlight-intel/desktop-hdr.json";
        recoverFromMarker();
    }

    if (m_Available) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "moonlight-intel: desktop-hdr available");
    }
    else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "moonlight-intel: desktop-hdr unavailable: %s",
                    qPrintable(reason));
    }

    emit availabilityChanged(m_Available, reason);
}

void DesktopHdrController::startSession(IntelPreferences::DesktopHdrMode mode, bool fullscreen, bool focused, const QString& displayName)
{
    // Sessions always end before the next starts, but never keep a display in HDR across them
    if (m_Phase != Phase::Idle) {
        endSession();
    }

    m_Mode = mode;
    m_HostHdr = false; // the session reports the host state once its decoder exists
    m_Fullscreen = fullscreen;
    m_Focused = focused;
    m_DisplayName = displayName;
    m_Restored = false;
    m_FallbackUsed = false;

    if (mode == IntelPreferences::DHM_OFF) {
        m_Phase = Phase::Inactive;
        setStatus("off (setting)");
    }
    else if (!m_Available) {
        m_Phase = Phase::Inactive;
        setStatus("unavailable");
    }
    else {
        m_Phase = Phase::Armed;
        setStatus(waitingStatus());
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "moonlight-intel: desktop-hdr session start mode=%d: %s",
                (int)mode, qPrintable(statusText()));
}

void DesktopHdrController::endSession()
{
    m_TransitionTimer->stop();
    m_FallbackTimer->stop();

    if (m_Phase == Phase::On) {
        leave();
    }

    m_Phase = Phase::Idle;
    m_HostHdr = false;
    setStatus(QString());
}

// Fullscreen-only mode wants the stream fullscreen and in front: the overview, another
// workspace or Alt+Tab keep the window fullscreen but take its focus
bool DesktopHdrController::streamInFront() const
{
    return m_Fullscreen && m_Focused;
}

bool DesktopHdrController::wantHdr() const
{
    return m_HostHdr && (m_Mode == IntelPreferences::DHM_HDR_STREAMS || streamInFront());
}

QString DesktopHdrController::waitingStatus() const
{
    if (m_Restored) {
        return "restored";
    }

    if (!m_HostHdr) {
        return "waiting for host HDR";
    }

    return m_Fullscreen ? "waiting for focus" : "waiting for fullscreen";
}

// Starts or cancels the debounce timer for the transition the inputs ask for. The condition
// must hold until the timer fires; any change back cancels it.
void DesktopHdrController::evaluate()
{
    if (m_Phase != Phase::Armed && m_Phase != Phase::On) {
        return;
    }

    bool wanted = wantHdr();
    if (wanted == (m_Phase == Phase::On)) {
        m_TransitionTimer->stop();
        if (m_Phase == Phase::Armed) {
            setStatus(waitingStatus());
        }
        return;
    }

    if (!m_TransitionTimer->isActive()) {
        bool fullscreenOnly = m_Mode == IntelPreferences::DHM_HDR_STREAMS_FULLSCREEN;
        int delayMs;
        if (wanted) {
            delayMs = fullscreenOnly ? ENTER_DELAY_FULLSCREEN_ONLY_MS : 0;
        }
        else {
            delayMs = (fullscreenOnly && !streamInFront()) ? LEAVE_DELAY_NOT_IN_FRONT_MS : LEAVE_DELAY_HOST_SDR_MS;
        }
        m_TransitionTimer->start(delayMs);
    }
}

void DesktopHdrController::onTransitionTimer()
{
    bool wanted = wantHdr();
    if (wanted && m_Phase == Phase::Armed) {
        enter();
    }
    else if (!wanted && m_Phase == Phase::On) {
        leave();
    }
}

// The target is the monitor hosting the window, matched by SDL's display name (Mutter's
// display-name on Wayland, the connector on X11). Without a match, the only HDR-capable
// monitor is used.
bool DesktopHdrController::resolveTarget(const QList<DesktopHdrMonitor>& monitors, DesktopHdrMonitor* target)
{
    const DesktopHdrMonitor* match = nullptr;
    int matches = 0;
    for (const DesktopHdrMonitor& monitor : monitors) {
        if (!m_DisplayName.isEmpty() &&
                (monitor.connector == m_DisplayName || monitor.displayName == m_DisplayName)) {
            match = &monitor;
            matches++;
        }
    }

    if (matches != 1) {
        match = nullptr;
        matches = 0;
        for (const DesktopHdrMonitor& monitor : monitors) {
            if (monitor.hdrSupported) {
                match = &monitor;
                matches++;
            }
        }
    }

    if (matches != 1 || !match->hdrSupported) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "moonlight-intel: desktop-hdr no single HDR-capable target for display '%s' (%d candidates)",
                    qPrintable(m_DisplayName), matches);
        return false;
    }

    *target = *match;
    return true;
}

void DesktopHdrController::fail(const QString& reason)
{
    m_Phase = Phase::Failed;
    setStatus("failed: " + (reason.length() > MAX_REASON_LENGTH ? reason.left(MAX_REASON_LENGTH) + "..." : reason));
}

void DesktopHdrController::enter()
{
    QList<DesktopHdrMonitor> monitors;
    QString detail;
    if (!m_Backend->currentState(&monitors, &detail)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "moonlight-intel: desktop-hdr enter: can't read state: %s",
                    qPrintable(detail));
        fail(detail);
        return;
    }

    DesktopHdrMonitor target;
    if (!resolveTarget(monitors, &target)) {
        m_Phase = Phase::Inactive;
        setStatus("unavailable");
        return;
    }

    quint32 hdrMode = m_Backend->hdrColorMode();
    if (target.colorMode == hdrMode) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "moonlight-intel: desktop-hdr %s already in HDR, leaving it alone this session",
                    qPrintable(target.connector));
        m_Phase = Phase::Passive;
        setStatus("on (already on)");
        return;
    }

    m_Target = target;
    m_PreviousMode = target.colorMode;

    // Without the marker a crash would leave the display in HDR, so don't switch without it
    if (!writeMarker()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "moonlight-intel: desktop-hdr enter: can't write %s",
                    qPrintable(m_MarkerPath));
        fail("can't write marker file");
        return;
    }

    setStatus("switching...");
    bool ok = m_Backend->setColorMode(target, hdrMode, &detail);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "moonlight-intel: desktop-hdr enter %s color-mode %u->%u: %s (%s)",
                qPrintable(target.connector), m_PreviousMode, hdrMode,
                ok ? "ok" : "failed", qPrintable(detail));

    if (!ok) {
        removeMarker();
        fail(detail);
        return;
    }

    m_Phase = Phase::On;
    m_Restored = false;
    setStatus("on (auto, " + target.connector + ")");

    if (!m_FallbackUsed) {
        m_FallbackTimer->start(FALLBACK_CHECK_DELAY_MS);
    }
}

void DesktopHdrController::leave()
{
    m_FallbackTimer->stop();

    QList<DesktopHdrMonitor> monitors;
    QString detail;
    if (!m_Backend->currentState(&monitors, &detail)) {
        // Keep the marker: the next launch retries the restore
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "moonlight-intel: desktop-hdr leave: can't read state: %s",
                    qPrintable(detail));
        fail(detail);
        return;
    }

    quint32 hdrMode = m_Backend->hdrColorMode();
    const DesktopHdrMonitor* current = findMonitor(monitors, m_Target);
    if (current == nullptr || current->colorMode != hdrMode) {
        // The user changed it during the stream, or the monitor is gone: leave it as it is
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "moonlight-intel: desktop-hdr leave %s: %s, not restoring",
                    qPrintable(m_Target.connector),
                    current == nullptr ? "monitor gone" : "no longer in HDR");
        removeMarker();
        m_Phase = Phase::Armed;
        setStatus(waitingStatus());
        return;
    }

    bool ok = m_Backend->setColorMode(*current, m_PreviousMode, &detail);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "moonlight-intel: desktop-hdr leave %s color-mode %u->%u: %s (%s)",
                qPrintable(m_Target.connector), hdrMode, m_PreviousMode,
                ok ? "ok" : "failed", qPrintable(detail));

    if (!ok) {
        // Keep the marker: the next launch retries the restore
        fail(detail);
        return;
    }

    removeMarker();
    m_Phase = Phase::Armed;
    m_Restored = true;
    setStatus(waitingStatus());
}

// If libplacebo cached the surface formats from before the switch, the swapchain stays SDR
// until it is recreated. Recreate the renderer once per session in that case.
void DesktopHdrController::checkRendererFallback()
{
    Fork::RenderStatsSnapshot stats;
    if (m_Phase != Phase::On || m_FallbackUsed ||
            !Fork::latestRenderStats(&stats) || !stats.sourceHdr || stats.swapchainHdr) {
        return;
    }

    m_FallbackUsed = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "moonlight-intel: desktop-hdr recreating renderer for HDR swapchain");

    SDL_Event event = {};
    event.type = SDL_RENDER_DEVICE_RESET;
    SDL_PushEvent(&event);
}

// The marker holds the target's identity, its previous mode and our pid. It lives in the
// runtime dir, which lasts exactly as long as the login session and with it Mutter's
// temporary configuration, so a stale marker can't outlive the state it describes.
bool DesktopHdrController::writeMarker()
{
    QJsonObject marker;
    marker["connector"] = m_Target.connector;
    marker["vendor"] = m_Target.vendor;
    marker["product"] = m_Target.product;
    marker["serial"] = m_Target.serial;
    marker["previousMode"] = (qint64)m_PreviousMode;
    marker["pid"] = QCoreApplication::applicationPid();

    if (!QDir().mkpath(QFileInfo(m_MarkerPath).path())) {
        return false;
    }

    QSaveFile file(m_MarkerPath);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    file.write(QJsonDocument(marker).toJson(QJsonDocument::Compact));
    return file.commit();
}

void DesktopHdrController::removeMarker()
{
    QFile::remove(m_MarkerPath);
}

void DesktopHdrController::recoverFromMarker()
{
    QFile file(m_MarkerPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    QJsonObject marker = QJsonDocument::fromJson(file.readAll()).object();
    file.close();

    qint64 pid = marker["pid"].toVariant().toLongLong();
    if (isProcessRunning(pid)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "moonlight-intel: desktop-hdr recovery: marker belongs to running pid %lld, leaving it",
                    pid);
        return;
    }

    DesktopHdrMonitor target;
    target.connector = marker["connector"].toString();
    target.vendor = marker["vendor"].toString();
    target.product = marker["product"].toString();
    target.serial = marker["serial"].toString();
    QJsonValue previousMode = marker["previousMode"];

    QList<DesktopHdrMonitor> monitors;
    QString detail;
    const DesktopHdrMonitor* current = nullptr;
    if (target.connector.isEmpty() || !previousMode.isDouble()) {
        detail = "invalid marker";
    }
    else if (!m_Backend->currentState(&monitors, &detail)) {
        detail = "can't read state: " + detail;
    }
    else if ((current = findMonitor(monitors, target)) == nullptr) {
        detail = "monitor gone";
    }
    else if (current->colorMode != m_Backend->hdrColorMode()) {
        detail = "no longer in HDR";
    }
    else {
        bool ok = m_Backend->setColorMode(*current, (quint32)previousMode.toInt(), &detail);
        detail = QString("color-mode %1->%2: %3 (%4)")
                     .arg(current->colorMode).arg(previousMode.toInt())
                     .arg(ok ? "ok" : "failed").arg(detail);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "moonlight-intel: desktop-hdr recovery %s (pid %lld): %s",
                qPrintable(target.connector), pid, qPrintable(detail));

    // Once only, even if the restore failed, so a bad marker can't retry on every launch
    removeMarker();
}
