#pragma once

#include "desktophdrbackend.h"
#include "intelpreferences.h"

#include <QObject>
#include <QString>

class QThread;
class QTimer;

// moonlight-intel: switches the display that hosts the stream to HDR while the host sends HDR,
// and restores its previous colour mode afterwards. A marker file in $XDG_RUNTIME_DIR records
// each switch, so the next launch can undo it after a crash. Desktop specifics live in
// DesktopHdrBackend; this class is platform-independent.
//
// One app-wide instance. The object lives on a worker thread that runs every backend call
// (a GNOME modeset blocks for hundreds of ms), plus the timers. The public methods are
// thread-safe and only queue work, except sessionEnded().
class DesktopHdrController : public QObject
{
    Q_OBJECT

public:
    static DesktopHdrController* get();

    // Starts the worker, which probes the backend and runs crash recovery (GUI thread, once)
    void start();

    // Stops the worker (GUI thread, at application exit)
    void stop();

    // Session hooks. The display name is SDL's name for the display hosting the window.
    // "Focused" is keyboard focus, which GNOME takes away for the overview, another workspace
    // or another window.
    void sessionStarted(IntelPreferences::DesktopHdrMode mode, bool fullscreen, bool focused, const QString& displayName);
    void notifyHostHdr(bool enabled);
    void notifyWindowState(bool fullscreen, bool focused, const QString& displayName);

    // Restores the display and waits up to 3 s for it, since the process may exit right after
    void sessionEnded();

    // Short state for the stats overlay, e.g. "on (auto, eDP-1)". Empty outside a session.
    static QString statusText();

signals:
    void availabilityChanged(bool available, const QString& reason);

private:
    enum class Phase
    {
        Idle,     // no session
        Inactive, // setting off, or no usable display
        Armed,    // waiting for the enter condition
        On,       // we switched the target to HDR and owe a restore
        Passive,  // the target was already in HDR: never touch it this session
        Failed,   // a switch failed: no more attempts this session
    };

    DesktopHdrController();

    // Worker-thread side
    void initializeWorker();
    void startSession(IntelPreferences::DesktopHdrMode mode, bool fullscreen, bool focused, const QString& displayName);
    void endSession();
    void evaluate();
    void onTransitionTimer();
    void enter();
    void leave();
    void checkRendererFallback();
    bool resolveTarget(const QList<DesktopHdrMonitor>& monitors, DesktopHdrMonitor* target);
    bool streamInFront() const;
    bool wantHdr() const;
    QString waitingStatus() const;
    void setStatus(const QString& status);
    void fail(const QString& reason);

    bool writeMarker();
    void removeMarker();
    void recoverFromMarker();

    QThread* m_Thread = nullptr;
    QTimer* m_TransitionTimer;
    QTimer* m_FallbackTimer;

    DesktopHdrBackend* m_Backend = nullptr;
    bool m_Available = false;
    QString m_MarkerPath;

    // Per session
    Phase m_Phase = Phase::Idle;
    IntelPreferences::DesktopHdrMode m_Mode = IntelPreferences::DHM_OFF;
    bool m_HostHdr = false;
    bool m_Fullscreen = false;
    bool m_Focused = false;
    QString m_DisplayName;
    DesktopHdrMonitor m_Target;
    quint32 m_PreviousMode = 0;
    bool m_Restored = false;
    bool m_FallbackUsed = false;
};
