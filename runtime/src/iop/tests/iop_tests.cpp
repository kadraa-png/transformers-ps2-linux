#include "ps2/ee_mem.h"
#include "ps2/spu2.h"
#include "ps2/iop.h"
#include "iop/kernel.h"
#include "iop/recompiled.h"
#include "iop/cdvd_rpc.h"
#include "ps2/iso.h"
#include "ps2/platform.h"
#include "ps2/intc.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <string>

namespace {
constexpr u32 EE_SEND = 0x100000;
constexpr u32 EE_RECV = 0x101000;
constexpr u32 EE_DATA = 0x102000;
constexpr u32 IOP_SEND = 0x9000;
constexpr u32 IOP_RPC_BUF = 0xa000;
u32 sif_interrupts = 0;
void put(u32 address, u32 value) { ee_write32_ram(address, value); }
u32 get(u32 address) { return ee_read32_ram(address); }
void check(bool passed, const char* description) {
    if (!passed) throw std::runtime_error(description);
    std::printf("PASS %s\n", description);
}
void test_heap() {
    iop::Context ctx{};
    auto sysmem = [&](u32 op, u32 mode = 0, u32 size = 0, u32 address = 0) {
        ctx.r[4] = mode; ctx.r[5] = size; ctx.r[6] = address;
        iop::kernel::dispatch(ctx, "sysmem", op);
        return ctx.r[2];
    };
    const u32 before = sysmem(8);
    const u32 a = sysmem(4, 0, 0x100);
    const u32 b = sysmem(4, 0, 0x200);
    const u32 c = sysmem(4, 0, 0x100);
    check(a && b == a + 0x100 && c == b + 0x200, "IOP heap allocates contiguous blocks");
    check(sysmem(5, b) == 0 && sysmem(4, 0, 0x180) == b,
          "IOP heap first-fit reuses freed middle block");
    check(sysmem(5, a) == 0 && sysmem(5, b) == 0 && sysmem(5, c) == 0,
          "IOP heap releases adjacent allocations");
    check(sysmem(7) == before && sysmem(8) == before,
          "IOP heap coalesces to original maximum and total");
    const u32 high = sysmem(4, 1, 0x200);
    check(high == iop::IOP_RAM_SIZE - 0x200 && sysmem(5, high) == 0,
          "IOP sysmem high-mode allocation and release");
    check(sysmem(4, 2, 0x200, b) == b && sysmem(5, b) == 0,
          "IOP sysmem fixed-address allocation and release");
}
void test_cdvd_rpc() {
    constexpr u32 send = 0xd000, recv = 0xd100;
    iop::cdvd_rpc::call(0x80000593, 1, send, 0, recv, 16);
    check(iop::read32(recv) == 1 && iop::read8(recv + 4) == 0 &&
              iop::read8(recv + 10) != 0, "CDVD ReadClock returns success and BCD date");
    u8 sector[2048]{};
    check(iso::read_sectors(16, 1, sector), "read CDVD reference PVD");
    iop::write32(send, 16); iop::write32(send + 4, 1);
    iop::write32(send + 8, EE_DATA | 0x20000000);
    iop::write32(send + 12, 0);
    put(EE_DATA + 2048, 0xa5a5a5a5);
    iop::cdvd_rpc::call(0x80000595, 1, send, 24, recv, 4);
    check(iop::read32(recv) == 1 &&
              std::memcmp(ee_host(EE_DATA), sector, sizeof(sector)) == 0 &&
              get(EE_DATA + 2048) == 0xa5a5a5a5,
          "CDVD Read copies exactly 2048 bytes through uncached EE address");
}
void dma(u32 source, u32 destination, u32 size) {
    iop::SifDmaXfer x{source, destination, size, 0};
    if (!iop::ee_sif_set_dma(&x, 1)) throw std::runtime_error("SIF DMA transfer rejected");
}
void cmd(u32 id, u32 size) {
    put(EE_SEND, size);
    put(EE_SEND + 8, id);
    dma(EE_SEND, iop::ee_sif_get_reg(2), size);
}
void arm() {
    std::memset(ee_host(EE_RECV), 0, 128);
    iop::ee_sif_set_dchain();
}
u32 bind(u32 server, bool expected = true) {
    arm();
    std::memset(ee_host(EE_SEND), 0, 0x40);
    put(EE_SEND + 16, 77);
    put(EE_SEND + 20, EE_SEND + 0x100);
    put(EE_SEND + 24, 88);
    put(EE_SEND + 28, EE_SEND + 0x200);
    put(EE_SEND + 32, server);
    cmd(0x80000009, 0x40);
    check(get(EE_RECV + 8) == 0x80000008 && get(EE_RECV + 32) == 0x80000009,
          "Sony SIF RPC BIND end packet delivered");
    u32 ptr = get(EE_RECV + 36);
    check((ptr != 0) == expected, "RPC binding server existence");
    return ptr;
}
u32 rpc(u32 server, u32 fno, u32 request, u32 request_size,
        bool fileio = false, u32 expected_slot = 0, bool aliased_reply = false,
        u32 response_size = 4) {
    dma(request, aliased_reply ? 0xc000 : IOP_RPC_BUF, request_size);
    arm();
    std::memset(ee_host(EE_SEND), 0, 0x40);
    put(EE_SEND + 16, 77);
    put(EE_SEND + 20, EE_SEND + 0x100);
    put(EE_SEND + 24, 88);
    put(EE_SEND + 28, EE_SEND + 0x200);
    put(EE_SEND + 32, fno);
    put(EE_SEND + 36, request_size);
    put(EE_SEND + 40, EE_DATA + 1024);
    put(EE_SEND + 44, fileio && fno == 255 ? 8 : response_size);
    put(EE_SEND + 48, 1); // blocking Sony sceSifCallRpc expects RPC_END
    put(EE_SEND + 52, server);
    put(EE_SEND + 4, aliased_reply ? 0xc000 : IOP_RPC_BUF);
    cmd(0x8000000a, 0x40);
    if (fileio && fno != 255) {
        check(get(EE_RECV + 8) == 0x80000011 && get(EE_RECV + 12) == expected_slot,
              "Sony FILEIO completion packet delivered with matching slot");
        arm();
    }
    check(get(EE_RECV + 8) == 0x80000008 && get(EE_RECV + 32) == 0x8000000a,
          "Sony SIF RPC CALL end packet delivered");
    return get(EE_DATA + 1024);
}
void load(u32 server, const char* name, const char* args = "") {
    std::memset(ee_host(EE_DATA), 0, 512);
    const u32 len = std::strlen(args) + 1;
    put(EE_DATA, len);
    std::memcpy(ee_host(EE_DATA + 8), name, std::strlen(name) + 1);
    std::memcpy(ee_host(EE_DATA + 260), args, len);
    dma(EE_DATA, IOP_RPC_BUF, 512);
    arm();
    std::memset(ee_host(EE_SEND), 0, 0x40);
    put(EE_SEND + 16, 77);
    put(EE_SEND + 20, EE_SEND + 0x100);
    put(EE_SEND + 24, 88);
    put(EE_SEND + 28, EE_SEND + 0x200);
    put(EE_SEND + 32, 0); // LF_F_MOD_LOAD
    put(EE_SEND + 36, 512);
    put(EE_SEND + 40, EE_DATA + 1024);
    put(EE_SEND + 44, 8);
    put(EE_SEND + 48, 1);
    put(EE_SEND + 52, server);
    put(EE_SEND + 4, IOP_RPC_BUF); // ps2sdk sceSifSendCmd sends the extra-data destination in header.dest
    cmd(0x8000000a, 0x40);
    check(get(EE_RECV + 8) == 0x80000008 && get(EE_RECV + 32) == 0x8000000a,
          "Sony SIF RPC CALL end packet delivered");
    check(static_cast<s32>(get(EE_DATA + 1024)) >= 0, name);
}
void test_queued_rpc() {
    std::size_t count = 0;
    const iop::Module* const* modules = iop::generated_modules(count);
    u32 rpc_loop = 0, delayed_handler = 0;
    for (std::size_t n = 0; n < count; ++n) {
        if (std::strcmp(modules[n]->name, "Sirens3_Server") != 0) continue;
        for (std::size_t k = 0; k < modules[n]->import_count; ++k) {
            const iop::Import& imp = modules[n]->imports[k];
            if (std::strcmp(imp.library, "sifcmd") == 0 && imp.ordinal == 22) rpc_loop = imp.address;
            if (std::strcmp(imp.library, "thbase") == 0 && imp.ordinal == 33) delayed_handler = imp.address;
        }
    }
    check(rpc_loop && delayed_handler, "Sirens imports RPC worker loop and blocking DelayThread");
    iop::Context c{};
    c.r[5] = 0x1000;
    iop::kernel::dispatch(c, "sysmem", 4);
    const u32 memory = c.r[2];
    check(memory != 0, "Allocate IOP RPC queue/server and worker descriptor");
    const u32 queue = memory, server = memory + 0x40, buffer = memory + 0x100;
    const u32 descriptor = memory + 0x200, stack = memory + 0x800;
    iop::write32(descriptor + 8, rpc_loop);
    iop::write32(descriptor + 12, 0x1000);
    iop::write32(descriptor + 16, 48);
    c = {}; c.r[4] = descriptor;
    iop::kernel::dispatch(c, "thbase", 4);
    const u32 worker = c.r[2];
    c = {}; c.r[4] = queue; c.r[5] = worker;
    iop::kernel::dispatch(c, "sifcmd", 19);
    c = {}; c.r[4] = server; c.r[5] = 0x66554433;
    c.r[6] = delayed_handler; c.r[7] = buffer; c.r[29] = stack;
    iop::write32(stack + 16, 0);                // cfunc
    iop::write32(stack + 20, memory + 0x300);   // cbuf
    iop::write32(stack + 24, queue);
    iop::kernel::dispatch(c, "sifcmd", 17);
    c = {}; c.r[4] = worker; c.r[5] = queue;
    iop::kernel::dispatch(c, "thbase", 6);
    iop::run(); // RPC worker blocks in sceSifRpcLoop until it receives a request.
    const u32 bound = bind(0x66554433);
    check(get(EE_RECV + 40) == buffer && get(EE_RECV + 44) == memory + 0x300,
          "Sony RPC BIND exposes the server receive buffer and callback buffer");
    auto request = [&](u32 mode, u32 micros) {
        put(EE_DATA, 0xaabbccdd);
        dma(EE_DATA, buffer, 4);
        arm();
        std::memset(ee_host(EE_SEND), 0, 64);
        put(EE_SEND + 16, 17);
        put(EE_SEND + 20, EE_SEND + 0x100);
        put(EE_SEND + 24, 19);
        put(EE_SEND + 28, EE_SEND + 0x300);
        put(EE_SEND + 32, micros);
        put(EE_SEND + 36, 4);
        put(EE_SEND + 48, mode);
        put(EE_SEND + 52, bound);
        put(EE_SEND + 4, buffer);
        cmd(0x8000000a, 64);
    };
    request(1, 25000);
    check(get(EE_RECV + 8) == 0,
          "Blocking guest RPC does not acknowledge before its IOP server thread runs");
    iop::run();
    check(get(EE_RECV + 8) == 0,
          "Server DelayThread suspends RPC response while the IOP fiber waits");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (get(EE_RECV + 8) == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        iop::run();
    }
    check(get(EE_RECV + 8) == 0x80000008 && get(EE_RECV + 32) == 0x8000000a &&
              get(EE_RECV + 28) == EE_SEND + 0x300,
          "IOP server fiber resumes and sends Sony RPC END with original EE client pointer");
    const u32 interrupts_before = sif_interrupts;
    request(0, 1000);
    put(EE_SEND + 0x100 + 16, 1);
    put(EE_SEND + 0x100 + 24, 17);
    const auto no_wait_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (get(EE_SEND + 0x100 + 16) != 0 &&
           std::chrono::steady_clock::now() < no_wait_deadline) {
        iop::run();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(get(EE_SEND + 0x100 + 16) == 0 && get(EE_SEND + 0x100 + 24) == 0 &&
              sif_interrupts == interrupts_before,
          "Sony NOWAIT RPC without end callback clears packet without SIF0 interrupt");
    iop::write32(buffer, 0x1234abcd);
    arm();
    std::memset(ee_host(EE_SEND), 0, 64);
    put(EE_SEND + 16, 13);
    put(EE_SEND + 20, EE_SEND + 0x100);
    put(EE_SEND + 24, 15);
    put(EE_SEND + 28, EE_SEND + 0x300);
    put(EE_SEND + 32, buffer);
    put(EE_SEND + 36, EE_DATA + 0x1800);
    put(EE_SEND + 40, 4);
    cmd(0x8000000c, 64);
    check(get(EE_DATA + 0x1800) == 0x1234abcd &&
              get(EE_RECV + 8) == 0x80000008 &&
              get(EE_RECV + 32) == 0x8000000c &&
              get(EE_RECV + 28) == EE_SEND + 0x300,
          "Sony RDATA copies guest memory and replies with RDATA CID, not RPC CALL");
}
}
namespace platform {
u64 now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count(); }
PadState pad_state_for_test{false, 0xffff, 128, 128, 128, 128, {}};
PadState pad_state(int port) {
    return port == 0 ? pad_state_for_test : PadState{false, 0xffff, 128, 128, 128, 128, {}};
}
}
namespace spu2 {
namespace {
u16 registers[0x400]{};
void (*irq_callback)(int) = nullptr;
void (*dma_callback)(int) = nullptr;
}
void init() { std::memset(registers, 0, sizeof(registers)); }
void reset() { init(); }
void shutdown() {}
u16 read16(u32 addr) {
    const u32 offset = (addr - 0x1f900000) & 0x7ff;
    return (offset & 0x3ff) == 0x344 ? 0x80 : registers[offset / 2];
}
void write16(u32 addr, u16 value) { registers[((addr - 0x1f900000) & 0x7ff) / 2] = value; }
void dma_write(int core, const u16*, u32) { if (dma_callback) dma_callback(core); }
void dma_read(int core, u16* target, u32 words) {
    std::memset(target, 0, words * sizeof(u16));
    if (dma_callback) dma_callback(core);
}
void advance(u32) {}
void sync() {}
void set_irq_callback(void (*callback)(int)) { irq_callback = callback; }
void set_dma_callback(void (*callback)(int)) { dma_callback = callback; }
}
void intc_raise(u32) {}
void dmac_raise(u32 channel) { if (channel == DMAC_SIF0) ++sif_interrupts; }
int main() {
    try {
        ee_mem_init();
        check(iso::open(IOP_TEST_ISO), "open Transformers ISO");
        iop::init();
        test_heap();
        check((iop::ee_sif_get_reg(4) & 0x70000) == 0x70000, "IOP SIF boot flags");
        check(iop::ee_sif_get_reg(2) != 0, "IOP SIF SUBADDR");
        test_cdvd_rpc();
        iop::write32(0x1f8010c0, 0x2000);
        iop::write32(0x1f8010c4, 16);
        iop::write32(0x1f8010c8, 0x01000001);
        check((iop::read32(0x1f8010c8) & 0x01000000) == 0 &&
              (iop::read32(0x1f801070) & (1u << 9)),
              "SPU2 DMA channel 4 completes and raises IOP IRQ");
        iop::write32(0x1f801500, 0x3000);
        iop::write32(0x1f801504, 16);
        iop::write32(0x1f801508, 0x01000000);
        check((iop::read32(0x1f801508) & 0x01000000) == 0 &&
              (iop::read32(0x1f801070) & (1u << 10)),
              "SPU2 DMA channel 7 completes and raises IOP IRQ");
        put(EE_SEND + 16, EE_RECV);
        cmd(0x80000000, 0x20); // CHANGE_SADDR
        arm();
        check(get(EE_RECV + 8) == 0x80000001 &&
                  get(EE_RECV + 16) == 0 && get(EE_RECV + 20) == 1,
              "IOP announces RPC initialization through SIF SET_SREG");
        u32 heap = bind(0x80000003);
        put(EE_DATA, 0x300);
        const u32 heap_addr = rpc(heap, 1, EE_DATA, 4);
        check(heap_addr >= 0xb0000 && heap_addr < iop::IOP_RAM_SIZE,
              "IOP heap RPC allocates guest RAM");
        put(EE_DATA, heap_addr);
        check(rpc(heap, 2, EE_DATA, 4) == 0 && rpc(heap, 7, EE_DATA, 0) >= 0x100000,
              "IOP heap RPC frees and coalesces guest RAM");
        arm();
        std::memset(ee_host(EE_SEND), 0, 64);
        put(EE_SEND + 16, 1);
        put(EE_SEND + 20, EE_SEND + 0x100);
        put(EE_SEND + 24, 17);
        put(EE_SEND + 28, EE_SEND + 0x200);
        put(EE_SEND + 32, 5); // IOP heap total RAM
        put(EE_SEND + 40, EE_DATA + 1024);
        put(EE_SEND + 44, 4);
        put(EE_SEND + 52, heap);
        put(EE_SEND + 4, IOP_RPC_BUF);
        put(EE_SEND + 0x100 + 16, 1);
        put(EE_SEND + 0x100 + 24, 17);
        const u32 before_nowait = sif_interrupts;
        cmd(0x8000000a, 64);
        check(get(EE_DATA + 1024) == iop::IOP_RAM_SIZE &&
                  get(EE_SEND + 0x100 + 16) == 0 &&
                  get(EE_SEND + 0x100 + 24) == 0 &&
                  sif_interrupts == before_nowait,
              "HLE heap NOWAIT call DMA-writes result and clears EE packet without RPC END");
        put(EE_DATA, 0x10000);
        const u32 image_addr = rpc(heap, 1, EE_DATA, 4);
        const char* image_path = "cdrom0:\\IOP\\TWOFISH.IRX;1";
        put(EE_DATA, image_addr);
        std::memcpy(ee_host(EE_DATA + 4), image_path, std::strlen(image_path) + 1);
        check(rpc(heap, 3, EE_DATA, 4 + std::strlen(image_path) + 1) == 0 &&
                  iop::read32(image_addr) == 0x464c457f,
              "IOP heap RPC loads ISO module bytes into allocated RAM");
        put(EE_DATA, image_addr);
        check(rpc(heap, 2, EE_DATA, 4) == 0, "IOP heap RPC frees loaded module buffer");
        const u32 fileio = bind(0x80000001);
        constexpr u32 fileio_result = EE_DATA + 0x2000;
        constexpr u32 fileio_result_ptr = EE_DATA + 0x2800;
        put(EE_DATA, fileio_result | 0x20000000);
        put(EE_DATA + 4, EE_DATA + 0x2440);
        check(rpc(fileio, 255, EE_DATA, 8, true, 0, true) == 0x30303832 &&
                  get(EE_DATA + 1028) == 2,
              "Sony FILEIO RPC advertises version 2800 and indexed completions");
        std::memset(ee_host(EE_DATA), 0, 1048);
        put(EE_DATA, 23);
        put(EE_DATA + 4, fileio_result_ptr);
        put(EE_DATA + 8, 4);
        put(EE_DATA + 12, 1);
        std::memcpy(ee_host(EE_DATA + 20), image_path, std::strlen(image_path) + 1);
        const u32 transport = rpc(fileio, 0, EE_DATA, 1048, true);
        const u32 fd = get(fileio_result + 16);
        check(transport == 1 && fd >= 3 && fd < 67 &&
                  get(fileio_result) == 23 && get(fileio_result + 4) == 0 &&
                  get(fileio_result + 8) == fileio_result_ptr,
              "Sony FILEIO 2.8 OPEN completion returns ISO fd");
        std::memset(ee_host(EE_DATA), 0, 32);
        put(EE_DATA, 23);
        put(EE_DATA + 4, fileio_result_ptr);
        put(EE_DATA + 8, 4);
        put(EE_DATA + 12, fd);
        put(EE_DATA + 16, (EE_DATA + 0x1101) | 0x20000000);
        put(EE_DATA + 20, 67);
        check(rpc(fileio, 2, EE_DATA, 32, true) == 1 &&
                  get(fileio_result + 16) == 67 &&
                  get(EE_DATA + 0x1101) == 0x464c457f &&
                  get(fileio_result + 20) == 63,
              "Sony FILEIO 2.8 READ returns unaligned data and 64-byte callback fragments");
        std::memset(ee_host(EE_DATA), 0, 20);
        put(EE_DATA, 23);
        put(EE_DATA + 4, fileio_result_ptr);
        put(EE_DATA + 8, 4);
        put(EE_DATA + 12, fd);
        check(rpc(fileio, 1, EE_DATA, 20, true) == 1 && get(fileio_result + 16) == 0,
              "Sony FILEIO 2.8 CLOSE completion closes ISO file");
        std::memset(ee_host(EE_DATA), 0, 1048);
        put(EE_DATA, 23);
        put(EE_DATA + 4, fileio_result_ptr);
        put(EE_DATA + 8, 4);
        put(EE_DATA + 12, 1);
        put(EE_DATA + 1044, 1);
        std::memcpy(ee_host(EE_DATA + 20), image_path, std::strlen(image_path) + 1);
        check(rpc(fileio, 0, EE_DATA, 1048, true, 1) == 1 &&
                  get(fileio_result + 0x440) == 23 &&
                  get(fileio_result + 0x444) == 0 &&
                  get(fileio_result + 0x450) >= 3,
              "Sony FILEIO indexed OPEN writes the selected EE record");
        const u32 second_fd = get(fileio_result + 0x450);
        std::memset(ee_host(EE_DATA), 0, 20);
        put(EE_DATA, 23);
        put(EE_DATA + 4, fileio_result_ptr);
        put(EE_DATA + 8, 4);
        put(EE_DATA + 12, second_fd);
        put(EE_DATA + 16, 1);
        check(rpc(fileio, 1, EE_DATA, 20, true, 1) == 1 &&
                  get(fileio_result + 0x450) == 0,
              "Sony FILEIO indexed CLOSE follows request slot");
        constexpr char romver_path[] = "rom0:ROMVER";
        std::memset(ee_host(EE_DATA), 0, 1048);
        put(EE_DATA, 23);
        put(EE_DATA + 4, fileio_result_ptr);
        put(EE_DATA + 8, 4);
        put(EE_DATA + 12, 1);
        std::memcpy(ee_host(EE_DATA + 20), romver_path, sizeof(romver_path));
        check(rpc(fileio, 0, EE_DATA, 1048, true) == 1 &&
                  get(fileio_result + 16) >= 3,
              "FILEIO opens retail BIOS rom0:ROMVER");
        const u32 romver_fd = get(fileio_result + 16);
        std::memset(ee_host(EE_DATA), 0, 32);
        put(EE_DATA, 23);
        put(EE_DATA + 4, fileio_result_ptr);
        put(EE_DATA + 8, 4);
        put(EE_DATA + 12, romver_fd);
        put(EE_DATA + 16, EE_DATA + 0x1101);
        put(EE_DATA + 20, 20);
        check(rpc(fileio, 2, EE_DATA, 32, true) == 1 &&
                  get(fileio_result + 16) == 16 &&
                  std::memcmp(ee_host(EE_DATA + 0x1101), "0200EC20040614\n", 16) == 0,
              "FILEIO ROMVER read returns exact European retail BIOS bytes and EOF");
        std::memset(ee_host(EE_DATA), 0, 28);
        put(EE_DATA, 23);
        put(EE_DATA + 4, fileio_result_ptr);
        put(EE_DATA + 8, 4);
        put(EE_DATA + 12, romver_fd);
        put(EE_DATA + 16, static_cast<u32>(-2));
        put(EE_DATA + 20, 2);
        check(rpc(fileio, 4, EE_DATA, 28, true) == 1 &&
                  get(fileio_result + 16) == 14,
              "FILEIO ROMVER seek is relative to 16-byte end");
        put(EE_DATA + 16, EE_DATA + 0x1101);
        put(EE_DATA + 20, 2);
        check(rpc(fileio, 2, EE_DATA, 32, true) == 1 &&
                  get(fileio_result + 16) == 2 &&
                  ee_host(EE_DATA + 0x1101)[0] == '\n' &&
                  ee_host(EE_DATA + 0x1101)[1] == 0,
              "FILEIO ROMVER read after seek returns terminating newline and NUL");
        std::memset(ee_host(EE_DATA), 0, 64);
        put(EE_DATA, 23);
        put(EE_DATA + 4, fileio_result_ptr);
        put(EE_DATA + 8, 4);
        put(EE_DATA + 12, EE_DATA + 0x3000);
        std::memcpy(ee_host(EE_DATA + 16), romver_path, sizeof(romver_path));
        check(rpc(fileio, 12, EE_DATA, 16 + sizeof(romver_path), true) == 1 &&
                  get(fileio_result + 16) == 0 && get(fileio_result + 32) == 16,
              "FILEIO ROMVER getstat reports the 16-byte read-only file");
        std::memset(ee_host(EE_DATA), 0, 20);
        put(EE_DATA, 23);
        put(EE_DATA + 4, fileio_result_ptr);
        put(EE_DATA + 8, 4);
        put(EE_DATA + 12, romver_fd);
        check(rpc(fileio, 1, EE_DATA, 20, true) == 1 &&
                  get(fileio_result + 16) == 0,
              "FILEIO closes ROMVER handle");
        // Two IOP completions in the same request slot may queue before the
        // EE re-arms SIF0. Its record must match each delivered packet.
        const u32 first_reply[5] = {0x111, 23, fileio_result_ptr, 4, 11};
        const u32 second_reply[5] = {0x222, 23, fileio_result_ptr, 4, 22};
        put(fileio_result, 0xdeadbeef);
        iop::send_fileio_completion(fileio_result, first_reply, sizeof(first_reply));
        iop::send_fileio_completion(fileio_result, second_reply, sizeof(second_reply));
        check(get(fileio_result) == 0xdeadbeef,
              "FILEIO completion remains deferred while SIF0 is disarmed");
        arm();
        check(get(EE_RECV + 8) == 0x80000011 &&
                  get(fileio_result) == 0x111 && get(fileio_result + 16) == 11,
              "First FILEIO completion matches first queued SIF packet");
        arm();
        check(get(EE_RECV + 8) == 0x80000011 &&
                  get(fileio_result) == 0x222 && get(fileio_result + 16) == 22,
              "Second FILEIO completion follows next SIF0 re-arm");
        std::memset(ee_host(EE_DATA), 0, 64);
        put(EE_DATA, 23);
        put(EE_DATA + 4, fileio_result_ptr);
        put(EE_DATA + 8, 4);
        put(EE_DATA + 12, EE_DATA + 0x3000);
        std::memcpy(ee_host(EE_DATA + 16), image_path, std::strlen(image_path) + 1);
        check(rpc(fileio, 12, EE_DATA, 17 + std::strlen(image_path), true) == 1 &&
                  get(fileio_result + 16) == 0 &&
                  (get(fileio_result + 24) & 0xf000) == 0x2000 &&
                  get(fileio_result + 32) > 0,
              "Sony FILEIO 2.8 GETSTAT returns ISO file mode and size");
        constexpr char iop_dir[] = "cdrom0:\\IOP";
        std::memset(ee_host(EE_DATA), 0, 64);
        put(EE_DATA, 23);
        put(EE_DATA + 4, fileio_result_ptr);
        put(EE_DATA + 8, 4);
        std::memcpy(ee_host(EE_DATA + 12), iop_dir, sizeof(iop_dir));
        check(rpc(fileio, 9, EE_DATA, 12 + sizeof(iop_dir), true) == 1 &&
                  get(fileio_result + 16) >= 3,
              "Sony FILEIO 2.8 DOPEN opens ISO directory");
        const u32 dir_fd = get(fileio_result + 16);
        bool found_twofish = false;
        for (u32 i = 0; i < 128; ++i) {
            std::memset(ee_host(EE_DATA), 0, 32);
            put(EE_DATA, 23);
            put(EE_DATA + 4, fileio_result_ptr);
            put(EE_DATA + 8, 4);
            put(EE_DATA + 12, dir_fd);
            put(EE_DATA + 16, EE_DATA + 0x3000);
            check(rpc(fileio, 11, EE_DATA, 32, true) == 1,
                  "Sony FILEIO 2.8 DREAD transport");
            if (get(fileio_result + 16) == 0) break;
            if (std::strstr(reinterpret_cast<const char*>(ee_host(fileio_result + 88)), "TWOFISH.IRX"))
                found_twofish = true;
        }
        check(found_twofish, "Sony FILEIO 2.8 DREAD enumerates ISO directory records");
        std::memset(ee_host(EE_DATA), 0, 20);
        put(EE_DATA, 23);
        put(EE_DATA + 4, fileio_result_ptr);
        put(EE_DATA + 8, 4);
        put(EE_DATA + 12, dir_fd);
        check(rpc(fileio, 10, EE_DATA, 20, true) == 1 &&
                  get(fileio_result + 16) == 0,
              "Sony FILEIO 2.8 DCLOSE releases ISO directory");
        bind(0x80000592);
        bind(0x80000593);
        bind(0x80000595);
        const u32 search_server = bind(0x80000597);
        std::memset(ee_host(EE_DATA), 0, 0x124);
        std::memcpy(ee_host(EE_DATA + 0x20), image_path, std::strlen(image_path) + 1);
        put(EE_DATA + 0x120, (EE_DATA + 0x1600) | 0x20000000);
        check(rpc(search_server, 0, EE_DATA, 0x124) == 1 &&
                  get(EE_DATA + 0x1600) != 0 &&
                  get(EE_DATA + 0x1604) != 0,
              "CDVD SearchFile copies ISO record to aliased EE RAM");
        const u32 diskready = bind(0x8000059a);
        const u32 diskready2 = bind(0x8000059c);
        put(EE_DATA, 0);
        check(rpc(diskready, 0, EE_DATA, 4) == 2 &&
                  rpc(diskready2, 0, EE_DATA, 4) == 2,
              "Sony CDVD legacy and extended DiskReady servers report mounted DVD");
        u32 lf = bind(0x80000006);
        load(lf, "cdrom0:\\IOP\\SIO2MAN.IRX;1");
        load(lf, "cdrom0:\\IOP\\TWOFISH.IRX;1");
        // Give TwoFish startup its own IOP fiber before issuing an EE user command.
        for (int i = 0; i < 64; ++i) iop::run();
        // Incoming TwoFish command ID 0, payload signature at +0x0c.
        std::memset(ee_host(EE_SEND), 0, 32);
        put(EE_SEND + 12, 0xf154f154);
        put(EE_SEND + 16, 16);
        put(EE_SEND + 20, 1);
        put(EE_SEND + 24, EE_DATA + 2048);
        put(EE_SEND + 28, EE_DATA + 0x1800);
        const u32 interrupts_before_read = sif_interrupts;
        arm();
        cmd(0, 32);
        bool completed = false;
        for (int i = 0; i < 1024 && !completed; ++i) {
            iop::run();
            if (get(EE_RECV + 8) == 0 && get(EE_RECV + 12) == 0xf154f154 &&
                get(EE_RECV + 16) == EE_DATA + 0x1800) {
                completed = true;
            } else if (get(EE_RECV) != 0) {
                arm(); // one SIF0 packet at a time, as Sony libsif does
            }
        }
        u8 pvd[2048]{};
        check(iso::read_sectors(16, 1, pvd), "read independent PVD reference");
        check(std::memcmp(ee_host(EE_DATA + 2048), pvd, sizeof pvd) == 0,
              "TwoFish DMA copied PVD into EE RAM");
        check(completed && sif_interrupts > interrupts_before_read,
              "TwoFish completion packet delivered through armed SIF0");
        load(lf, "cdrom0:\\IOP\\PADMAN.IRX;1");
        load(lf, "cdrom0:\\IOP\\LIBSD.IRX;1");
        iso::Entry midi{};
        check(iso::find("cdrom0:\\IOP\\MODMIDI.IRX;1", &midi), "Locate MODMIDI buffer in ISO");
        put(EE_DATA, midi.size);
        const u32 midi_buffer = rpc(heap, 1, EE_DATA, 4);
        check(midi_buffer && midi_buffer <= iop::IOP_RAM_SIZE - midi.size &&
                  iso::read_bytes(static_cast<u64>(midi.lba) * iso::SECTOR_SIZE, midi.size,
                                  iop::g_iop_ram + midi_buffer),
              "Copy original MODMIDI IRX into IOP heap");
        std::memset(ee_host(EE_DATA), 0, 512);
        put(EE_DATA, midi_buffer);
        check(static_cast<s32>(rpc(lf, 6, EE_DATA, 512)) >= 0,
              "Sony loadfile LoadModuleBuffer identifies and starts original MODMIDI IRX");
        put(EE_DATA, midi_buffer);
        check(rpc(heap, 2, EE_DATA, 4) == 0,
              "IOP heap releases Sony LoadModuleBuffer source after relocation");
        load(lf, "cdrom0:\\IOP\\SIRENS3.IRX;1", "RETAIL=1;HEAPSIZE=131072;");
        load(lf, "cdrom0:\\IOP\\MCMAN.IRX;1");
        load(lf, "cdrom0:\\IOP\\MCSERV.IRX;1");
        for (int i = 0; i < 200; ++i) {
            iop::vblank(true);
            iop::run();
            iop::vblank(false);
            iop::run();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        bind(0x00514e95);
        test_queued_rpc();
        const u32 pad = bind(0x80000100);
        constexpr u32 pad_area = EE_DATA + 0x3000;
        auto pad_rpc = [&](u32 method) {
            std::memset(ee_host(EE_DATA), 0, 128);
            put(EE_DATA, method);
        };
        pad_rpc(0x12);
        rpc(pad, 1, EE_DATA, 128, false, 0, true, 128);
        check(get(EE_DATA + 1024 + 12) == 0x405,
              "Sony libpad sees a PADMAN 4.05-compatible module version");
        pad_rpc(0x10);
        rpc(pad, 1, EE_DATA, 128, false, 0, true, 128);
        check(get(EE_DATA + 1024 + 12) == 1, "Aliased Sony PADMAN initialization succeeds");
        platform::pad_state_for_test = {true, 0xffef, 0x42, 0x43, 0x44, 0x45, {}};
        pad_rpc(1);
        put(EE_DATA + 4, 0);
        put(EE_DATA + 8, 0);
        put(EE_DATA + 16, pad_area);
        rpc(pad, 1, EE_DATA, 128, false, 0, true, 128);
        check(get(EE_DATA + 1024 + 12) == 1,
              "Sony padPortOpen preserves overlapping request/reply and opens EE DMA area");
        iop::vblank(true);
        const u32 newest = get(pad_area + 88) > get(pad_area + 216) ? pad_area : pad_area + 128;
        const u8* buttons = ee_host(newest);
        check(get(newest + 96) == 8 && buttons[1] == 0x73 &&
                  buttons[2] == 0xef && buttons[3] == 0xff &&
                  buttons[4] == 0x42 && buttons[112] == 6 && buttons[113] == 0,
              "Sony padRead/getState select the latest stable double-buffered DMA frame");
        pad_rpc(10);
        put(EE_DATA + 12, 0xfff);
        rpc(pad, 1, EE_DATA, 128, false, 0, true, 128);
        iop::vblank(true);
        const u32 pressure = get(pad_area + 88) > get(pad_area + 216) ? pad_area : pad_area + 128;
        check(get(EE_DATA + 1024 + 16) == 1 && ee_host(pressure)[1] == 0x79 &&
                  get(pressure + 96) == 20 && get(pressure + 121) == 0x3ffff,
              "Sony padSetButtonInfo enables pressure packet and DMA capability mask");
        pad_rpc(6);
        put(EE_DATA + 12, 0);
        put(EE_DATA + 16, 1);
        rpc(pad, 1, EE_DATA, 128, false, 0, true, 128);
        iop::vblank(true);
        const u32 digital = get(pad_area + 88) > get(pad_area + 216) ? pad_area : pad_area + 128;
        check(get(EE_DATA + 1024 + 20) == 1 && ee_host(digital)[1] == 0x41 &&
                  get(digital + 96) == 4 && ee_host(digital)[110] == 1,
              "Sony padSetMainMode updates the next DMA frame and reports success");
        pad_rpc(8);
        const u8 alignment[6] = {0, 1, 0xff, 0xff, 0xff, 0xff};
        std::memcpy(ee_host(EE_DATA + 12), alignment, sizeof(alignment));
        rpc(pad, 1, EE_DATA, 128, false, 0, true, 128);
        iop::vblank(true);
        const u32 aligned = get(pad_area + 88) > get(pad_area + 216) ? pad_area : pad_area + 128;
        check(get(EE_DATA + 1024 + 20) == 1 &&
                  std::memcmp(ee_host(aligned + 40), alignment, sizeof(alignment)) == 0,
              "Sony padSetActAlign persists actuator mapping in EE DMA");
        pad_rpc(0x1a);
        rpc(pad, 1, EE_DATA, 128, false, 0, true, 128);
        check(get(EE_DATA + 1024 + 12) == 1,
              "Unknown PADMAN method reports nonfatal compatibility result");
        platform::pad_state_for_test.connected = false;
        bind(0x80000400);
        std::memset(ee_host(EE_SEND), 0, 32);
        cmd(0x80000003, 32); // RESET retains EE packet buffer and restarts IOP services.
        check((iop::ee_sif_get_reg(4) & 0x70000) == 0,
              "IOP reset defers reboot-ready flags");
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        check((iop::ee_sif_get_reg(4) & 0x70000) == 0x70000,
              "IOP reboot republishes Sony boot flags");
        put(EE_SEND + 16, EE_RECV);
        cmd(0x80000000, 32);
        arm();
        check(get(EE_RECV + 8) == 0x80000001,
              "IOP reboot reannounces RPC initialization");
        bind(0x80000001);
        bind(0x80000003);
        bind(0x8000059c);
        bind(0x80000100, false);
        bind(0x00514e95, false);
        iop::shutdown();
        iso::close();
        std::puts("IOP integration passed");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "IOP integration FAILED: %s\n", e.what());
        return EXIT_FAILURE;
    }
}
