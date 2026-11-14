# Compile settings shared by the full tree and packaging/game (the game-only
# project the launcher builds on the player's machine).  Keep both in step by
# changing them here only.

add_compile_options(-march=x86-64-v3 -fno-strict-aliasing)

option(PS2_VU_FAST "Enable statically pipelined VU1 microprogram graphs" ON)

# Options for the VU code, applied to the prebuilt ps2_vu and the generated
# ps2_vu_generated alike.
set(PS2_VU_DEFS)
set(PS2_VU_OPTS)
if(PS2_VU_FAST)
  list(APPEND PS2_VU_DEFS PS2_VU_FAST)
  list(APPEND PS2_VU_OPTS -O3 -ffp-contract=off)
endif()

# Profile-guided optimization of the VU code (the VU1 thread is the usual
# bottleneck).  GENERATE builds an instrumented binary; run the game through
# the scenes to optimize, then reconfigure with USE.  Partial training keeps
# code the training run never reached optimized normally.
set(PS2_VU_PGO "OFF" CACHE STRING "VU profile-guided optimization: OFF, GENERATE or USE")
set(PS2_VU_PGO_DIR "${CMAKE_BINARY_DIR}/pgo-vu" CACHE PATH "VU PGO profile directory")
set(PS2_VU_LINK_OPTS)
if(PS2_VU_PGO STREQUAL "GENERATE")
  list(APPEND PS2_VU_OPTS -fprofile-generate=${PS2_VU_PGO_DIR} -fprofile-update=atomic)
  list(APPEND PS2_VU_LINK_OPTS -fprofile-generate=${PS2_VU_PGO_DIR})
elseif(PS2_VU_PGO STREQUAL "USE")
  list(APPEND PS2_VU_OPTS -fprofile-use=${PS2_VU_PGO_DIR} -fprofile-partial-training -Wno-missing-profile)
endif()

# Recompiled EE code.
set(PS2_EE_GEN_OPTS -O2 -fno-strict-aliasing -Wno-unused-label -Wno-unused-variable
    -fno-semantic-interposition)
