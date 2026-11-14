#include "ps2/platform.h"
#include "ps2/gs.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <filesystem>
#include <mutex>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <vector>

namespace platform {
namespace {
SDL_Window* window = nullptr;
SDL_AudioStream* audio = nullptr;
SDL_Gamepad* pads[2] = {};
SDL_JoystickID pad_ids[2] = {};
bool running = false;
bool fullscreen = false;

// Mouse look: while the mouse is captured (relative mode), its motion drives
// the right stick and its buttons map to pad buttons.  A left click in the
// window captures it (that click is not passed on); Escape or losing focus
// releases it.
bool mouse_captured = false;
bool swallow_left = false;         // the click that captured the mouse is still held
u32 mouse_buttons = 0;             // SDL_BUTTON_MASK bits currently held
float mouse_dx = 0, mouse_dy = 0;  // motion since the last vblank
float mouse_vx_prev = 0, mouse_vy_prev = 0;
u64 mouse_last_ns = 0;
int mouse_rx = 0, mouse_ry = 0;    // right-stick deflection -127..127, updated per vblank
float mouse_sensitivity = 1.0f;
bool mouse_invert_y = false;
// "look <x counts/s> <y counts/s> <secs>" control requests: a fake steady
// mouse motion for unattended testing.
std::mutex look_mutex;
float look_vx = 0, look_vy = 0;
u64 look_until_ns = 0;
// Direct mouse look (mouse_camera_take): motion not yet taken by the camera
// hook, when the hook last took it, and when the mouse last moved.
bool mouse_stick_only = false;
float cam_dx = 0, cam_dy = 0;
u64 cam_take_ns = 0, mouse_move_ns = 0;
constexpr u64 kCamHookAlive = 250'000'000;   // ns without a take -> back to stick emulation

// Pad protocol bits: SELECT L3 R3 START UP RIGHT DOWN LEFT L2 R2 L1 R1 TRIANGLE CIRCLE CROSS SQUARE.
enum Button : unsigned {
    SELECT, L3, R3, START, UP, RIGHT, DOWN, LEFT,
    L2, R2, L1, R1, TRIANGLE, CIRCLE, CROSS, SQUARE
};
constexpr Button pressure_buttons[] = {
    RIGHT, LEFT, UP, DOWN, TRIANGLE, CIRCLE, CROSS, SQUARE, L1, R1, L2, R2
};

void press(PadState& state, Button button) {
    state.buttons &= static_cast<u16>(~(1u << button));
}

void open_pad(SDL_JoystickID id) {
    for (int slot = 0; slot < 2; ++slot) {
        if (pad_ids[slot] == id) return;
    }
    for (int slot = 0; slot < 2; ++slot) {
        if (!pads[slot]) {
            SDL_Gamepad* pad = SDL_OpenGamepad(id);
            if (!pad) {
                std::fprintf(stderr, "SDL_OpenGamepad: %s\n", SDL_GetError());
                return;
            }
            pads[slot] = pad;
            pad_ids[slot] = id;
            return;
        }
    }
}

void fill_free_slots() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (!ids) return;
    for (int i = 0; i < count; ++i) open_pad(ids[i]);
    SDL_free(ids);
}

void remove_pad(SDL_JoystickID id) {
    for (int slot = 0; slot < 2; ++slot) {
        if (pad_ids[slot] == id) {
            SDL_CloseGamepad(pads[slot]);
            pads[slot] = nullptr;
            pad_ids[slot] = 0;
            break;
        }
    }
    if (!pads[0] && pads[1]) {
        pads[0] = pads[1];
        pad_ids[0] = pad_ids[1];
        pads[1] = nullptr;
        pad_ids[1] = 0;
    }
    fill_free_slots();
}

u8 stick_axis(Sint16 value) {
    // SDL sticks are signed -32768..32767; the PS2 sticks use unsigned 0..255.
    return static_cast<u8>((static_cast<int>(value) + 32768) >> 8);
}

void apply_gamepad(PadState& state, SDL_Gamepad* pad) {
    constexpr struct { SDL_GamepadButton input; Button output; } mappings[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, CROSS}, {SDL_GAMEPAD_BUTTON_EAST, CIRCLE},
        {SDL_GAMEPAD_BUTTON_WEST, SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH, TRIANGLE},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, R1},
        {SDL_GAMEPAD_BUTTON_BACK, SELECT}, {SDL_GAMEPAD_BUTTON_START, START},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, R3},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, UP}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, RIGHT},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, DOWN}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, LEFT}
    };
    for (const auto& mapping : mappings)
        if (SDL_GetGamepadButton(pad, mapping.input)) press(state, mapping.output);
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 9830) press(state, L2);
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 9830) press(state, R2);
    state.lx = stick_axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX));
    state.ly = stick_axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));
    state.rx = stick_axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX));
    state.ry = stick_axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY));
}

void apply_keyboard(PadState& state) {
    if (SDL_GetKeyboardFocus() != window) return;
    const bool* keys = SDL_GetKeyboardState(nullptr);
    // launcher/CONTROLS.md has the full table.
    constexpr struct { SDL_Scancode key; Button button; } mappings[] = {
        {SDL_SCANCODE_UP, UP}, {SDL_SCANCODE_RIGHT, RIGHT},
        {SDL_SCANCODE_DOWN, DOWN}, {SDL_SCANCODE_LEFT, LEFT},
        {SDL_SCANCODE_SPACE, CROSS}, {SDL_SCANCODE_F, TRIANGLE},
        {SDL_SCANCODE_R, SQUARE}, {SDL_SCANCODE_C, CIRCLE},
        {SDL_SCANCODE_Q, L1}, {SDL_SCANCODE_LSHIFT, L1},
        {SDL_SCANCODE_E, L2}, {SDL_SCANCODE_LCTRL, L2},
        {SDL_SCANCODE_V, L3}, {SDL_SCANCODE_Z, R3},
        {SDL_SCANCODE_RETURN, START}, {SDL_SCANCODE_ESCAPE, START},
        {SDL_SCANCODE_TAB, SELECT}, {SDL_SCANCODE_BACKSPACE, SELECT},
        // Pad-style face/shoulder keys, handy in menus.
        {SDL_SCANCODE_K, CROSS}, {SDL_SCANCODE_L, CIRCLE},
        {SDL_SCANCODE_J, SQUARE}, {SDL_SCANCODE_I, TRIANGLE},
        {SDL_SCANCODE_1, L2}, {SDL_SCANCODE_2, R1}, {SDL_SCANCODE_3, R2}
    };
    for (const auto& mapping : mappings)
        if (keys[mapping.key]) press(state, mapping.button);
    if (keys[SDL_SCANCODE_A] != keys[SDL_SCANCODE_D])
        state.lx = keys[SDL_SCANCODE_A] ? 0 : 255;
    if (keys[SDL_SCANCODE_W] != keys[SDL_SCANCODE_S])
        state.ly = keys[SDL_SCANCODE_W] ? 0 : 255;
}

void set_mouse_capture(bool on) {
    if (!window || on == mouse_captured) return;
    if (!SDL_SetWindowRelativeMouseMode(window, on)) {
        std::fprintf(stderr, "SDL_SetWindowRelativeMouseMode: %s\n", SDL_GetError());
        return;
    }
    mouse_captured = on;
    mouse_dx = mouse_dy = 0;
    mouse_vx_prev = mouse_vy_prev = 0;
    mouse_rx = mouse_ry = 0;
    if (!on) mouse_buttons = 0;
}

// Turns the mouse motion of the last vblank into a right-stick deflection.
// The game turns the camera at a rate proportional to the stick, so the stick
// follows the mouse speed (counts per second), averaged over two vblanks to
// smooth out uneven event timing.  Small motions start just past the game's
// stick dead zone, so slow aiming still moves the camera.
int stick_from_speed(float counts_per_sec) {
    constexpr float full_speed = 1600.0f;   // counts/s for full deflection at sensitivity 1
    constexpr float dead_zone = 28.0f;      // measured: the game ignores deflections up to ~26
    const float magnitude = std::fabs(counts_per_sec) * mouse_sensitivity * 127.0f / full_speed;
    if (magnitude < 0.5f) return 0;
    const float out = std::min(127.0f, dead_zone + magnitude * (127.0f - dead_zone) / 127.0f);
    return static_cast<int>(counts_per_sec < 0 ? -out : out);
}

void update_mouse_look() {
    const u64 now = now_ns();
    float dt = mouse_last_ns ? static_cast<float>(now - mouse_last_ns) * 1e-9f : 0.02f;
    mouse_last_ns = now;
    dt = std::clamp(dt, 0.004f, 0.1f);
    float dx = mouse_dx, dy = mouse_dy;
    mouse_dx = mouse_dy = 0;
    bool looking;
    {
        std::lock_guard lock(look_mutex);
        looking = now < look_until_ns;
        if (looking) { dx += look_vx * dt; dy += look_vy * dt; }
    }
    if (!mouse_captured && !looking) { mouse_rx = mouse_ry = 0; return; }
    if (looking) {   // the fake "look" motion also feeds the direct camera
        float lx, ly;
        {
            std::lock_guard lock(look_mutex);
            lx = look_vx * dt;
            ly = look_vy * dt;
        }
        cam_dx += lx;
        cam_dy += ly;
        if (lx != 0 || ly != 0) mouse_move_ns = now;
    }
    const float vx = dx / dt, vy = dy / dt;
    if (!mouse_stick_only && now - cam_take_ns < kCamHookAlive) {
        // The game camera takes the motion directly; no stick deflection.
        mouse_rx = mouse_ry = 0;
        mouse_vx_prev = vx;
        mouse_vy_prev = vy;
        return;
    }
    mouse_rx = stick_from_speed(0.5f * (vx + mouse_vx_prev));
    mouse_ry = stick_from_speed(0.5f * (vy + mouse_vy_prev)) * (mouse_invert_y ? -1 : 1);
    mouse_vx_prev = vx;
    mouse_vy_prev = vy;
}

} // namespace

void mouse_camera_take(float& dx, float& dy, bool& recent) {
    const u64 now = now_ns();
    dx = dy = 0;
    recent = false;
    if (mouse_stick_only) { cam_dx = cam_dy = 0; return; }
    // Motion from before a pause (menu, cutscene, another camera) is dropped
    // instead of jumping the camera when the hook runs again.
    if (now - cam_take_ns >= kCamHookAlive) cam_dx = cam_dy = 0;
    cam_take_ns = now;
    dx = cam_dx * mouse_sensitivity;
    dy = cam_dy * mouse_sensitivity * (mouse_invert_y ? -1.0f : 1.0f);
    cam_dx = cam_dy = 0;
    recent = mouse_move_ns && now - mouse_move_ns < 1'000'000'000ull;
}

namespace {

void apply_mouse(PadState& state) {
    if (mouse_rx) state.rx = static_cast<u8>(128 + mouse_rx);
    if (mouse_ry) state.ry = static_cast<u8>(128 + mouse_ry);
    if (!mouse_captured) return;
    if ((mouse_buttons & SDL_BUTTON_LMASK) && !swallow_left) press(state, R1);   // primary weapon
    if (mouse_buttons & SDL_BUTTON_RMASK) press(state, R2);                      // secondary weapon
    if (mouse_buttons & SDL_BUTTON_MMASK) press(state, R3);
    if (mouse_buttons & SDL_BUTTON_X1MASK) press(state, L1);
    if (mouse_buttons & SDL_BUTTON_X2MASK) press(state, L2);
}

// PS2_INPUT_SCRIPT="start@70,cross@75+0.5,lup@80+2,..." presses port-0
// buttons (or pushes a stick to one side: lup/ldown/lleft/lright and the r*
// equivalents) at the given seconds after start (hold 0.15 s unless
// +<seconds> is given), for unattended smoke runs.
struct ScriptedPress { unsigned input; u64 from_ns, to_ns; };   // < 16: Button
std::vector<ScriptedPress> script;
std::mutex script_mutex;   // the control pipe thread appends presses
constexpr const char* input_names[] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                       "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square",
                                       "lup", "ldown", "lleft", "lright", "rup", "rdown", "rleft", "rright"};
std::string control_shot;  // pending "shot <path>" request, guarded by script_mutex
std::string control_dump;  // pending "dump <path>" request, guarded by script_mutex
std::vector<std::string> control_debug;  // pending debug commands, guarded by script_mutex
u64 script_origin_ns = 0;
void load_input_script() {
    const char* text = std::getenv("PS2_INPUT_SCRIPT");
    if (!text) return;
    const auto& names = input_names;
    std::string all(text);
    size_t begin = 0;
    while (begin < all.size()) {
        const size_t end = std::min(all.find(',', begin), all.size());
        const std::string item = all.substr(begin, end - begin);
        begin = end + 1;
        const size_t at = item.find('@');
        if (at == std::string::npos) continue;
        const std::string name = item.substr(0, at);
        double start = 0, hold = 0.15;
        if (std::sscanf(item.c_str() + at + 1, "%lf+%lf", &start, &hold) < 1) continue;
        for (unsigned input = 0; input < std::size(names); ++input)
            if (name == names[input])
                script.push_back({input, static_cast<u64>(start * 1e9), static_cast<u64>((start + hold) * 1e9)});
    }
    if (!script.empty()) std::fprintf(stderr, "[input] %zu scripted presses\n", script.size());
}
// PS2_CONTROL=<fifo>: live control for unattended testing.  Each line is
// "press <input> [hold seconds]" (names as in PS2_INPUT_SCRIPT) or
// "shot <png path>" (saved at the next vblank), "dump <path>" (EE RAM +
// scratchpad at the next vblank) or "quit".
void control_thread(std::string path) {
    const int fd = open(path.c_str(), O_RDWR);   // RDWR: no EOF when writers close
    if (fd < 0) { std::fprintf(stderr, "[control] cannot open %s\n", path.c_str()); return; }
    FILE* in = fdopen(fd, "r");
    char line[600];
    while (std::fgets(line, sizeof(line), in)) {
        char verb[16] = {}, arg[512] = {};
        double hold = 0.15;
        if (std::sscanf(line, "%15s %511s %lf", verb, arg, &hold) < 1) continue;
        if (!std::strcmp(verb, "quit")) {   // same path as closing the window
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
            continue;
        }
        std::lock_guard lock(script_mutex);
        if (!std::strcmp(verb, "shot")) { control_shot = arg; continue; }
        if (!std::strcmp(verb, "dump")) { control_dump = arg; continue; }
        if (!std::strcmp(verb, "watch")) { control_debug.push_back("watch " + std::string(arg)); continue; }
        if (!std::strcmp(verb, "watchreport")) { control_debug.push_back("watchreport"); continue; }
        if (!std::strcmp(verb, "look")) {   // look <x counts/s> <y counts/s> <secs>
            float vx = 0, vy = 0, secs = 1;
            if (std::sscanf(line, "%*s %f %f %f", &vx, &vy, &secs) >= 2) {
                std::lock_guard look_lock(look_mutex);
                look_vx = vx;
                look_vy = vy;
                look_until_ns = now_ns() + static_cast<u64>(secs * 1e9);
            }
            continue;
        }
        if (std::strcmp(verb, "press")) continue;
        const u64 t = now_ns() - script_origin_ns;
        for (unsigned input = 0; input < std::size(input_names); ++input)
            if (!std::strcmp(arg, input_names[input]))
                script.push_back({input, t, t + static_cast<u64>(hold * 1e9)});
    }
}
void apply_script(PadState& state) {
    std::lock_guard lock(script_mutex);
    if (script.empty()) return;
    const u64 t = now_ns() - script_origin_ns;
    for (const auto& press_at : script) {
        if (t < press_at.from_ns || t >= press_at.to_ns) continue;
        if (press_at.input < 16) { press(state, static_cast<Button>(press_at.input)); continue; }
        const unsigned stick = press_at.input - 16;   // l/r x up, down, left, right
        u8& axis = (stick & 2) ? (stick < 4 ? state.lx : state.rx) : (stick < 4 ? state.ly : state.ry);
        axis = (stick & 1) ? 255 : 0;
    }
}
} // namespace

bool take_control_shot(std::string& path) {
    std::lock_guard lock(script_mutex);
    if (control_shot.empty()) return false;
    path.swap(control_shot);
    control_shot.clear();
    return true;
}

std::vector<std::string> take_control_debug() {
    std::lock_guard lock(script_mutex);
    std::vector<std::string> out;
    out.swap(control_debug);
    return out;
}

bool take_control_dump(std::string& path) {
    std::lock_guard lock(script_mutex);
    if (control_dump.empty()) return false;
    path.swap(control_dump);
    control_dump.clear();
    return true;
}

bool init(const Config& cfg) {
    if (window) return true;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    if (!SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4) ||
        !SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5) ||
        !SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE) ||
        !SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1)) {
        std::fprintf(stderr, "SDL_GL_SetAttribute: %s\n", SDL_GetError());
        shutdown();
        return false;
    }
    const int scale = cfg.window_scale > 0 ? cfg.window_scale : 1;
    int width = (cfg.widescreen ? 796 : 640) * scale, height = 448 * scale;
    if (cfg.window_width > 0 && cfg.window_height > 0) {
        width = cfg.window_width;
        height = cfg.window_height;
    }
    mouse_sensitivity = cfg.mouse_sensitivity > 0 ? cfg.mouse_sensitivity : 1.0f;
    mouse_invert_y = cfg.mouse_invert_y;
    mouse_stick_only = cfg.mouse_stick;
    window = SDL_CreateWindow("Transformers", width, height,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        shutdown();
        return false;
    }
    fullscreen = cfg.fullscreen;
    if (fullscreen && !SDL_SetWindowFullscreen(window, true)) {
        std::fprintf(stderr, "SDL_SetWindowFullscreen: %s\n", SDL_GetError());
        shutdown();
        return false;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 48000};
    audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!audio || !SDL_ResumeAudioStreamDevice(audio)) {
        std::fprintf(stderr, "SDL audio: %s\n", SDL_GetError());
        shutdown();
        return false;
    }
    fill_free_slots();
    script_origin_ns = now_ns();
    load_input_script();
    if (const char* control = std::getenv("PS2_CONTROL"); control && *control)
        std::thread(control_thread, std::string(control)).detach();
    running = true;
    return true;
}

void shutdown() {
    running = false;
    set_mouse_capture(false);
    for (int slot = 0; slot < 2; ++slot) {
        if (pads[slot]) SDL_CloseGamepad(pads[slot]);
        pads[slot] = nullptr;
        pad_ids[slot] = 0;
    }
    if (audio) SDL_DestroyAudioStream(audio);
    audio = nullptr;
    if (window) SDL_DestroyWindow(window);
    window = nullptr;
    SDL_Quit();
}

void* sdl_window() { return window; }

bool poll_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (window && event.window.windowID == SDL_GetWindowID(window)) running = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (!event.key.repeat && event.key.scancode == SDL_SCANCODE_F10) {
                    // Debug: 2 s raw GS dump for gs_replay.
                    const char* home = std::getenv("HOME");
                    char path[512];
                    std::snprintf(path, sizeof path, "%s/.cache/transformers-port/gsdumps", home ? home : ".");
                    std::filesystem::create_directories(path);
                    std::snprintf(path + std::strlen(path), sizeof path - std::strlen(path), "/dump_%llu.gs",
                                  static_cast<unsigned long long>(now_ns() / 1000000000ull));
                    gs::request_dump(path, 120);
                    std::fprintf(stderr, "[dump] F10: recording 120 vsyncs to %s\n", path);
                }
                if (!event.key.repeat && event.key.scancode == SDL_SCANCODE_F11 && window) {
                    if (SDL_SetWindowFullscreen(window, !fullscreen)) fullscreen = !fullscreen;
                    else std::fprintf(stderr, "SDL_SetWindowFullscreen: %s\n", SDL_GetError());
                }
                if (!event.key.repeat && event.key.scancode == SDL_SCANCODE_ESCAPE) set_mouse_capture(false);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (!window || event.button.windowID != SDL_GetWindowID(window)) break;
                if (!mouse_captured) {
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        set_mouse_capture(true);
                        swallow_left = mouse_captured;
                    }
                    break;
                }
                mouse_buttons |= SDL_BUTTON_MASK(event.button.button);
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.button == SDL_BUTTON_LEFT) swallow_left = false;
                mouse_buttons &= ~SDL_BUTTON_MASK(event.button.button);
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (mouse_captured) {
                    mouse_dx += event.motion.xrel;
                    mouse_dy += event.motion.yrel;
                    cam_dx += event.motion.xrel;
                    cam_dy += event.motion.yrel;
                    if (event.motion.xrel != 0 || event.motion.yrel != 0) mouse_move_ns = now_ns();
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                set_mouse_capture(false);
                break;
            case SDL_EVENT_GAMEPAD_ADDED: open_pad(event.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: remove_pad(event.gdevice.which); break;
            default: break;
        }
    }
    update_mouse_look();
    return running;
}

PadState pad_state(int port) {
    PadState state{false, 0xffff, 0x80, 0x80, 0x80, 0x80, {}};
    if (port < 0 || port > 1) return state;
    state.connected = port == 0 || pads[port] != nullptr;
    if (pads[port]) apply_gamepad(state, pads[port]);
    if (port == 0) { apply_keyboard(state); apply_mouse(state); apply_script(state); }
    for (unsigned i = 0; i < 12; ++i)
        state.pressure[i] = (state.buttons & (1u << pressure_buttons[i])) ? 0 : 0xff;
    return state;
}

void pad_rumble(int port, u8 small_motor, u8 large_motor) {
    if (port < 0 || port > 1 || !pads[port]) return;
    SDL_RumbleGamepad(pads[port], static_cast<u16>(large_motor * 257u),
                      small_motor ? 0xffff : 0, 100);
}

void audio_push(const s16* frames, u32 nframes) {
    if (!frames || !nframes) return;
    // PS2_AUDIO_DUMP=<file>: raw s16 stereo 48 kHz copy of everything produced.
    static FILE* dump = [] {
        const char* path = std::getenv("PS2_AUDIO_DUMP");
        return path && *path ? std::fopen(path, "wb") : nullptr;
    }();
    if (dump) std::fwrite(frames, 4, nframes, dump);
    if (!audio) return;
    // Never let latency grow without bound (e.g. a stalled device): drop the
    // new batch when more than half a second is already queued.
    if (SDL_GetAudioStreamQueued(audio) > 48000 * 4 / 2) return;
    // SDL's byte count is an int; split unusually large batches without truncating.
    constexpr u32 max_frames = static_cast<u32>(std::numeric_limits<int>::max() / 4);
    while (nframes) {
        const u32 count = nframes < max_frames ? nframes : max_frames;
        if (!SDL_PutAudioStreamData(audio, frames, static_cast<int>(count * 4u))) {
            std::fprintf(stderr, "SDL_PutAudioStreamData: %s\n", SDL_GetError());
            return;
        }
        frames += count * 2u;
        nframes -= count;
    }
}

u32 audio_queued_frames() {
    if (!audio) return 0;
    const int bytes = SDL_GetAudioStreamQueued(audio);
    return bytes > 0 ? static_cast<u32>(bytes / 4) : 0;
}

u64 now_ns() { return SDL_GetTicksNS(); }
} // namespace platform
