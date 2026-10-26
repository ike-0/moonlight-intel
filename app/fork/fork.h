#pragma once

// moonlight-intel: the fork module's entry points. Upstream files include only this header,
// so each hook into upstream code stays a single, greppable line.

struct SDL_Window;

namespace Fork
{

// Registers the IntelPreferences QML singleton and starts the desktop-HDR controller, which
// runs crash recovery. Call once on the GUI thread, before the QML engine loads.
void initialize();

// Held around every commit of the stream window's Wayland surface, so commits from different
// threads never interleave. Mesa's Vulkan present (render thread) builds its commit over several
// requests, and with explicit sync a commit from the frame-pacing vsync source (main thread) in
// between is a protocol error that makes the compositor drop the connection.
class SurfaceCommitLock
{
public:
    SurfaceCommitLock();
    ~SurfaceCommitLock();

    SurfaceCommitLock(const SurfaceCommitLock&) = delete;
    SurfaceCommitLock& operator=(const SurfaceCommitLock&) = delete;
};

// Settings read on the decoder/renderer path (lock-free)
bool preferVulkanForSdr();
bool logGpuTiming();

// Appends the fork's stats overlay lines (GPU render, GPU state, scaling, output and desktop
// HDR) at output, which has room for length bytes. Only whole lines are written. Returns the
// bytes added.
int appendOverlayStats(char* output, int length, int streamFps);

// Session hooks (session.cpp): desktop HDR, the upscaler config, toasts and the GPU state
// sampler. Only sessionEnded() blocks, for up to 3 s.
void sessionStarted(SDL_Window* window);
void notifyHostHdr(bool enabled);
void notifyWindowState(SDL_Window* window);
void sessionEnded();

// In-stream hotkeys (keyboard.cpp): Ctrl+Alt+Shift+<key> down, with the key's SDL keycode and
// scancode. Returns true if the fork consumed the key, which then never reaches the host.
bool handleStreamHotkey(int sym, int scancode);

}
