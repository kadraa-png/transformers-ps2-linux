#pragma once
#include "ps2/types.h"

namespace iop::fileio_rpc {
void call(u32 fno, u32 send, u32 send_size, u32 recv, u32 recv_size);
void reset();
} // namespace iop::fileio_rpc
