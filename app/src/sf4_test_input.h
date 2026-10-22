// sf4 - automated tests on the PC: a virtual pad driven by a button script (from nfsmw-nx's nfsmw_prueba_entrada).
//
// With sf4_test_buttons (for example "8:start,12:start,16-18:a") the app adds to the SDK input system a synthetic
// device that presses those buttons at those seconds after the input system is created. It takes the game past
// the intro and the menus unattended, so the Direct3D trace (sf4_d3d_trace) sees menus and fights.
//
// InputSystem::GetState merges all devices of user 0 and the synthetic ones go to that user, so it coexists with
// the real pad and keyboard. Empty (the default) adds nothing.

#pragma once

#include <rex/runtime.h>

namespace sf4::test_input {

// Wraps config.input_factory to add the script pad if the cvar is not empty. Call it in OnPreSetup, once ReXApp
// has installed the default factory.
void WrapInput(rex::RuntimeConfig& config);

}  // namespace sf4::test_input
