# Game-only build: recompiles the player's disc and links it against the
# prebuilt runtime.  The launcher runs this on the player's machine:
#   cmake -S <runtime>/src/packaging/game -B <work> -G Ninja \
#         -DPS2_ISO=<iso> -DPS2_RUNTIME_DIR=<prefix>/share/transformers-port
cmake_minimum_required(VERSION 3.20)
project(transformers_game LANGUAGES C CXX ASM)

# The AppImage's gcc reports its system headers under the mount path, which
# changes on every launch; -MMD keeps them out of the dependency files so a
# resumed build doesn't recompile everything.
set(CMAKE_DEPFILE_FLAGS_C "-MMD -MT <DEP_TARGET> -MF <DEP_FILE>")
set(CMAKE_DEPFILE_FLAGS_CXX "-MMD -MT <DEP_TARGET> -MF <DEP_FILE>")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(NOT PS2_RUNTIME_DIR OR NOT PS2_ISO)
  message(FATAL_ERROR "pass -DPS2_RUNTIME_DIR=<prefix>/share/transformers-port and -DPS2_ISO=<disc image>")
endif()

set(PS2_SOURCE_ROOT ${PS2_RUNTIME_DIR}/src)
set(PS2_INCLUDE ${PS2_SOURCE_ROOT}/runtime/include)
set(PS2_RUNTIME_SRC ${PS2_SOURCE_ROOT}/runtime/src)
set(PS2_VU_INCLUDES ${PS2_INCLUDE} ${PS2_RUNTIME_SRC}/vu
    ${PS2_SOURCE_ROOT}/third_party/pcsx2_vu ${PS2_SOURCE_ROOT}/third_party/pcsx2_vif)
include(${PS2_SOURCE_ROOT}/cmake/ps2_flags.cmake)
find_package(ps2runtime CONFIG REQUIRED PATHS ${PS2_RUNTIME_DIR}/runtime/cmake NO_DEFAULT_PATH)
include(${PS2_SOURCE_ROOT}/cmake/ps2_generated.cmake)

add_executable(transformers ${PS2_RUNTIME_SRC}/core/main.cpp)
target_link_libraries(transformers PRIVATE
  "$<LINK_GROUP:RESCAN,ps2::ps2_core,ee_generated,ps2::ps2_vu,ps2_vu_generated,ps2::ps2_gs,ps2::ps2_iop,ps2_iop_generated,ps2::ps2_spu2,ps2::ps2_platform>"
  ZLIB::ZLIB)
set_target_properties(transformers PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_options(transformers PRIVATE -rdynamic   # readable crash backtraces
  # The player's libstdc++ may be older than the bundled compiler's.
  -static-libstdc++ -static-libgcc
  # GL and GLib are linked against stub copies; their own dependencies are
  # resolved on the player's system at run time.
  -Wl,--allow-shlib-undefined)
