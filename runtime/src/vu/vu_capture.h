#pragma once
#include "VU.h"

// Cold-path recorder: snapshots authentic launch micro images and first input
// states outside the repository when PS2_VU_CAPTURE points to a directory.
void vu_capture_written(unsigned unit);
void vu_capture_launch(unsigned unit, unsigned start_pc);
// A JR/JALR target inside the current image: an extra entry point for
// tools/vu_manifest.py (indirect.tsv, one line per distinct target and
// jumping pair).
void vu_capture_indirect(unsigned unit, unsigned target_pc, unsigned source_pc);
