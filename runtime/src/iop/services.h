#pragma once
#include "ps2/types.h"

namespace iop::services {
// send/recv are IOP RAM addresses; embedded DMA pointers in requests are EE addresses.
void call(u32 sid, u32 fno, u32 send, u32 send_size, u32 recv, u32 recv_size);
void vblank(bool start);
void reset();
} // namespace iop::services
