#include "fork.h"
#include "desktophdr.h"
#include "intelpreferences.h"

#include <QCoreApplication>
#include <QQmlEngine>

#include "SDL_compat.h"

#include <mutex>

namespace Fork
{

static bool isFullscreen(SDL_Window* window)
{
    // Also matches SDL_WINDOW_FULLSCREEN_DESKTOP
    return SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN;
}

static bool isFocused(SDL_Window* window)
{
    return SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS;
}

static QString displayName(SDL_Window* window)
{
    const char* name = SDL_GetDisplayName(SDL_GetWindowDisplayIndex(window));
    return name != nullptr ? QString::fromUtf8(name) : QString();
}

void initialize()
{
    IntelPreferences* preferences = IntelPreferences::get();
    qmlRegisterSingletonType<IntelPreferences>("IntelPreferences", 1, 0,
                                               "IntelPreferences",
                                               [](QQmlEngine*, QJSEngine*) -> QObject* {
                                                   // Not owned by the engine: C++ uses it too
                                                   QQmlEngine::setObjectOwnership(IntelPreferences::get(), QQmlEngine::CppOwnership);
                                                   return IntelPreferences::get();
                                               });

    DesktopHdrController* controller = DesktopHdrController::get();
    QObject::connect(controller, &DesktopHdrController::availabilityChanged,
                     preferences, &IntelPreferences::setDesktopHdrAvailability);
    QObject::connect(qApp, &QCoreApplication::aboutToQuit,
                     preferences, [controller] { controller->stop(); });
    controller->start();
}

bool preferVulkanForSdr()
{
    return IntelPreferences::get()->vulkanForSdr();
}

static std::mutex s_SurfaceCommitLock;

SurfaceCommitLock::SurfaceCommitLock()
{
    s_SurfaceCommitLock.lock();
}

SurfaceCommitLock::~SurfaceCommitLock()
{
    s_SurfaceCommitLock.unlock();
}

bool logGpuTiming()
{
    return IntelPreferences::get()->logGpuTiming();
}

void sessionStarted(SDL_Window* window)
{
    DesktopHdrController::get()->sessionStarted(IntelPreferences::get()->desktopHdrMode(),
                                                isFullscreen(window), isFocused(window),
                                                displayName(window));
}

void notifyHostHdr(bool enabled)
{
    DesktopHdrController::get()->notifyHostHdr(enabled);
}

void notifyWindowState(SDL_Window* window)
{
    DesktopHdrController::get()->notifyWindowState(isFullscreen(window), isFocused(window),
                                                   displayName(window));
}

void sessionEnded()
{
    DesktopHdrController::get()->sessionEnded();
}

}
