cmake_minimum_required(VERSION 3.20)
project(transformers_port LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ON: build only the prebuilt runtime archives and the launcher, which contain
# no game-derived code and need no disc image (CI, the AppImage).
option(PS2_RUNTIME_ONLY "Build only the prebuilt runtime archives and the launcher (no ISO)" OFF)

# Every runtime module sees the shared contract headers.
set(PS2_SOURCE_ROOT ${CMAKE_SOURCE_DIR})
set(PS2_INCLUDE ${CMAKE_SOURCE_DIR}/runtime/include)
set(PS2_RUNTIME_SRC ${CMAKE_SOURCE_DIR}/runtime/src)
set(PS2_VU_INCLUDES ${PS2_INCLUDE} ${PS2_RUNTIME_SRC}/vu
    ${CMAKE_SOURCE_DIR}/third_party/pcsx2_vu ${CMAKE_SOURCE_DIR}/third_party/pcsx2_vif)
include(cmake/ps2_flags.cmake)

list(APPEND CMAKE_PREFIX_PATH ${CMAKE_SOURCE_DIR}/third_party/install)
find_package(SDL3 CONFIG REQUIRED PATHS ${CMAKE_SOURCE_DIR}/third_party/install/lib64/cmake/SDL3)

# The game ISO is required for a full build: its executable and IOP modules
# are extracted and recompiled to C++ at configure time.
# Pass -DPS2_ISO=/path/to/Transformers.iso.
if(NOT PS2_RUNTIME_ONLY)
  set(PS2_ISO "${CMAKE_SOURCE_DIR}/iso/transformers.iso" CACHE FILEPATH "Transformers (Europe) disc image")
  include(cmake/ps2_generated.cmake)
endif()

# Module libraries: each directory defines a static library target named
# ps2_<module> (e.g. ps2_gs, ps2_vu, ps2_platform) plus optional tests.
foreach(mod core kernel hw vu gs iop spu2 platform)
  if(EXISTS ${CMAKE_SOURCE_DIR}/runtime/src/${mod}/CMakeLists.txt)
    add_subdirectory(runtime/src/${mod})
  endif()
endforeach()

if(EXISTS ${CMAKE_SOURCE_DIR}/runtime/tests/CMakeLists.txt)
  add_subdirectory(runtime/tests)
endif()

add_subdirectory(launcher)

# The game executable: recompiled code + runtime.
if(NOT PS2_RUNTIME_ONLY)
  find_package(ZLIB REQUIRED)
  add_executable(transformers runtime/src/core/main.cpp)
  target_link_libraries(transformers PRIVATE
    "$<LINK_GROUP:RESCAN,ps2_core,ee_generated,ps2_vu,ps2_vu_generated,ps2_gs,ps2_iop,ps2_iop_generated,ps2_spu2,ps2_platform>"
    ZLIB::ZLIB)
  set_target_properties(transformers PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  target_link_options(transformers PRIVATE -rdynamic)   # readable crash backtraces
endif()

# ---- installed runtime (consumed by packaging/game on the player's machine) ----------
# <prefix>/share/transformers-port/
#   runtime/lib, runtime/cmake, runtime/version   prebuilt archives (no game code)
#   src/                                          headers, tools and sources the
#                                                 game-only build needs, in repo layout
#   art/                                          launcher artwork
set(PS2_INSTALL_DIR share/transformers-port)
set(PS2_RT_LIBS ps2_core ps2_vu ps2_gs gsdx ps2_iop ps2_spu2 ps2_platform)
foreach(t IN LISTS PS2_RT_LIBS)
  target_include_directories(${t} INTERFACE
    $<INSTALL_INTERFACE:${PS2_INSTALL_DIR}/src/runtime/include>
    $<INSTALL_INTERFACE:${PS2_INSTALL_DIR}/src/runtime/src>)
endforeach()
install(TARGETS ${PS2_RT_LIBS} EXPORT ps2runtime ARCHIVE DESTINATION ${PS2_INSTALL_DIR}/runtime/lib)
install(EXPORT ps2runtime NAMESPACE ps2:: FILE ps2runtimeTargets.cmake
        DESTINATION ${PS2_INSTALL_DIR}/runtime/cmake)
configure_file(cmake/ps2runtimeConfig.cmake.in ${CMAKE_BINARY_DIR}/ps2runtimeConfig.cmake @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/ps2runtimeConfig.cmake DESTINATION ${PS2_INSTALL_DIR}/runtime/cmake)

if(NOT PS2_RUNTIME_VERSION)
  execute_process(COMMAND git describe --tags --always --dirty
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE PS2_RUNTIME_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT PS2_RUNTIME_VERSION)
    set(PS2_RUNTIME_VERSION unknown)
  endif()
endif()
file(WRITE ${CMAKE_BINARY_DIR}/runtime_version "${PS2_RUNTIME_VERSION}\n")
install(FILES ${CMAKE_BINARY_DIR}/runtime_version DESTINATION ${PS2_INSTALL_DIR}/runtime RENAME version)

install(DIRECTORY runtime/include DESTINATION ${PS2_INSTALL_DIR}/src/runtime)
install(DIRECTORY runtime/src DESTINATION ${PS2_INSTALL_DIR}/src/runtime
        FILES_MATCHING PATTERN "*.h" PATTERN "*.inc" PATTERN "tests" EXCLUDE)
install(FILES runtime/src/core/main.cpp DESTINATION ${PS2_INSTALL_DIR}/src/runtime/src/core)
install(DIRECTORY third_party/pcsx2_vu DESTINATION ${PS2_INSTALL_DIR}/src/third_party)
install(FILES third_party/pcsx2_vif/compat.h DESTINATION ${PS2_INSTALL_DIR}/src/third_party/pcsx2_vif)
install(DIRECTORY tools DESTINATION ${PS2_INSTALL_DIR}/src
        FILES_MATCHING PATTERN "*.py" PATTERN "*.json" PATTERN "extra_entries.txt"
        PATTERN "tests" EXCLUDE PATTERN "__pycache__" EXCLUDE)
install(FILES cmake/ps2_flags.cmake cmake/ps2_generated.cmake DESTINATION ${PS2_INSTALL_DIR}/src/cmake)
install(FILES packaging/game/CMakeLists.txt DESTINATION ${PS2_INSTALL_DIR}/src/packaging/game)
install(DIRECTORY packaging/art/ DESTINATION ${PS2_INSTALL_DIR}/art)
