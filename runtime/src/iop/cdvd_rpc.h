#pragma once
#include "ps2/types.h"

namespace iop::cdvd_rpc {
// SIF RPC send/receive packets reside in IOP RAM; file and sector DMA targets
// encoded in their payloads reside in EE RAM.
void call(u32 sid, u32 fno, u32 send, u32 send_size, u32 recv, u32 recv_size);
}
