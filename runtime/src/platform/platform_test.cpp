#include "ps2/platform.h"

#include <SDL3/SDL.h>
#include <cmath>
#include <cstdio>

int main() {
    if (!platform::init({1, false, true})) return 1;
    std::printf("Window opened: %p; audio device opened\n", platform::sdl_window());
    const u64 start = platform::now_ns();
    u32 sent = 0;
    platform::PadState previous[2] = {};
    bool seen[2] = {};
    while (platform::poll_events() && platform::now_ns() - start < 3'000'000'000ULL) {
        // Feed a 440 Hz stereo tone in short slices over one second, without
        // prequeueing a full second of audio and introducing a second of latency.
        constexpr u32 chunk = 480;
        if (sent < 48000 && platform::audio_queued_frames() < 960) {
            s16 frames[chunk * 2];
            const u32 count = sent + chunk <= 48000 ? chunk : 48000 - sent;
            for (u32 i = 0; i < count; ++i) {
                const double angle = 6.283185307179586 * 440.0 * (sent + i) / 48000.0;
                const s16 sample = static_cast<s16>(std::sin(angle) * 4000.0);
                frames[2 * i] = sample;
                frames[2 * i + 1] = sample;
            }
            platform::audio_push(frames, count);
            sent += count;
        }
        for (int port = 0; port < 2; ++port) {
            const auto state = platform::pad_state(port);
            const auto& old = previous[port];
            if (!seen[port] || state.connected != old.connected || state.buttons != old.buttons ||
                state.lx != old.lx || state.ly != old.ly || state.rx != old.rx || state.ry != old.ry) {
                std::printf("Pad %d: connected=%d buttons=%04x left=(%u,%u) right=(%u,%u)\n",
                            port, state.connected, state.buttons, state.lx, state.ly, state.rx, state.ry);
                previous[port] = state;
                seen[port] = true;
            }
        }
        SDL_Delay(10);
    }
    std::printf("Played %u frames of 440 Hz tone; queued=%u frames\n",
                sent, platform::audio_queued_frames());
    platform::shutdown();
    return sent == 48000 ? 0 : 1;
}
