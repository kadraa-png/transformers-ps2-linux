# Core runtime (memory, dispatch, EE kernel HLE, EE hardware).
find_package(ZLIB REQUIRED)

set(PS2_SRC ${CMAKE_SOURCE_DIR}/runtime/src)
add_library(ps2_core STATIC
  log.cpp mem.cpp iso.cpp dispatch.cpp crash.cpp profile.cpp
  ${PS2_SRC}/kernel/fiber.cpp ${PS2_SRC}/kernel/kernel.cpp
  ${PS2_SRC}/hw/hwregs.cpp ${PS2_SRC}/hw/ipu.cpp
  ${PS2_SRC}/hw/ipu/Mpeg.cpp ${PS2_SRC}/hw/ipu/Idct.cpp
  ${PS2_SRC}/hw/ipu/yuv2rgb.cpp)
target_include_directories(ps2_core PUBLIC $<BUILD_INTERFACE:${PS2_INCLUDE}> $<BUILD_INTERFACE:${PS2_SRC}>)
target_compile_options(ps2_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
