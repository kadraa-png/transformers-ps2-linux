# Hand-written VU/VIF runtime.  The generated VU programs and PCSX2's opcode
# bodies (VUops.cpp, which includes them) are in ps2_vu_generated
# (cmake/ps2_generated.cmake).
add_library(ps2_vu STATIC
    vu.cpp
    vu_capture.cpp
    vu_compiled.cpp
    vu_thread.cpp
    vif.cpp
    ${PROJECT_SOURCE_DIR}/third_party/pcsx2_vu/VUinterpreter.cpp
    ${PROJECT_SOURCE_DIR}/third_party/pcsx2_vu/VUmacroDispatch.cpp
    ${PROJECT_SOURCE_DIR}/third_party/pcsx2_vif/Vif_Codes.cpp
    ${PROJECT_SOURCE_DIR}/third_party/pcsx2_vif/Vif_Transfer.cpp
    ${PROJECT_SOURCE_DIR}/third_party/pcsx2_vif/Vif_Unpack.cpp
    ${PROJECT_SOURCE_DIR}/third_party/pcsx2_vif/newVif_Unpack.cpp)
if(NOT PS2_VU_FAST)
    target_sources(ps2_vu PRIVATE ${PROJECT_SOURCE_DIR}/third_party/pcsx2_vu/VUflags.cpp)
endif()
target_include_directories(ps2_vu PUBLIC $<BUILD_INTERFACE:${PS2_INCLUDE}> PRIVATE ${PS2_VU_INCLUDES})
target_compile_definitions(ps2_vu PRIVATE ${PS2_VU_DEFS})
target_compile_options(ps2_vu PRIVATE ${PS2_VU_OPTS})
target_link_options(ps2_vu INTERFACE ${PS2_VU_LINK_OPTS})

if(PS2_RUNTIME_ONLY)
    return()
endif()
# Tests below need the generated VU programs (and the extracted game ELF).
set(VU_LIBS "$<LINK_GROUP:RESCAN,ps2_vu,ps2_vu_generated>" ps2_core ps2_platform)

add_executable(vu_tests tests/vu_tests.cpp)
target_link_libraries(vu_tests PRIVATE ${VU_LIBS})
target_compile_options(vu_tests PRIVATE -UNDEBUG)
target_include_directories(vu_tests PRIVATE ${PS2_INCLUDE})

add_executable(vu_compiled_tests tests/vu_compiled_tests.cpp)
target_link_libraries(vu_compiled_tests PRIVATE ${VU_LIBS})
target_compile_options(vu_compiled_tests PRIVATE -UNDEBUG)
target_include_directories(vu_compiled_tests PRIVATE ${PS2_VU_INCLUDES} ${PS2_VU_GEN_DIR})
target_compile_definitions(vu_compiled_tests PRIVATE
    VU_GAME_ELF="${PS2_GAME_ELF}")   # extracted from the ISO at configure time

add_executable(vu_capture_replay tests/vu_capture_replay.cpp)
target_link_libraries(vu_capture_replay PRIVATE ${VU_LIBS})
target_include_directories(vu_capture_replay PRIVATE ${PS2_VU_INCLUDES})
