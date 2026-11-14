/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010 PCSX2 Dev Team
 *
 *  This file is distributed under the terms of the GNU Lesser General Public
 *  License, version 3 or (at your option) any later version.
 *  See <https://www.gnu.org/licenses/>.
 */
#pragma once
#include "ps2/vu.h"

// The macro interpreter takes the complete COP2 instruction word.  The
// decoder in tools/r5900.py supplies the opcode, including the special2
// function field; do not rebuild it from the helper name.
void vu0_macro_exec(u32 word, bool lower);

#define VU0_UPPER(NAME) PS2_FORCEINLINE void vu0m_##NAME(u32 word) { vu0_macro_exec(word, false); }
#define VU0_LOWER(NAME) PS2_FORCEINLINE void vu0m_##NAME(u32 word) { vu0_macro_exec(word, true); }

// COP2 SPECIAL1, funct 0x00..0x2f (broadcast variants occupy four functs).
VU0_UPPER(vaddbc) VU0_UPPER(vsubbc) VU0_UPPER(vmaddbc) VU0_UPPER(vmsubbc)
VU0_UPPER(vmaxbc) VU0_UPPER(vminibc) VU0_UPPER(vmulbc)
VU0_UPPER(vmulq) VU0_UPPER(vmaxi) VU0_UPPER(vmuli) VU0_UPPER(vminii)
VU0_UPPER(vaddq) VU0_UPPER(vmaddq) VU0_UPPER(vaddi) VU0_UPPER(vmaddi)
VU0_UPPER(vsubq) VU0_UPPER(vmsubq) VU0_UPPER(vsubi) VU0_UPPER(vmsubi)
VU0_UPPER(vadd) VU0_UPPER(vmadd) VU0_UPPER(vmul) VU0_UPPER(vmax)
VU0_UPPER(vsub) VU0_UPPER(vmsub) VU0_UPPER(vopmsub) VU0_UPPER(vmini)

// COP2 SPECIAL2, index = (sa << 2) | (funct & 3), upper: 0x00..0x2f.
VU0_UPPER(vaddabc) VU0_UPPER(vsubabc) VU0_UPPER(vmaddabc)
VU0_UPPER(vmsubabc) VU0_UPPER(vmulabc)
VU0_UPPER(vitof0) VU0_UPPER(vitof4) VU0_UPPER(vitof12) VU0_UPPER(vitof15)
VU0_UPPER(vftoi0) VU0_UPPER(vftoi4) VU0_UPPER(vftoi12) VU0_UPPER(vftoi15)
VU0_UPPER(vmulaq) VU0_UPPER(vabs) VU0_UPPER(vmulai) VU0_UPPER(vclipw)
VU0_UPPER(vaddaq) VU0_UPPER(vmaddaq) VU0_UPPER(vaddai) VU0_UPPER(vmaddai)
VU0_UPPER(vsubaq) VU0_UPPER(vmsubaq) VU0_UPPER(vsubai) VU0_UPPER(vmsubai)
VU0_UPPER(vadda) VU0_UPPER(vmadda) VU0_UPPER(vmula)
VU0_UPPER(vsuba) VU0_UPPER(vmsuba) VU0_UPPER(vopmula) VU0_UPPER(vnop)

// COP2 SPECIAL1 integer operations, funct 0x30..0x35.
VU0_LOWER(viadd) VU0_LOWER(visub) VU0_LOWER(viaddi)
VU0_LOWER(viand) VU0_LOWER(vior)

// VCALLMS and VCALLMSR execute the VU0 program synchronously.  They are
// control flow, not an ordinary interpreter opcode dispatch.
PS2_FORCEINLINE void vu0m_vcallms(u32 word) { vu0_callms(((word >> 6) & 0x7fff) * 8); }
PS2_FORCEINLINE void vu0m_vcallmsr(u32) { vu0_callms(g_vu0.vi[27] * 8); }

// COP2 SPECIAL2 lower operations, index 0x30..0x43.
VU0_LOWER(vmove) VU0_LOWER(vmr32)
VU0_LOWER(vlqi) VU0_LOWER(vsqi) VU0_LOWER(vlqd) VU0_LOWER(vsqd)
VU0_LOWER(vdiv) VU0_LOWER(vsqrt) VU0_LOWER(vrsqrt) VU0_LOWER(vwaitq)
VU0_LOWER(vmtir) VU0_LOWER(vmfir) VU0_LOWER(vilwr) VU0_LOWER(viswr)
VU0_LOWER(vrnext) VU0_LOWER(vrget) VU0_LOWER(vrinit) VU0_LOWER(vrxor)

#undef VU0_UPPER
#undef VU0_LOWER
