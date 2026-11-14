add_library(ps2_spu2 STATIC
  spu2.cpp
  "${CMAKE_SOURCE_DIR}/third_party/spu2x/Mixer.cpp"
  "${CMAKE_SOURCE_DIR}/third_party/spu2x/ADSR.cpp"
  "${CMAKE_SOURCE_DIR}/third_party/spu2x/Reverb.cpp"
  "${CMAKE_SOURCE_DIR}/third_party/spu2x/ReadInput.cpp"
  "${CMAKE_SOURCE_DIR}/third_party/spu2x/Dma.cpp"
  "${CMAKE_SOURCE_DIR}/third_party/spu2x/RegTable.cpp"
  "${CMAKE_SOURCE_DIR}/third_party/spu2x/spu2sys.cpp")
target_include_directories(ps2_spu2 PUBLIC $<BUILD_INTERFACE:${PS2_INCLUDE}>
  PRIVATE "${CMAKE_SOURCE_DIR}/third_party/spu2x")

add_executable(spu2_tests tests/spu2_tests.cpp)
target_link_libraries(spu2_tests PRIVATE ps2_spu2)
target_include_directories(spu2_tests PRIVATE ${PS2_INCLUDE})
target_compile_options(spu2_tests PRIVATE -UNDEBUG)
add_test(NAME spu2_tests COMMAND spu2_tests)
# spu2_tests no longer links (needs platform::now_ns); build it explicitly when reviving it.
set_target_properties(spu2_tests PROPERTIES EXCLUDE_FROM_ALL ON)
