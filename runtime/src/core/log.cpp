#include "ps2/log.h"

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

u32 g_log_mask = LOG_TTY;

namespace {
std::mutex g_log_lock;

struct CatName {
    const char* name;
    u32 bit;
};
const CatName kCats[] = {
    {"sys", LOG_SYS},   {"thread", LOG_THREAD}, {"intc", LOG_INTC}, {"dma", LOG_DMA},
    {"hw", LOG_HW},     {"gs", LOG_GS},         {"vif", LOG_VIF},   {"vu", LOG_VU},
    {"sif", LOG_SIF},   {"iop", LOG_IOP},       {"cdvd", LOG_CDVD}, {"pad", LOG_PAD},
    {"mc", LOG_MC},     {"spu", LOG_SPU},       {"ipu", LOG_IPU},   {"tty", LOG_TTY},
    {"call", LOG_CALL},
};
} // namespace

void log_init_from_env() {
    const char* e = getenv("PS2_LOG");
    if (!e) return;
    std::string s(e);
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t comma = s.find(',', pos);
        std::string tok = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? s.size() + 1 : comma + 1;
        bool neg = !tok.empty() && tok[0] == '-';
        if (neg) tok = tok.substr(1);
        u32 bits = 0;
        if (tok == "all") bits = ~0u & ~LOG_CALL;
        for (const auto& c : kCats)
            if (tok == c.name) bits = c.bit;
        if (neg) g_log_mask &= ~bits;
        else g_log_mask |= bits;
    }
}

void log_printf(u32 cat, const char* fmt, ...) {
    (void)cat;
    std::lock_guard<std::mutex> lk(g_log_lock);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void fatal_error(const char* fmt, ...) {
    {
        std::lock_guard<std::mutex> lk(g_log_lock);
        fputs("FATAL: ", stderr);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
        fflush(stderr);
    }
    abort();
}
