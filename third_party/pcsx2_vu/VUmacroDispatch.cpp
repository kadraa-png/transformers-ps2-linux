/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010 PCSX2 Dev Team
 *
 *  This file is distributed under the terms of the GNU Lesser General Public
 *  License, version 3 or (at your option) any later version.
 *  See <https://www.gnu.org/licenses/>.
 */
#include "compat.h"

// Original COP2 macro functions in VUops.cpp.  These set VU0.code and
// synchronize the macro-visible status/MAC/Q/CLIP registers where needed.
#define DECLARE(OP) void OP()
DECLARE(VADDx); DECLARE(VADDy); DECLARE(VADDz); DECLARE(VADDw);
DECLARE(VSUBx); DECLARE(VSUBy); DECLARE(VSUBz); DECLARE(VSUBw);
DECLARE(VMADDx); DECLARE(VMADDy); DECLARE(VMADDz); DECLARE(VMADDw);
DECLARE(VMSUBx); DECLARE(VMSUBy); DECLARE(VMSUBz); DECLARE(VMSUBw);
DECLARE(VMAXx); DECLARE(VMAXy); DECLARE(VMAXz); DECLARE(VMAXw);
DECLARE(VMINIx); DECLARE(VMINIy); DECLARE(VMINIz); DECLARE(VMINIw);
DECLARE(VMULx); DECLARE(VMULy); DECLARE(VMULz); DECLARE(VMULw);
DECLARE(VMULq); DECLARE(VMAXi); DECLARE(VMULi); DECLARE(VMINIi);
DECLARE(VADDq); DECLARE(VMADDq); DECLARE(VADDi); DECLARE(VMADDi);
DECLARE(VSUBq); DECLARE(VMSUBq); DECLARE(VSUBi); DECLARE(VMSUBi);
DECLARE(VADD); DECLARE(VMADD); DECLARE(VMUL); DECLARE(VMAX);
DECLARE(VSUB); DECLARE(VMSUB); DECLARE(VOPMSUB); DECLARE(VMINI);
DECLARE(VIADD); DECLARE(VISUB); DECLARE(VIADDI); DECLARE(VIAND); DECLARE(VIOR);
DECLARE(VADDAx); DECLARE(VADDAy); DECLARE(VADDAz); DECLARE(VADDAw);
DECLARE(VSUBAx); DECLARE(VSUBAy); DECLARE(VSUBAz); DECLARE(VSUBAw);
DECLARE(VMADDAx); DECLARE(VMADDAy); DECLARE(VMADDAz); DECLARE(VMADDAw);
DECLARE(VMSUBAx); DECLARE(VMSUBAy); DECLARE(VMSUBAz); DECLARE(VMSUBAw);
DECLARE(VMULAx); DECLARE(VMULAy); DECLARE(VMULAz); DECLARE(VMULAw);
DECLARE(VITOF0); DECLARE(VITOF4); DECLARE(VITOF12); DECLARE(VITOF15);
DECLARE(VFTOI0); DECLARE(VFTOI4); DECLARE(VFTOI12); DECLARE(VFTOI15);
DECLARE(VMULAq); DECLARE(VABS); DECLARE(VMULAi); DECLARE(VCLIPw);
DECLARE(VADDAq); DECLARE(VMADDAq); DECLARE(VADDAi); DECLARE(VMADDAi);
DECLARE(VSUBAq); DECLARE(VMSUBAq); DECLARE(VSUBAi); DECLARE(VMSUBAi);
DECLARE(VADDA); DECLARE(VMADDA); DECLARE(VMULA); DECLARE(VSUBA);
DECLARE(VMSUBA); DECLARE(VOPMULA); DECLARE(VNOP);
DECLARE(VMOVE); DECLARE(VMR32); DECLARE(VLQI); DECLARE(VSQI);
DECLARE(VLQD); DECLARE(VSQD); DECLARE(VDIV); DECLARE(VSQRT);
DECLARE(VRSQRT); DECLARE(VWAITQ); DECLARE(VMTIR); DECLARE(VMFIR);
DECLARE(VILWR); DECLARE(VISWR); DECLARE(VRNEXT); DECLARE(VRGET);
DECLARE(VRINIT); DECLARE(VRXOR);
#undef DECLARE

// COP2 SPECIAL1 indexes by funct.  COP2 SPECIAL2 indexes by
// (sa << 2) | (funct & 3).  These are EE macro encodings (VU0.cpp), not
// VU micro lower encodings indexed by word >> 25 (VUops.cpp).
#define EXEC(INDEX, OP) case INDEX: OP(); return
void vu0_macro_exec(u32 word, bool lower)
{
    cpuRegs.code = word;
    const u32 funct = word & 63;
    if (funct < 0x3c) {
        if (lower) {
            switch (funct) {
                EXEC(0x30, VIADD); EXEC(0x31, VISUB); EXEC(0x32, VIADDI);
                EXEC(0x34, VIAND); EXEC(0x35, VIOR);
            }
        } else {
            switch (funct) {
                EXEC(0x00, VADDx); EXEC(0x01, VADDy); EXEC(0x02, VADDz); EXEC(0x03, VADDw);
                EXEC(0x04, VSUBx); EXEC(0x05, VSUBy); EXEC(0x06, VSUBz); EXEC(0x07, VSUBw);
                EXEC(0x08, VMADDx); EXEC(0x09, VMADDy); EXEC(0x0a, VMADDz); EXEC(0x0b, VMADDw);
                EXEC(0x0c, VMSUBx); EXEC(0x0d, VMSUBy); EXEC(0x0e, VMSUBz); EXEC(0x0f, VMSUBw);
                EXEC(0x10, VMAXx); EXEC(0x11, VMAXy); EXEC(0x12, VMAXz); EXEC(0x13, VMAXw);
                EXEC(0x14, VMINIx); EXEC(0x15, VMINIy); EXEC(0x16, VMINIz); EXEC(0x17, VMINIw);
                EXEC(0x18, VMULx); EXEC(0x19, VMULy); EXEC(0x1a, VMULz); EXEC(0x1b, VMULw);
                EXEC(0x1c, VMULq); EXEC(0x1d, VMAXi); EXEC(0x1e, VMULi); EXEC(0x1f, VMINIi);
                EXEC(0x20, VADDq); EXEC(0x21, VMADDq); EXEC(0x22, VADDi); EXEC(0x23, VMADDi);
                EXEC(0x24, VSUBq); EXEC(0x25, VMSUBq); EXEC(0x26, VSUBi); EXEC(0x27, VMSUBi);
                EXEC(0x28, VADD); EXEC(0x29, VMADD); EXEC(0x2a, VMUL); EXEC(0x2b, VMAX);
                EXEC(0x2c, VSUB); EXEC(0x2d, VMSUB); EXEC(0x2e, VOPMSUB); EXEC(0x2f, VMINI);
            }
        }
    } else {
        const u32 index = ((word >> 4) & 0x7c) | (word & 3);
        if (lower) {
            switch (index) {
                EXEC(0x30, VMOVE); EXEC(0x31, VMR32);
                EXEC(0x34, VLQI); EXEC(0x35, VSQI); EXEC(0x36, VLQD); EXEC(0x37, VSQD);
                EXEC(0x38, VDIV); EXEC(0x39, VSQRT); EXEC(0x3a, VRSQRT); EXEC(0x3b, VWAITQ);
                EXEC(0x3c, VMTIR); EXEC(0x3d, VMFIR); EXEC(0x3e, VILWR); EXEC(0x3f, VISWR);
                EXEC(0x40, VRNEXT); EXEC(0x41, VRGET); EXEC(0x42, VRINIT); EXEC(0x43, VRXOR);
            }
        } else {
            switch (index) {
                EXEC(0x00, VADDAx); EXEC(0x01, VADDAy); EXEC(0x02, VADDAz); EXEC(0x03, VADDAw);
                EXEC(0x04, VSUBAx); EXEC(0x05, VSUBAy); EXEC(0x06, VSUBAz); EXEC(0x07, VSUBAw);
                EXEC(0x08, VMADDAx); EXEC(0x09, VMADDAy); EXEC(0x0a, VMADDAz); EXEC(0x0b, VMADDAw);
                EXEC(0x0c, VMSUBAx); EXEC(0x0d, VMSUBAy); EXEC(0x0e, VMSUBAz); EXEC(0x0f, VMSUBAw);
                EXEC(0x10, VITOF0); EXEC(0x11, VITOF4); EXEC(0x12, VITOF12); EXEC(0x13, VITOF15);
                EXEC(0x14, VFTOI0); EXEC(0x15, VFTOI4); EXEC(0x16, VFTOI12); EXEC(0x17, VFTOI15);
                EXEC(0x18, VMULAx); EXEC(0x19, VMULAy); EXEC(0x1a, VMULAz); EXEC(0x1b, VMULAw);
                EXEC(0x1c, VMULAq); EXEC(0x1d, VABS); EXEC(0x1e, VMULAi); EXEC(0x1f, VCLIPw);
                EXEC(0x20, VADDAq); EXEC(0x21, VMADDAq); EXEC(0x22, VADDAi); EXEC(0x23, VMADDAi);
                EXEC(0x24, VSUBAq); EXEC(0x25, VMSUBAq); EXEC(0x26, VSUBAi); EXEC(0x27, VMSUBAi);
                EXEC(0x28, VADDA); EXEC(0x29, VMADDA); EXEC(0x2a, VMULA);
                EXEC(0x2c, VSUBA); EXEC(0x2d, VMSUBA); EXEC(0x2e, VOPMULA); EXEC(0x2f, VNOP);
            }
        }
    }
    std::abort(); // No decoded macro helper can reach an unassigned opcode.
}
#undef EXEC
