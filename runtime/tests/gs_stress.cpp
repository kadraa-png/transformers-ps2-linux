#include "ps2/gs.h"
#include <SDL3/SDL.h>
#include <png.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr int primitives = 100000;
constexpr int frames = 120;
constexpr int batch = 100;
struct Packet {
    std::vector<u8> bytes;
    void quad(u64 lo, u64 hi) {
        for (int i = 0; i != 64; i += 8) bytes.push_back(lo >> i);
        for (int i = 0; i != 64; i += 8) bytes.push_back(hi >> i);
    }
    void send() {
        gs::gif_path3(bytes.data(), static_cast<u32>(bytes.size() / 16));
        bytes.clear();
    }
};
constexpr u64 color(int index) {
    return static_cast<u64>(index * 19 % 230 + 20)
        | (static_cast<u64>(index * 43 % 210 + 40) << 8)
        | (static_cast<u64>(index * 37 % 240 + 15) << 16)
        | (128ull << 24) | (0x3f800000ull << 32);
}
constexpr u64 xy(int x, int y) { return static_cast<u64>(x) << 4 | static_cast<u64>(y) << 20; }
bool read_png(const char* path, std::vector<u8>& pixels) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, path)) return false;
    image.format = PNG_FORMAT_RGBA;
    pixels.resize(PNG_IMAGE_SIZE(image));
    const bool ok = png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr);
    png_image_free(&image);
    return ok;
}
struct Result {
    std::vector<u8> pixels;
    double submit_ms = 0, total_ms = 0, vsync_sum_ms = 0, vsync_max_ms = 0;
    bool ok = false;
};
Result run(SDL_Window* window, bool sync, int renderer, const char* image_path) {
    if (sync) setenv("PS2_GS_SYNC", "1", 1);
    else unsetenv("PS2_GS_SYNC");
    Result result;
    gs::Config cfg;
    cfg.renderer = renderer;
    cfg.vsync = false;
    cfg.sw_threads = 2;
    if (!gs::init(window, cfg)) return result;
    gs::write_priv64(0x0000, 1); // PMODE.EN1
    gs::write_priv64(0x0010, 3ull << 13); // PAL SMODE1.CMOD
    gs::write_priv64(0x0020, 1); // interlaced field mode
    gs::write_priv64(0x0070, 10ull << 9); // DISPFB1, 640-wide 32-bit FB
    gs::write_priv64(0x0080, 639ull << 32 | 511ull << 44);
    gs::vsync(0); // establish swap-chain state before the stress scene
    Packet setup;
    setup.quad(3 | (1ull << 15) | (1ull << 60), 0xe);
    setup.quad(1, 0x1a); // PRMODECONT: PRIM controls drawing mode
    setup.quad(10ull << 16, 0x4c); // FRAME_1
    setup.quad(639ull << 16 | 511ull << 48, 0x40); // SCISSOR_1
    setup.send();
    const auto start = Clock::now();
    Packet packet;
    packet.bytes.reserve(16 + batch * 4 * 16);
    int index = 0;
    for (int frame = 0; frame < frames; ++frame) {
        const int frame_end = primitives * (frame + 1) / frames;
        while (index < frame_end) {
            const int count = std::min(batch, frame_end - index);
            packet.quad(static_cast<u64>(count * 4) | (1ull << 15) | (1ull << 60), 0xe);
            for (int j = 0; j < count; ++j, ++index) {
                const int x = (index * 17) % 636;
                const int y = (index * 31) % 508;
                packet.quad(6, 0x00); // untextured sprite
                packet.quad(color(index), 0x01);
                packet.quad(xy(x, y), 0x05);
                packet.quad(xy(x + 4, y + 4), 0x05);
            }
            packet.send();
        }
        const auto vsync_start = Clock::now();
        gs::vsync(frame & 1);
        const double latency = std::chrono::duration<double, std::milli>(Clock::now() - vsync_start).count();
        result.vsync_sum_ms += latency;
        result.vsync_max_ms = std::max(result.vsync_max_ms, latency);
    }
    const auto submitted = Clock::now();
    result.ok = gs::screenshot(image_path) && read_png(image_path, result.pixels);
    gs::shutdown();
    const auto finished = Clock::now();
    result.submit_ms = std::chrono::duration<double, std::milli>(submitted - start).count();
    result.total_ms = std::chrono::duration<double, std::milli>(finished - start).count();
    return result;
}
}
int main(int argc, char** argv) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("GS stress", 960, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    char temp_dir[] = "/tmp/gs-stress-XXXXXX";
    if (!mkdtemp(temp_dir)) {
        std::perror("mkdtemp");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    const std::string async_path = std::string(temp_dir) + "/async.png";
    const std::string sync_path = std::string(temp_dir) + "/sync.png";
    const int renderer = argc > 1 && std::strcmp(argv[1], "hardware") == 0 ? 1 : 0;
    // One process / one SDL window ensures both executions see the same size
    // and guest register sequence; the GSdx context is created on each owner.
    const Result async = run(window, false, renderer, async_path.c_str());
    const Result sync = run(window, true, renderer, sync_path.c_str());
    const bool equal = async.ok && sync.ok && async.pixels == sync.pixels;
    size_t colored = 0;
    for (size_t i = 0; i + 3 < async.pixels.size(); i += 4)
        colored += async.pixels[i] > 40 || async.pixels[i + 1] > 40 || async.pixels[i + 2] > 40;
    const bool visible = async.pixels.size() == (960 * 720 * 4) && colored > 100000;
    std::fprintf(stderr,
        "GS stress %s: %d primitives/%d frames, async submit %.1f ms (%.0f primitives/s), "
        "async total %.1f ms (%.0f primitives/s), sync total %.1f ms (%.0f primitives/s), "
        "vsync mean/max async %.3f/%.3f ms sync %.3f/%.3f ms, pixel match=%d visible=%d\n",
        renderer ? "hardware" : "software", primitives, frames,
        async.submit_ms, primitives * 1000.0 / async.submit_ms,
        async.total_ms, primitives * 1000.0 / async.total_ms,
        sync.total_ms, primitives * 1000.0 / sync.total_ms,
        async.vsync_sum_ms / frames, async.vsync_max_ms,
        sync.vsync_sum_ms / frames, sync.vsync_max_ms, equal, visible);
    std::remove(async_path.c_str());
    std::remove(sync_path.c_str());
    rmdir(temp_dir);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return equal && visible ? 0 : 1;
}
