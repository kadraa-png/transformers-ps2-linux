// Transformers (PAL) native port - entry point.
//
// usage: transformers [options] <path/to/Transformers.iso>
//   --renderer hw|sw    GS renderer (default hw: OpenGL; sw = accurate software rasterizer, slower)
//   --sw-threads N      software rasterizer worker threads (0: draw on the GS thread)
//   --upscale N         internal resolution multiplier for the hw renderer
//   --scale N           initial window scale (default 2)
//   --resolution WxH    initial window size, e.g. 1920x1080 (also picks --upscale to match)
//   --widescreen        16:9: the game renders its widescreen mode, shown at 16:9
//   --mouse-sens X      mouse look sensitivity (default 1.0)
//   --invert-mouse      invert mouse look up/down
//   --mouse-stick       mouse drives the right stick (old behaviour) instead of
//                       turning the game camera directly
//   --sharpen N         output sharpening 0-100 (default 40, 0 = off)
//   --deflicker         keep the game's anti-flicker blend (softer, as on a TV)
//   --fullscreen
//   --lang en|fr|de|es|it
#include "ps2/ee_context.h"
#include "ps2/gs.h"
#include "ps2/iop.h"
#include "ps2/iso.h"
#include "ps2/log.h"
#include "ps2/platform.h"
#include "ps2/vu.h"

#include "hw/hw.h"
#include "kernel/kernel.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <zlib.h>

extern const u32 g_ee_elf_crc32;
void ee_dispatch_init();
void crash_handler_install();
void profiler_start();
void profiler_stop_and_report();

namespace {

std::string boot_path_from_cnf() {
    iso::Entry e;
    if (!iso::find("SYSTEM.CNF", &e)) FATAL("SYSTEM.CNF not found on the disc image");
    std::string cnf(e.size, '\0');
    iso::read_bytes((u64)e.lba * iso::SECTOR_SIZE, e.size, cnf.data());
    auto pos = cnf.find("BOOT2");
    if (pos == std::string::npos) FATAL("SYSTEM.CNF has no BOOT2 line");
    auto eq = cnf.find('=', pos);
    auto end = cnf.find_first_of("\r\n", eq);
    std::string v = cnf.substr(eq + 1, end - eq - 1);
    while (!v.empty() && v.front() == ' ') v.erase(v.begin());
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
    return v;  // e.g. cdrom0:\SLES_523.88;1
}

u32 load_elf(const std::string& boot) {
    iso::Entry e;
    if (!iso::find(boot, &e)) FATAL("boot executable %s not found on the disc", boot.c_str());
    std::vector<u8> elf(e.size);
    iso::read_bytes((u64)e.lba * iso::SECTOR_SIZE, e.size, elf.data());
    u32 crc = (u32)crc32(0, elf.data(), (uInt)elf.size());
    if (crc != g_ee_elf_crc32)
        FATAL("%s (crc %08x) is not the executable this port was built from (crc %08x). "
              "This build supports Transformers (USA), SLUS-20668, only.",
              boot.c_str(), crc, g_ee_elf_crc32);
    if (memcmp(elf.data(), "\x7f" "ELF", 4) != 0) FATAL("boot file is not an ELF");
    u32 entry, phoff;
    u16 phnum;
    memcpy(&entry, &elf[0x18], 4);
    memcpy(&phoff, &elf[0x1C], 4);
    memcpy(&phnum, &elf[0x2C], 2);
    for (u32 i = 0; i < phnum; ++i) {
        const u8* ph = &elf[phoff + i * 32];
        u32 type, off, vaddr, filesz, memsz;
        memcpy(&type, ph, 4);
        memcpy(&off, ph + 4, 4);
        memcpy(&vaddr, ph + 8, 4);
        memcpy(&filesz, ph + 16, 4);
        memcpy(&memsz, ph + 20, 4);
        if (type != 1 || !memsz) continue;
        memcpy(ee_host(vaddr), &elf[off], filesz);
        if (memsz > filesz) memset(ee_host(vaddr + filesz), 0, memsz - filesz);
        LOG(LOG_SYS, "[boot] segment %08x file %x mem %x\n", vaddr, filesz, memsz);
    }
    return entry;
}

int lang_code(const char* s) {
    static const char* names[] = {"jp", "en", "fr", "es", "de", "it"};
    for (int i = 0; i < 6; ++i)
        if (!strcmp(s, names[i])) return i;
    return 1;
}

void usage() {
    fprintf(stderr,
            "usage: transformers [--renderer sw|hw] [--sw-threads N] [--upscale N] [--scale N]\n"
            "                    [--resolution WxH] [--widescreen] [--mouse-sens X] [--invert-mouse] [--mouse-stick]\n"
            "                    [--sharpen N] [--deflicker] [--fullscreen] [--vsync]\n"
            "                    [--lang en|fr|de|es|it] <Transformers (USA).iso>\n");
}

} // namespace

int main(int argc, char** argv) {
    log_init_from_env();
    crash_handler_install();
    profiler_start();
    gs::Config gcfg;
    gcfg.renderer = 1;    // OpenGL: holds the game's 30 fps on modest CPUs; --renderer sw for the exact rasterizer
    gcfg.vsync = false;   // the guest's 50 Hz timer paces frames; --vsync opts into host vsync
    gcfg.sharpen = 40;           // mild contrast-adaptive sharpening of the output
    gcfg.skip_deflicker = true;  // the 1-line anti-flicker blend only blurs on a monitor
    platform::Config pcfg;
    kernel::Config kcfg;
    const char* iso_path = getenv("TRANSFORMERS_ISO");
    bool upscale_given = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { usage(); exit(2); }
            return argv[++i];
        };
        if (a == "--renderer") gcfg.renderer = std::string(next()) == "hw" ? 1 : 0;
        else if (a == "--upscale") { gcfg.upscale = atoi(next()); upscale_given = true; }
        else if (a == "--sw-threads") gcfg.sw_threads = atoi(next());
        else if (a == "--scale") pcfg.window_scale = atoi(next());
        else if (a == "--fullscreen") pcfg.fullscreen = true;
        else if (a == "--widescreen") pcfg.widescreen = gcfg.widescreen = kcfg.widescreen = true;
        else if (a == "--resolution") {
            const char* r = next();
            if (sscanf(r, "%dx%d", &pcfg.window_width, &pcfg.window_height) != 2 ||
                pcfg.window_width <= 0 || pcfg.window_height <= 0) { usage(); return 2; }
        }
        else if (a == "--mouse-sens") pcfg.mouse_sensitivity = static_cast<float>(atof(next()));
        else if (a == "--invert-mouse") pcfg.mouse_invert_y = true;
        else if (a == "--mouse-stick") pcfg.mouse_stick = true;
        else if (a == "--vsync") gcfg.vsync = true;
        else if (a == "--sharpen") gcfg.sharpen = atoi(next());
        else if (a == "--deflicker") gcfg.skip_deflicker = false;
        else if (a == "--lang") kcfg.language = lang_code(next());
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else iso_path = argv[i];
    }
    if (!iso_path) { usage(); return 2; }
    // --resolution without --upscale: render at least as many lines as the
    // window shows (the game draws 448 lines; 1080p -> x3).
    if (pcfg.window_height > 0 && !upscale_given)
        gcfg.upscale = std::max(1, (pcfg.window_height + 447) / 448);
    if (!iso::open(iso_path)) FATAL("cannot open disc image %s", iso_path);

    ee_mem_init();
    std::string boot = boot_path_from_cnf();
    u32 entry = load_elf(boot);
    ee_dispatch_init();

    if (!platform::init(pcfg)) FATAL("platform init failed");
    if (!gs::init(platform::sdl_window(), gcfg)) FATAL("GS backend init failed");
    vu_init();
    vif_init();
    hw::init();
    vu_thread_start();
    iop::init();

    kcfg.argv = {boot};
    kernel::init(kcfg);
    int rc = kernel::run(entry);
    profiler_stop_and_report();   // before shutdown steps that could hang

    iop::shutdown();
    vu_thread_stop();
    gs::shutdown();
    platform::shutdown();
    iso::close();
    return rc;
}
