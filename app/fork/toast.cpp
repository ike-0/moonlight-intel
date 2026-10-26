#include "toast.h"

#include "streaming/session.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace Fork
{

static std::mutex s_Lock;
static bool s_Active = false;   // a session's overlay manager is alive
static SDL_TimerID s_HideTimer = 0;
static uintptr_t s_Sequence = 0; // identifies the toast a hide timer belongs to
static char s_Text[128];        // the toast showing while s_HideTimer != 0

static Uint32 hideToast(Uint32, void* param)
{
    std::lock_guard<std::mutex> lock(s_Lock);

    // A newer toast or the session end got here first
    if (!s_Active || (uintptr_t)param != s_Sequence) {
        return 0;
    }
    s_HideTimer = 0;

    // Only hide our own text: an upstream message may have replaced it meanwhile
    Overlay::OverlayManager& overlays = Session::get()->getOverlayManager();
    if (overlays.isOverlayEnabled(Overlay::OverlayStatusUpdate) &&
            strcmp(overlays.getOverlayText(Overlay::OverlayStatusUpdate), s_Text) == 0) {
        overlays.setOverlayState(Overlay::OverlayStatusUpdate, false);
    }

    return 0;
}

void showToast(const char* format, ...)
{
    char text[sizeof(s_Text)];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(s_Lock);
    Session* session = Session::get();
    if (!s_Active || session == nullptr) {
        return;
    }

    Overlay::OverlayManager& overlays = session->getOverlayManager();
    bool ownToastShowing = s_HideTimer != 0 &&
            strcmp(overlays.getOverlayText(Overlay::OverlayStatusUpdate), s_Text) == 0;
    if (overlays.isOverlayEnabled(Overlay::OverlayStatusUpdate) && !ownToastShowing) {
        return;
    }

    if (s_HideTimer != 0) {
        SDL_RemoveTimer(s_HideTimer);
    }

    snprintf(s_Text, sizeof(s_Text), "%s", text);
    overlays.updateOverlayText(Overlay::OverlayStatusUpdate, s_Text);
    overlays.setOverlayState(Overlay::OverlayStatusUpdate, true);
    s_HideTimer = SDL_AddTimer(1500, hideToast, (void*)++s_Sequence);
}

void toastSessionStarted()
{
    std::lock_guard<std::mutex> lock(s_Lock);
    s_Active = true;
}

void toastSessionEnded()
{
    std::lock_guard<std::mutex> lock(s_Lock);
    s_Active = false;
    s_Sequence++;
    if (s_HideTimer != 0) {
        SDL_RemoveTimer(s_HideTimer);
        s_HideTimer = 0;
    }
}

}
