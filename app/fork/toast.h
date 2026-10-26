#pragma once

#include "SDL_compat.h"

namespace Fork
{

// moonlight-intel: short in-stream messages on upstream's status overlay (red, bottom left),
// hidden after 1.5 s. A new toast replaces the one showing. A toast never covers or clears a
// message it didn't set: while upstream shows one (connection warning, gamepad mouse mode),
// toasts are skipped. Any thread.
//
// The overlay wraps lines wider than 1024 px, and then drops the outline. At 36 pt that is about
// 47 characters, so put longer toasts on two lines.
void showToast(SDL_PRINTF_FORMAT_STRING const char* format, ...) SDL_PRINTF_VARARG_FUNC(1);

// Toasts only show between these (fork.cpp session hooks)
void toastSessionStarted();
void toastSessionEnded();

}
