#include "ps2/gs.h"
#include <SDL3/SDL.h>
#include <png.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <utility>
#include <xmmintrin.h>

namespace {
int signals = 0;
int finishes = 0;
int label_events = 0;
void event(u32 bits) {
    if (bits & 1) ++signals;
    if (bits & 2) ++finishes;
    if (!bits) ++label_events;
}

struct Packet {
    std::vector<u8> bytes;
    void qword(u64 lo, u64 hi) {
        for (int shift = 0; shift < 64; shift += 8) bytes.push_back(lo >> shift);
        for (int shift = 0; shift < 64; shift += 8) bytes.push_back(hi >> shift);
    }
    void regs(std::initializer_list<std::pair<u8, u64>> items) {
        qword(items.size() | (1ull << 15) | (1ull << 60), 0xe);
        for (auto [reg, val] : items) qword(val, reg);
    }
    void image(const std::vector<u8>& pixels) {
        if (pixels.size() % 16) std::abort();
        qword((pixels.size() / 16) | (1ull << 15) | (2ull << 58), 0);
        bytes.insert(bytes.end(), pixels.begin(), pixels.end());
    }
    void send() {
        if (bytes.size() % 16) std::abort();
        gs::gif_path3(bytes.data(), bytes.size() / 16);
        bytes.clear();
    }
};

bool frame_mode_coordinates = false;
u64 xy(int x, int y) {
    return (u64(x) << 4) | (u64(frame_mode_coordinates ? y / 2 : y) << 20);
}
constexpr u64 rgba(u8 r, u8 g, u8 b, u8 a = 128) {
    return u64(r) | (u64(g) << 8) | (u64(b) << 16) | (u64(a) << 24) | (u64(0x3f800000) << 32);
}
constexpr u64 tex0(int bp, int psm, int clut = 0) {
    return u64(bp) | (u64(psm == 0x13 ? 2 : 1) << 14) | (u64(psm) << 20)
        | (6ull << 26) | (6ull << 30) | (1ull << 34) | (u64(clut) << 37)
        | (u64(clut != 0) << 61); // indexed textures load the CLUT on TEX0 writes
}
void upload(Packet& p, int bp, int psm, int width, int height, const std::vector<u8>& data) {
    p.regs({{0x50, u64(psm == 0x13 ? 2 : 1) << 48 | u64(bp) << 32 | u64(psm) << 56},
            {0x51, 0}, {0x52, u64(width) | u64(height) << 32}, {0x53, 0}});
    p.image(data);
    p.send();
}
void sprite(Packet& p, int x1, int y1, int x2, int y2, u64 color, bool textured, bool blend) {
    p.regs({{0x00, 6ull | u64(textured) << 4 | u64(blend) << 6 | u64(textured) << 8},
            {0x01, color}, {0x05, xy(x1,y1)}, {0x05, xy(x2,y2)}});
}
bool inspect_picture(const char* path) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, path)) return false;
    if (image.width != 1152 || image.height != 720) {
        png_image_free(&image);
        return false;
    }
    image.format = PNG_FORMAT_RGBA; // preserve alpha; RGB conversion premultiplies dim interlaced fields
    std::vector<u8> pixels(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr)) {
        png_image_free(&image);
        return false;
    }
    png_image_free(&image);
    auto pixel = [&](int x, int y) { return pixels.data() + (y * 1152 + x) * 4; };
    auto difference = [&](int x1, int y1, int x2, int y2) {
        const u8* a = pixel(x1, y1);
        const u8* b = pixel(x2, y2);
        return std::abs(int(a[0]) - int(b[0])) + std::abs(int(a[1]) - int(b[1]))
            + std::abs(int(a[2]) - int(b[2]));
    };
    const u8* red = pixel(250, 150);
    const u8* green = pixel(450, 150);
    const u8* indexed_top = pixel(700, 480);
    const u8* indexed_bottom = pixel(700, 680);
    return pixel(15, 360)[0] < 5 && pixel(1140, 360)[0] < 5 // 4:3 side bars
        && pixel(140, 360)[2] > 15                    // visible clear color
        && red[0] > red[1] && green[1] > green[0]   // Gouraud triangle
        && difference(680, 100, 730, 100) > 35     // PSMCT32 checker
        && indexed_top[0] > indexed_top[1] && indexed_bottom[1] > indexed_bottom[0]
        && difference(700, 480, 700, 680) > 80     // PSMT8 + CLUT
        && int(pixel(300, 650)[0]) - int(pixel(140, 650)[0]) > 17; // alpha blend
}

}

int main(int argc, char** argv) {
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "SDL init: %s\n", SDL_GetError()); return 1; }
    SDL_Window* window = SDL_CreateWindow("GSdx smoke", 960, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window) { std::fprintf(stderr, "SDL window: %s\n", SDL_GetError()); return 1; }
    gs::Config cfg;
    cfg.renderer = argc > 1 && argv[1][0] == 'h' ? 1 : 0;
    cfg.vsync = false;
    cfg.sw_threads = 2;
    _mm_setcsr(0xffc0); // EE guest uses RZ/FTZ/DAZ; adapter must restore it.
    if (!gs::init(window, cfg)) { std::fprintf(stderr, "GSdx initialization failed\n"); return 1; }
    gs::set_event_callback(event);
    SDL_SetWindowSize(window, 1152, 720);
    if (!SDL_SyncWindow(window)) { std::fprintf(stderr, "SDL resize: %s\n", SDL_GetError()); return 1; }
    const int height = argc > 3 && std::strcmp(argv[3], "ntsc") == 0 ? 448 : 512;
    const bool frame_mode = argc > 3 && std::strcmp(argv[3], "pal-frame") == 0;
    frame_mode_coordinates = frame_mode;
    gs::write_priv64(0x0000, 1); // PMODE.EN1
    gs::write_priv64(0x0010, u64(height == 512 ? 3 : 2) << 13); // SMODE1.CMOD
    gs::write_priv64(0x0020, 1 | u64(frame_mode) << 1); // SMODE2.INT, FFMD
    gs::write_priv64(0x0070, 10ull << 9); // DISPFB1 FBW=10, PSMCT32
    gs::write_priv64(0x0080, 639ull << 32 | u64(height - 1) << 44);
    gs::write_priv64(0x1010, 0x1f00); // masked GS IMR; events still report CSR changes
    gs::vsync(0); // settle the newly resized Wayland/EGL swap chain before drawing

    Packet p;
    p.regs({{0x1a, 1}, {0x4c, 10ull << 16},
            {0x40, u64(639) << 16 | u64(frame_mode ? 255 : 511) << 48},
            {0x47, 0}, // TEST_1: depth and alpha tests disabled
            {0x42, 1ull << 2 | 1ull << 6}, // alpha: (Cs-Cd)*As/128+Cd
            {0x18, 0}});
    sprite(p, 0, 0, 640, 512, rgba(25, 34, 58), false, false);
    // Gouraud triangle: red, green, blue corners, against dark background.
    p.regs({{0x00, 3 | 1ull << 3}, {0x01, rgba(245, 55, 65)}, {0x05, xy(58, 65)},
            {0x01, rgba(48, 240, 94)}, {0x05, xy(345, 85)},
            {0x01, rgba(67, 76, 248)}, {0x05, xy(140, 330)}});
    p.send();

    std::vector<u8> rgba_tex(64 * 64 * 4);
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
        size_t i = (y * 64 + x) * 4;
        rgba_tex[i] = x * 4;
        rgba_tex[i+1] = y * 4;
        rgba_tex[i+2] = ((x / 8 + y / 8) & 1) ? 230 : 35;
        rgba_tex[i+3] = 128;
    }
    upload(p, 0x2000, 0, 64, 64, rgba_tex);
    p.regs({{0x06, tex0(0x2000, 0)}, {0x08, 0}, {0x14, 0},
            {0x00, 6ull | (1ull << 4) | (1ull << 8)}, {0x01, rgba(128,128,128)},
            {0x03, 0}, {0x05, xy(360, 55)}, {0x03, 64ull << 4 | (64ull << 20)}, {0x05, xy(590, 285)}});
    p.send();
    p.regs({{0x50, 0x2000ull | (1ull << 16)}, {0x51, 0}, {0x52, 4ull | 1ull << 32}, {0x53, 1}});
    p.send();
    std::array<u8, 16> downloaded{};
    gs::read_fifo(downloaded.data(), 1);
    const bool fifo_ok = std::memcmp(downloaded.data(), rgba_tex.data(), 16) == 0;

    std::vector<u8> indexed(64 * 64);
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x)
        indexed[y * 64 + x] = static_cast<u8>((x / 8 + 8 * (y / 8)) * 4);
    upload(p, 0x2100, 0x13, 64, 64, indexed);
    std::vector<u8> clut(16 * 16 * 4);
    for (int i = 0; i < 256; ++i) {
        clut[i * 4] = 255 - i; clut[i * 4+1] = i;
        clut[i * 4+2] = (i / 16) * 16; clut[i * 4+3] = 128;
    }
    upload(p, 0x2200, 0, 16, 16, clut);
    p.regs({{0x06, tex0(0x2100, 0x13, 0x2200)}, {0x00, 6ull | 1ull << 4 | 1ull << 8},
            {0x01, rgba(128,128,128)}, {0x03, 0}, {0x05, xy(380, 290)},
            {0x03, 64ull << 4 | 64ull << 20}, {0x05, xy(580, 490)}});
    sprite(p, 95, 365, 370, 468, rgba(245, 225, 30, 64), false, true);
    p.send();
    p.regs({{0x60, 0xffffffff00000012ull}});
    gs::gif_path3(p.bytes.data(), 1); // split GIFtag and A+D data across DMA calls
    gs::gif_path3(p.bytes.data() + 16, 1);
    // Force XGKICK to cross the VU1 ring boundary between a tag and its data.
    gs::write_priv64(0x1000, 1); // CSR SIGNAL write-one-to-clear
    Packet wrap;
    wrap.regs({{0x60, 0xffffffff00000033ull}});
    std::array<u8, 16384> vu1{};
    std::memcpy(vu1.data() + 16368, wrap.bytes.data(), 16);
    std::memcpy(vu1.data(), wrap.bytes.data() + 16, 16);
    gs::gif_path1(vu1.data(), 1023);
    const u64 path1_id = gs::read_priv64(0x1080); // synchronization point for asynchronous GS
    const bool path1_ok = signals == 2 && (path1_id & 0xffffffff) == 0x33;
    Packet direct;
    direct.regs({{0x61, 0}, {0x62, 0xffffffff000000abull}});
    gs::gif_path2(direct.bytes.data(), direct.bytes.size() / 16);
    const u64 path2_id = gs::read_priv64(0x1080);
    const bool path2_ok = finishes == 1 && label_events == 1 && (path2_id >> 32) == 0xab;
    gs::vsync(0);
    gs::vsync(1);
    const bool field_ok = (gs::read_priv64(0x1000) & (1u << 13)) != 0;
    const char* image_path = argc > 2 ? argv[2] : "gs-smoke.png";
    const bool saved = gs::screenshot(image_path);
    const bool picture_ok = saved && inspect_picture(image_path);
    const u64 csr = gs::read_priv64(0x1000);
    const bool csr_set = (csr & 3) == 3; // SIGNAL and FINISH from GIF
    gs::write_priv64(0x1000, 1); // clear SIGNAL, leave FINISH set
    const bool csr_cleared = (gs::read_priv64(0x1000) & 3) == 2;
    gs::write_priv64(0x1000, 1u << 9); // GS reset clears displayed privileged regs and CSR
    const bool reset_ok = gs::read_priv64(0x0000) == 0 && (gs::read_priv64(0x1000) & 0x1f) == 0;
    const bool csr_ok = csr_set && csr_cleared && reset_ok;
    gs::shutdown();
    const unsigned int mxcsr_after = _mm_getcsr();
    const bool mxcsr_ok = (mxcsr_after & ~0x3fu) == 0xffc0;
    _mm_setcsr(0x1f80);
    std::fprintf(stderr, "GS smoke: png=%d picture=%d FIFO=%d PATH1=%d PATH2=%d CSR=%d FIELD=%d MXCSR=%d (%x)\n",
                 saved, picture_ok, fifo_ok, path1_ok, path2_ok, csr_ok, field_ok, mxcsr_ok, mxcsr_after);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return picture_ok && fifo_ok && path1_ok && path2_ok && csr_ok && field_ok && mxcsr_ok ? 0 : 1;
}
