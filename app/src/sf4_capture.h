// sf4 - captures of the game image for tests (from nfsmw-nx's nfsmw_nativo_captura). With sf4_capture_every_s > 0
// it saves a PNG every N seconds in captures/ next to the executable, taken from the presenter's guest output, so
// a scripted run (sf4_test_buttons) can be checked without looking at the screen.

#pragma once

#include <functional>

namespace rex::ui {
class Presenter;
}

namespace sf4::capture {

// Starts the capture thread if the cvar asks for it. The presenter is requested on every capture: it may not
// exist yet at startup.
void Start(std::function<rex::ui::Presenter*()> get_presenter);

// Stops the thread (idempotent).
void Stop();

}  // namespace sf4::capture
