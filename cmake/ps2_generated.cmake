# Game-derived code: everything here is produced from the disc image at
# configure time and never ships.  Used by the full tree and by packaging/game.
#
# Inputs:
#   PS2_SOURCE_ROOT   tree holding tools/ and third_party/pcsx2_vu/
#   PS2_ISO           the Transformers (Europe) disc image
#   PS2_INCLUDE       runtime/include
#   PS2_RUNTIME_SRC   runtime/src (private headers the generated code includes)
#   PS2_VU_INCLUDES   include dirs of the VU code (without the generated dir)
#   PS2_VU_DEFS, PS2_VU_OPTS, PS2_VU_LINK_OPTS, PS2_EE_GEN_OPTS  (ps2_flags.cmake)
# Targets: ee_generated, ps2_iop_generated, ps2_vu_generated.
# Sets PS2_GAME_ELF (the extracted boot executable) and PS2_VU_GEN_DIR.

find_package(Python3 REQUIRED COMPONENTS Interpreter)

if(NOT EXISTS "${PS2_ISO}")
  message(FATAL_ERROR "PS2_ISO=${PS2_ISO} does not exist; pass -DPS2_ISO=/path/to/Transformers.iso")
endif()
set(PS2_TOOLS "${PS2_SOURCE_ROOT}/tools")
file(GLOB RECOMP_TOOLS ${PS2_TOOLS}/*.py ${PS2_TOOLS}/extra_entries.txt)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RECOMP_TOOLS})

# ---- recompiled EE executable --------------------------------------------------------
set(PS2_GAME_ELF ${CMAKE_BINARY_DIR}/SLES_523.88)
set(EE_GEN_DIR ${CMAKE_BINARY_DIR}/generated)
execute_process(COMMAND ${Python3_EXECUTABLE} ${PS2_TOOLS}/extract_elf.py ${PS2_ISO} ${PS2_GAME_ELF}
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "extracting the boot executable from ${PS2_ISO} failed")
endif()
execute_process(COMMAND ${Python3_EXECUTABLE} ${PS2_TOOLS}/recomp_ee.py ${PS2_GAME_ELF} ${EE_GEN_DIR}
                RESULT_VARIABLE rc OUTPUT_QUIET)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "EE recompilation failed")
endif()
file(GLOB EE_GEN_SRC ${EE_GEN_DIR}/ee_*.cpp)
add_library(ee_generated STATIC ${EE_GEN_SRC})
target_include_directories(ee_generated PRIVATE ${PS2_INCLUDE} ${EE_GEN_DIR})
target_compile_options(ee_generated PRIVATE ${PS2_EE_GEN_OPTS})

# ---- recompiled IOP modules ------------------------------------------------------------
# The game's IOP modules are read from the disc image; the generator splits
# functions into translation units, so it runs at configure time to discover
# the source list.
set(IOP_IRX_DIR "${CMAKE_BINARY_DIR}/iso_files")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${PS2_TOOLS}/extract_iso_files.py"
  "${PS2_ISO}" "${IOP_IRX_DIR}" IOP/TWOFISH.IRX IOP/LIBSD.IRX IOP/MODMIDI.IRX IOP/SIRENS3.IRX
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "extracting the IOP modules from ${PS2_ISO} failed")
endif()
set(IOP_IRX_FILES
  "${IOP_IRX_DIR}/TWOFISH.IRX"
  "${IOP_IRX_DIR}/LIBSD.IRX"
  "${IOP_IRX_DIR}/MODMIDI.IRX"
  "${IOP_IRX_DIR}/SIRENS3.IRX")
set(IOP_GEN_DIR "${CMAKE_BINARY_DIR}/generated_iop")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${PS2_TOOLS}/recomp_iop.py"
  --out "${IOP_GEN_DIR}" ${IOP_IRX_FILES}
  WORKING_DIRECTORY "${PS2_SOURCE_ROOT}"
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "IOP static recompiler failed: ${rc}")
endif()
file(GLOB IOP_GENERATED_SOURCES CONFIGURE_DEPENDS "${IOP_GEN_DIR}/*.cpp")
add_custom_command(OUTPUT "${IOP_GEN_DIR}/.generated"
  COMMAND "${Python3_EXECUTABLE}" "${PS2_TOOLS}/recomp_iop.py"
    --out "${IOP_GEN_DIR}" ${IOP_IRX_FILES}
  COMMAND "${CMAKE_COMMAND}" -E touch "${IOP_GEN_DIR}/.generated"
  DEPENDS "${PS2_TOOLS}/recomp_iop.py" "${PS2_TOOLS}/irx.py" "${PS2_TOOLS}/r3000.py" ${IOP_IRX_FILES}
  WORKING_DIRECTORY "${PS2_SOURCE_ROOT}"
  VERBATIM)
add_custom_target(iop_generated DEPENDS "${IOP_GEN_DIR}/.generated")
set_source_files_properties(${IOP_GENERATED_SOURCES} PROPERTIES GENERATED TRUE)
add_library(ps2_iop_generated STATIC ${IOP_GENERATED_SOURCES})
add_dependencies(ps2_iop_generated iop_generated)
target_include_directories(ps2_iop_generated PRIVATE "${PS2_INCLUDE}" "${PS2_RUNTIME_SRC}")

# ---- VU micro programs -----------------------------------------------------------------
set(PS2_VU_MANIFEST "${PS2_TOOLS}/vu_manifest.json"
    CACHE FILEPATH "Observed VIF MPG launch manifest (ELF offsets only)")
set(PS2_VU_IMAGES "${PS2_TOOLS}/vu_images.json"
    CACHE FILEPATH "Captured VU1 micro images (format 2, ELF offsets only)")
set(PS2_VU_GEN_DIR ${CMAKE_BINARY_DIR}/generated_vu)
set(VU_MANIFEST_ARGS)
if(PS2_VU_MANIFEST)
  list(APPEND VU_MANIFEST_ARGS --manifest ${PS2_VU_MANIFEST})
endif()
if(PS2_VU_IMAGES AND EXISTS ${PS2_VU_IMAGES})
  list(APPEND VU_MANIFEST_ARGS --images ${PS2_VU_IMAGES})
endif()
execute_process(COMMAND ${Python3_EXECUTABLE} ${PS2_TOOLS}/recomp_vu.py
    ${PS2_GAME_ELF} ${PS2_VU_GEN_DIR} ${VU_MANIFEST_ARGS}
    RESULT_VARIABLE rc OUTPUT_QUIET)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "VU recompilation failed")
endif()
foreach(input IN ITEMS ${PS2_VU_MANIFEST} ${PS2_VU_IMAGES})
  if(EXISTS ${input})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${input})
  endif()
endforeach()

# VUops.cpp and each image shard peak around 2.2 GB while compiling; the
# ps2_heavy pool keeps them from running side by side whatever -j is.
set(PS2_HEAVY_JOBS 1 CACHE STRING "VU translation units compiled at once (~2.2 GB RAM each)")
set_property(GLOBAL APPEND PROPERTY JOB_POOLS ps2_heavy=${PS2_HEAVY_JOBS})

set(VU_GEN_INCLUDES ${PS2_VU_INCLUDES} ${PS2_VU_GEN_DIR})
add_library(ps2_vu_generated STATIC
  ${PS2_VU_GEN_DIR}/vu_pairs.cpp
  ${PS2_SOURCE_ROOT}/third_party/pcsx2_vu/VUops.cpp)
target_include_directories(ps2_vu_generated PRIVATE ${VU_GEN_INCLUDES})
target_compile_definitions(ps2_vu_generated PRIVATE ${PS2_VU_DEFS})
target_compile_options(ps2_vu_generated PRIVATE ${PS2_VU_OPTS})
target_link_options(ps2_vu_generated INTERFACE ${PS2_VU_LINK_OPTS})
set_property(TARGET ps2_vu_generated PROPERTY JOB_POOL_COMPILE ps2_heavy)
if(PS2_VU_FAST)
  # The generated graphs share this unity translation unit with PCSX2's
  # opcode bodies. Keep line tables, but avoid per-variable debug tracking
  # across hundreds of thousands of generated instructions.
  set_source_files_properties(${PS2_SOURCE_ROOT}/third_party/pcsx2_vu/VUops.cpp
      TARGET_DIRECTORY ps2_vu_generated
      PROPERTIES COMPILE_OPTIONS "-g1;-fno-var-tracking-assignments")
endif()

# Whole-image VU graphs: four generated VU1 shards plus one VU0 shard (4), each its own translation
# unit (one unit holding all of them needs more than 8 GB to compile).  A shard
# compiles a private copy of VUops.cpp's opcode bodies so the graphs can inline
# them; tools/localize_shard.py then makes those strong definitions local
# (keeping the vu_img*_x entry points and weak inline/template symbols), so
# the copies never clash with ps2_vu_generated's own definitions.
foreach(k RANGE 4)
  add_library(ps2_vu_image${k} OBJECT ${PS2_VU_GEN_DIR}/vu_image_shard${k}.cpp)
  target_include_directories(ps2_vu_image${k} PRIVATE ${VU_GEN_INCLUDES})
  target_compile_definitions(ps2_vu_image${k} PRIVATE ${PS2_VU_DEFS})
  target_compile_options(ps2_vu_image${k} PRIVATE ${PS2_VU_OPTS} -g1 -fno-var-tracking-assignments)
  set_property(TARGET ps2_vu_image${k} PROPERTY JOB_POOL_COMPILE ps2_heavy)
  set(local_obj ${CMAKE_CURRENT_BINARY_DIR}/vu_image_shard${k}.local.o)
  add_custom_command(OUTPUT ${local_obj}
    COMMAND ${Python3_EXECUTABLE} ${PS2_TOOLS}/localize_shard.py
            ${CMAKE_OBJCOPY} ${CMAKE_NM} ${PS2_VU_GEN_DIR}/vu_image_shard${k}.keep
            $<TARGET_OBJECTS:ps2_vu_image${k}> ${local_obj}
    DEPENDS ps2_vu_image${k} $<TARGET_OBJECTS:ps2_vu_image${k}>
            ${PS2_VU_GEN_DIR}/vu_image_shard${k}.keep ${PS2_TOOLS}/localize_shard.py
    COMMAND_EXPAND_LISTS VERBATIM)
  target_sources(ps2_vu_generated PRIVATE ${local_obj})
endforeach()
