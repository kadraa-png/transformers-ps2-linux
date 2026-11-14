#pragma once
// Host platform layer (SDL3): window, input, audio output, timing.
#include "ps2/types.h"
#include <string>
#include <vector>

namespace platform {

struct Config {
    int window_scale = 2;      // initial window size = 640x448 * scale (PAL 640x512)
    int window_width = 0;      // explicit initial window size (overrides window_scale)
    int window_height = 0;
    bool widescreen = false;   // 16:9 window when no explicit size is given
    bool fullscreen = false;
    float mouse_sensitivity = 1.0f;   // mouse -> right stick gain
    bool mouse_invert_y = false;
    bool mouse_stick = false;         // --mouse-stick: mouse drives the right stick only
    bool pal = true;           // 50 Hz timing
};

bool init(const Config& cfg);
void shutdown();
void* sdl_window();            // SDL_Window*, created without an SDL renderer

// Pump host events; returns false when the user asked to quit.
bool poll_events();

// DualShock 2 state for port 0/1 in the exact layout libpad reports:
//   buttons: active-low 16-bit mask (bit order of the PS2 pad protocol:
//            SELECT L3 R3 START UP RIGHT DOWN LEFT L2 R2 L1 R1 TRIANGLE
//            CIRCLE CROSS SQUARE, bit 0 = SELECT)
//   analog sticks 0..255 (0x80 centre): rx, ry, lx, ly
//   pressure 0..255 for the 12 pressure-sensitive buttons in protocol order:
//            RIGHT LEFT UP DOWN TRIANGLE CIRCLE CROSS SQUARE L1 R1 L2 R2
struct PadState {
    bool connected;
    u16 buttons;
    u8 rx, ry, lx, ly;
    u8 pressure[12];
};
PadState pad_state(int port);
void pad_rumble(int port, u8 small_motor, u8 large_motor);

// Audio: interleaved stereo s16 at 48000 Hz.  Push from the emulation thread;
// the device pulls on its own thread.  Returns queued sample frames.
void audio_push(const s16* frames, u32 nframes);
u32 audio_queued_frames();

// Direct mouse look, consumed by the hooked game camera controller (see
// CAMERA_HOOKS in tools/recomp_ee.py): mouse counts since the last call times
// --mouse-sens, y negated for --invert-mouse.  `recent`: the mouse moved within
// the last second.  While the hook keeps taking motion the mouse no longer
// drives the right stick.
void mouse_camera_take(float& dx, float& dy, bool& recent);

// PS2_CONTROL pipe: a pending screenshot request ("shot <path>").
bool take_control_shot(std::string& path);
// PS2_CONTROL pipe: a pending memory dump request ("dump <path>").
bool take_control_dump(std::string& path);
// PS2_CONTROL pipe: pending debug commands ("watch <addr>", "watchreport").
std::vector<std::string> take_control_debug();

// Monotonic host time in nanoseconds.
u64 now_ns();

} // namespace platform
