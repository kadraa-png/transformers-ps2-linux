#pragma once

#include "ps2/types.h"

namespace iop {
struct Context;
namespace kernel {

void reset();
void step();
void start_module(u32 entry, const Context& context);
void checkpoint();
bool has_work();
u64 deadline();
void interrupt(u32 irq);
void vblank(bool start);
u32 dispatch(Context& context, const char* library, u32 ordinal);
bool has_rpc_server(u32 server_id);
u32 rpc_buffer(u32 server_id, u32& cbuf);
bool supports(const char* library, u32 ordinal);
void rpc_request(u32 server_id, u32 fno, u32 send_addr, u32 send_size, u32 token);
void heap_rpc(u32 fno, u32 send_addr, u32 send_size, u32 recv_addr, u32 recv_size);
void command(u32 packet_addr, u32 size);
void dump_threads();

} // namespace kernel
} // namespace iop
