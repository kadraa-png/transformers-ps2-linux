add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/gsdx ${CMAKE_BINARY_DIR}/third_party/gsdx)
add_library(ps2_gs STATIC gs.cpp)
target_include_directories(ps2_gs PUBLIC $<BUILD_INTERFACE:${PS2_INCLUDE}>)
target_link_libraries(ps2_gs PUBLIC gsdx)
# ps2_core supplies the log sink gs.cpp writes to (PS2_LOG=gs).
add_executable(gs_smoke ${CMAKE_SOURCE_DIR}/runtime/tests/gs_smoke.cpp)
target_link_libraries(gs_smoke PRIVATE ps2_gs ps2_core)
add_executable(gs_stress ${CMAKE_SOURCE_DIR}/runtime/tests/gs_stress.cpp)
target_link_libraries(gs_stress PRIVATE ps2_gs ps2_core)
add_executable(gs_replay ${CMAKE_SOURCE_DIR}/runtime/tests/gs_replay.cpp)
target_link_libraries(gs_replay PRIVATE ps2_gs)
# gs_smoke and gs_stress predate the current runtime and no longer link; build
# them explicitly when reviving them.
set_target_properties(gs_smoke gs_stress PROPERTIES EXCLUDE_FROM_ALL ON)
