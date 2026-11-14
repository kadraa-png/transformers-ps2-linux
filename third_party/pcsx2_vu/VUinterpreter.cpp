/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

// Adapted from PCSX2 1.6 VU0microInterp.cpp and VU1microInterp.cpp.
#include "VUops.h"
#include "vu_compiled.h"
#include <cstring>

extern void _vuFlushAll(VURegs* VU);
// The host delivers VU D/T interrupts and clears VIF STAT.VEW when E completes.
extern void vu_port_intc_irq(unsigned idx);
extern void vu_port_vif_vew_end(unsigned idx);

static constexpr u32 VU0_PROGMASK = 0x0fff;
static constexpr u32 VU1_PROGMASK = 0x3fff;

static void _vu0ExecUpper(VURegs* VU, u32 *ptr, const VUCompiledPair* pair) {
	VU->code = ptr[1];
	if (pair) pair->upper[0](); else VU0_UPPER_OPCODE[VU->code & 0x3f]();
}

static void _vu0ExecLower(VURegs* VU, u32 *ptr, const VUCompiledPair* pair) {
	VU->code = ptr[0];
	if (pair) pair->lower[0](); else VU0_LOWER_OPCODE[VU->code >> 25]();
}

int vu0branch = 0;
static void _vu0Exec(VURegs* VU, const VUCompiledPair* pair = nullptr)
{
	_VURegsNum lregs;
	_VURegsNum uregs;
	VECTOR _VF;
	VECTOR _VFc;
	REG_VI _VI;
	REG_VI _VIc;
	u32 *ptr;
	int vfreg;
	int vireg;
	int discard=0;

	ptr = pair ? const_cast<u32*>(&pair->lower_word) : (u32*)&VU->Micro[VU->VI[REG_TPC].UL];
	VU->VI[REG_TPC].UL+=8;

	if (ptr[1] & 0x40000000) {
		VU->ebit = 2;
	}
	if (ptr[1] & 0x20000000) { /* M flag */
		VU->flags|= VUFLAG_MFLAGSET;
	}
	if (ptr[1] & 0x10000000) { /* D flag */
		if (VU0.VI[REG_FBRST].UL & 0x4) {
			vpu_stat_set(0x2);
			vu_port_intc_irq(0);
			VU->ebit = 1;
		}
	}
	if (ptr[1] & 0x08000000) { /* T flag */
		if (VU0.VI[REG_FBRST].UL & 0x8) {
			vpu_stat_set(0x4);
			vu_port_intc_irq(0);
			VU->ebit = 1;
		}
	}

	VU->code = ptr[1];
	if (pair) uregs = pair->upper_regs[0]; else VU0regs_UPPER_OPCODE[VU->code & 0x3f](&uregs);
#ifndef INT_VUSTALLHACK
	_vuTestUpperStalls(VU, &uregs);
#endif

	/* check upper flags */
	if (ptr[1] & 0x80000000) { /* I flag */
		_vu0ExecUpper(VU, ptr, pair);

		VU->VI[REG_I].UL = ptr[0];
		memset(&lregs, 0, sizeof(lregs));
	} else {
		VU->code = ptr[0];
		if (pair) lregs = pair->lower_regs[0]; else VU0regs_LOWER_OPCODE[VU->code >> 25](&lregs);
#ifndef INT_VUSTALLHACK
		_vuTestLowerStalls(VU, &lregs);
#endif

		vu0branch = lregs.pipe == VUPIPE_BRANCH;

		vfreg = 0; vireg = 0;
		if (uregs.VFwrite) {
			if (lregs.VFwrite == uregs.VFwrite) {
				discard = 1;
			}
			if (lregs.VFread0 == uregs.VFwrite ||
				lregs.VFread1 == uregs.VFwrite) {
				_VF = VU->VF[uregs.VFwrite];
				vfreg = uregs.VFwrite;
			}
		}
		if (uregs.VIread & (1 << REG_CLIP_FLAG)) {
			if (lregs.VIwrite & (1 << REG_CLIP_FLAG)) {
				discard = 1;
			}
			if (lregs.VIread & (1 << REG_CLIP_FLAG)) {
				_VI = VU0.VI[REG_CLIP_FLAG];
				vireg = REG_CLIP_FLAG;
			}
		}

		_vu0ExecUpper(VU, ptr, pair);

		if (discard == 0) {
			if (vfreg) {
				_VFc = VU->VF[vfreg];
				VU->VF[vfreg] = _VF;
			}
			if (vireg) {
				_VIc = VU->VI[vireg];
				VU->VI[vireg] = _VI;
			}

			_vu0ExecLower(VU, ptr, pair);

			if (vfreg) {
				VU->VF[vfreg] = _VFc;
			}
			if (vireg) {
				VU->VI[vireg] = _VIc;
			}
		}
	}
	_vuAddUpperStalls(VU, &uregs);

	if (!(ptr[1] & 0x80000000))
		_vuAddLowerStalls(VU, &lregs);

	_vuTestPipes(VU);

	if(VU->VIBackupCycles > 0)
		VU->VIBackupCycles--;

	if (VU->branch > 0) {
		if (VU->branch-- == 1) {
			VU->VI[REG_TPC].UL = VU->branchpc;

			if(VU->takedelaybranch)
			{
				VU->branch = 2;
				VU->branchpc = VU->delaybranchpc;
				VU->delaybranchpc = 0;
				VU->takedelaybranch = false;
			}
		}
	}

	if( VU->ebit > 0 ) {
		if( VU->ebit-- == 1 ) {
			VU->VIBackupCycles = 0;
			_vuFlushAll(VU);
			vpu_stat_clear(0x1); /* E flag */
			vu_port_vif_vew_end(0);
		}
	}
}

void vu0Exec(VURegs* VU)
{
	VU0.VI[REG_TPC].UL &= VU0_PROGMASK;
	_vu0Exec(VU);
	VU->cycle++;
}

static void _vu1ExecUpper(VURegs* VU, u32 *ptr, const VUCompiledPair* pair) {
	VU->code = ptr[1];
	if (pair) pair->upper[1](); else VU1_UPPER_OPCODE[VU->code & 0x3f]();
}

static void _vu1ExecLower(VURegs* VU, u32 *ptr, const VUCompiledPair* pair) {
	VU->code = ptr[0];
	if (pair) pair->lower[1](); else VU1_LOWER_OPCODE[VU->code >> 25]();
}

int vu1branch = 0;
static void _vu1Exec(VURegs* VU, const VUCompiledPair* pair = nullptr)
{
	_VURegsNum lregs;
	_VURegsNum uregs;
	VECTOR _VF;
	VECTOR _VFc;
	REG_VI _VI;
	REG_VI _VIc;
	u32 *ptr;
	int vfreg;
	int vireg;
	int discard=0;

	ptr = pair ? const_cast<u32*>(&pair->lower_word) : (u32*)&VU->Micro[VU->VI[REG_TPC].UL];
	VU->VI[REG_TPC].UL+=8;

	if (ptr[1] & 0x40000000) { /* E flag */
		VU->ebit = 2;
	}
	if (ptr[1] & 0x10000000) { /* D flag */
		if (VU0.VI[REG_FBRST].UL & 0x400) {
			vpu_stat_set(0x200);
			vu_port_intc_irq(1);
			VU->ebit = 1;
		}
	}
	if (ptr[1] & 0x08000000) { /* T flag */
		if (VU0.VI[REG_FBRST].UL & 0x800) {
			vpu_stat_set(0x400);
			vu_port_intc_irq(1);
			VU->ebit = 1;
		}
	}

	VU->code = ptr[1];
	if (pair) uregs = pair->upper_regs[1]; else VU1regs_UPPER_OPCODE[VU->code & 0x3f](&uregs);
#ifndef INT_VUSTALLHACK
	_vuTestUpperStalls(VU, &uregs);
#endif

	/* check upper flags */
	if (ptr[1] & 0x80000000) { /* I flag */
		_vu1ExecUpper(VU, ptr, pair);

		VU->VI[REG_I].UL = ptr[0];
		// Lower not used, set to 0 to fill in the FMAC stall gap.
		memset(&lregs, 0, sizeof(lregs));
	} else {
		VU->code = ptr[0];
		if (pair) lregs = pair->lower_regs[1]; else VU1regs_LOWER_OPCODE[VU->code >> 25](&lregs);
#ifndef INT_VUSTALLHACK
		_vuTestLowerStalls(VU, &lregs);
#endif

		vu1branch = lregs.pipe == VUPIPE_BRANCH;

		vfreg = 0; vireg = 0;
		if (uregs.VFwrite) {
			if (lregs.VFwrite == uregs.VFwrite) {
				discard = 1;
			}
			if (lregs.VFread0 == uregs.VFwrite ||
				lregs.VFread1 == uregs.VFwrite) {
				_VF = VU->VF[uregs.VFwrite];
				vfreg = uregs.VFwrite;
			}
		}
		if (uregs.VIread & (1 << REG_CLIP_FLAG)) {
			if (lregs.VIwrite & (1 << REG_CLIP_FLAG)) {
				discard = 1;
			}
			if (lregs.VIread & (1 << REG_CLIP_FLAG)) {
				_VI = VU->VI[REG_CLIP_FLAG];
				vireg = REG_CLIP_FLAG;
			}
		}

		_vu1ExecUpper(VU, ptr, pair);

		if (discard == 0) {
			if (vfreg) {
				_VFc = VU->VF[vfreg];
				VU->VF[vfreg] = _VF;
			}
			if (vireg) {
				_VIc = VU->VI[vireg];
				VU->VI[vireg] = _VI;
			}

			_vu1ExecLower(VU, ptr, pair);

			if (vfreg) {
				VU->VF[vfreg] = _VFc;
			}
			if (vireg) {
				VU->VI[vireg] = _VIc;
			}
		}
	}
	_vuAddUpperStalls(VU, &uregs);
	_vuAddLowerStalls(VU, &lregs);

	_vuTestPipes(VU);

	if(VU->VIBackupCycles > 0)
		VU->VIBackupCycles--;

	if (VU->branch > 0) {
		if (VU->branch-- == 1) {
			VU->VI[REG_TPC].UL = VU->branchpc;

			if(VU->takedelaybranch)
			{
				VU->branch = 2;
				VU->branchpc = VU->delaybranchpc;
				VU->delaybranchpc = 0;
				VU->takedelaybranch = false;
			}
		}
	}

	if( VU->ebit > 0 ) {
		if( VU->ebit-- == 1 ) {
			VU->VIBackupCycles = 0;
			_vuFlushAll(VU);
			vpu_stat_clear(0x100);
			vu_port_vif_vew_end(1);
		}
	}
}

void vu1Exec(VURegs* VU)
{
	_vu1Exec(VU);
	VU->cycle++;
}

u32 vu_port_run_micro(unsigned idx, u32 start_pc_bytes, u32 max_pairs)
{
	if (idx > 1) return 0;
	VURegs* VU = &vuRegs[idx];
	const u32 running_bit = idx ? 0x100 : 0x1;
	VU->VI[REG_TPC].UL = start_pc_bytes;
	u32 pairs = 0;
	while (pairs < max_pairs && (vpu_stat() & running_bit)) {
		if (idx) {
			VU->VI[REG_TPC].UL &= VU1_PROGMASK;
			vu1Exec(VU);
		} else {
			vu0Exec(VU);
		}
		++pairs;
	}
	return pairs;
}

// Diagnostics (PS2_LOG=vu): VU1 pairs retired per execution tier.
u64 g_vu1_tier_pairs[4];   // graph, fast, compiled single-step, interpreter fallback
u64 g_vu1_graph_exits[2048];   // pair PC where a graph run returned
u64 g_vu0_tier_pairs[2];   // whole-image graph, compiled single-step
u32 vu_port_run_compiled(unsigned idx, u32 start_pc_bytes, u32 max_pairs)
{
	if (idx > 1) return 0;
	VURegs* VU = &vuRegs[idx];
	const u32 running_bit = idx ? 0x100 : 0x1;
	VU->VI[REG_TPC].UL = start_pc_bytes;
	u32 pairs = 0;
	VUFastPipes fast_pipes{};
	if (idx) fast_pipes.mapped = vu_compiled_map(idx);
	while (pairs < max_pairs && (vpu_stat() & running_bit)) {
		VU->VI[REG_TPC].UL &= idx ? VU1_PROGMASK : VU0_PROGMASK;
		const VUCompiledPair* pair = vu_compiled_at(idx, VU->VI[REG_TPC].UL);
		if (!pair) {
			// An unmapped branch target or overwritten pair resumes the exact
			// interpreter pipeline state; never resume compiled code this run.
			const u32 n = vu_port_run_micro(idx, VU->VI[REG_TPC].UL, max_pairs - pairs);
			if (idx) g_vu1_tier_pairs[3] += n;
			pairs += n;
			break;
		}
		if (idx) {
#ifdef PS2_VU_FAST
			const u32 image_pairs = vu_port_run_image(max_pairs - pairs);
			if (image_pairs) {
				fast_pipes.valid = false;
				g_vu1_tier_pairs[0] += image_pairs;
				++g_vu1_graph_exits[(VU->VI[REG_TPC].UL >> 3) & 2047];
				pairs += image_pairs;
				continue;
			}
			const u32 graph_pairs = vu_port_run_graph_pair(
				static_cast<u32>(pair - g_vu_compiled_pairs), max_pairs - pairs,
				fast_pipes);
			if (graph_pairs) {
				fast_pipes.valid = false;
				g_vu1_tier_pairs[0] += graph_pairs;
				++g_vu1_graph_exits[(VU->VI[REG_TPC].UL >> 3) & 2047];
				pairs += graph_pairs;
				continue;
			}
#endif
			const u32 fast_pairs = vu_port_run_fast_pair(
				static_cast<u32>(pair - g_vu_compiled_pairs), max_pairs - pairs,
				fast_pipes);
			if (fast_pairs) {
				g_vu1_tier_pairs[1] += fast_pairs;
				pairs += fast_pairs;
				continue;
			}
		}
#ifdef PS2_VU_FAST
		else {
			const u32 image_pairs = vu_port_run_image0(max_pairs - pairs);
			if (image_pairs) {
				g_vu0_tier_pairs[0] += image_pairs;
				pairs += image_pairs;
				continue;
			}
		}
#endif
		fast_pipes.valid = false;
		if (idx) { _vu1Exec(VU, pair); ++g_vu1_tier_pairs[2]; } else { _vu0Exec(VU, pair); ++g_vu0_tier_pairs[1]; }
		++VU->cycle;
		++pairs;
	}
	return pairs;
}
