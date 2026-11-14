// Replays a raw GSdx dump (PS2_GS_DUMP=<file>:<first vsync>:<vsyncs>) through
// the same GSdx build the game uses and saves a PNG after every vsync, so two
// renderer configurations can be compared on identical GS input:
//   gs_replay <dump> <out prefix> [hw|sw] [upscale]
// Environment variables reach GSdx as usual (e.g. PS2_GS_SCISSOR_BATCH=0).
#include "ps2/gs.h"
#include "ps2/log.h"
#include "ps2/platform.h"
#include <SDL3/SDL.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
void GSsetBaseMem(unsigned char*);
void GSgifTransfer1(unsigned char*, unsigned int);
void GSgifTransfer2(unsigned char*, unsigned int);
void GSgifTransfer3(unsigned char*, unsigned int);
void GSgifTransfer(const unsigned char*, unsigned int);
void GSreadFIFO2(unsigned char*, unsigned int);
void GSvsync(int);
void GSsetGameCRC(unsigned int, int);
struct GSFreezeData { int size; unsigned char* data; };
int GSfreeze(int mode, GSFreezeData* data);
int GSsaveCurrent(const char* path);
}

// The few runtime services gs.cpp uses, without linking the game.
u32 g_log_mask = 0;
void log_printf(u32, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
}
void ee_dump_trace(int) {}
namespace platform { u64 now_ns() { return SDL_GetTicksNS(); } }

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <dump> <out prefix> [hw|sw] [upscale]\n", argv[0]);
        return 2;
    }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::perror(argv[1]); return 1; }
    std::vector<unsigned char> file;
    {
        unsigned char buf[1 << 16];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) file.insert(file.end(), buf, buf + n);
        std::fclose(f);
    }
    size_t pos = 0;
    auto take = [&](void* dst, size_t n) {
        if (pos + n > file.size()) return false;
        std::memcpy(dst, file.data() + pos, n);
        pos += n;
        return true;
    };

    setenv("PS2_GS_SYNC", "1", 1);   // GSdx runs on this thread: calls below go straight in
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "SDL init: %s\n", SDL_GetError()); return 1; }
    SDL_Window* window = SDL_CreateWindow("gs_replay", 960, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window) { std::fprintf(stderr, "SDL window: %s\n", SDL_GetError()); return 1; }
    gs::Config cfg;
    cfg.renderer = argc > 3 && std::strcmp(argv[3], "sw") == 0 ? 0 : 1;
    cfg.upscale = argc > 4 ? std::atoi(argv[4]) : 1;
    cfg.vsync = false;
    if (!gs::init(window, cfg)) { std::fprintf(stderr, "GSdx initialization failed\n"); return 1; }

    std::vector<unsigned char> regs(0x2000);
    GSsetBaseMem(regs.data());
    unsigned int crc = 0;
    GSFreezeData fd{};
    if (!take(&crc, 4) || !take(&fd.size, 4)) { std::fprintf(stderr, "short dump\n"); return 1; }
    std::vector<unsigned char> state(fd.size);
    fd.data = state.data();
    if (!take(fd.data, fd.size) || !take(regs.data(), 0x2000)) { std::fprintf(stderr, "short dump\n"); return 1; }
    GSsetGameCRC(crc, 0);
    if (GSfreeze(0 /* FREEZE_LOAD */, &fd) != 0) { std::fprintf(stderr, "state load failed\n"); return 1; }

    int frame = 0;
    std::vector<unsigned char> path1(0x4000), data, fifo;
    unsigned char type;
    while (take(&type, 1)) {
        if (type == 0) {
            unsigned char index;
            unsigned int size;
            if (!take(&index, 1) || !take(&size, 4)) break;
            if (index == 0) {
                if (size > 0x4000) break;
                if (!take(path1.data() + 0x4000 - size, size)) break;
                GSgifTransfer1(path1.data(), 0x4000 - size);
            } else {
                data.resize(size);
                if (!take(data.data(), size)) break;
                if (index == 1) GSgifTransfer2(data.data(), size / 16);
                else if (index == 2) GSgifTransfer3(data.data(), size / 16);
                else GSgifTransfer(data.data(), size / 16);
            }
        } else if (type == 1) {
            unsigned char field;
            if (!take(&field, 1)) break;
            gs::vsync(field);
            // The merged output texture, saved without an extra present (a
            // window capture would add a vsync and change later frames).
            const std::string out = std::string(argv[2]) + std::to_string(frame++) + ".png";
            if (!GSsaveCurrent(out.c_str())) std::fprintf(stderr, "no output for %s\n", out.c_str());
        } else if (type == 2) {
            unsigned int size;
            if (!take(&size, 4)) break;
            fifo.resize(size);
            GSreadFIFO2(fifo.data(), size / 16);
        } else if (type == 3) {
            if (!take(regs.data(), 0x2000)) break;
        } else {
            std::fprintf(stderr, "unknown packet type %d at %zu\n", type, pos - 1);
            break;
        }
    }
    std::printf("replayed %d vsyncs\n", frame);
    gs::shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
