add_library(ps2_iop STATIC iop.cpp kernel.cpp cdvd_rpc.cpp fileio_rpc.cpp services.cpp)
target_include_directories(ps2_iop PUBLIC $<BUILD_INTERFACE:${PS2_INCLUDE}> $<BUILD_INTERFACE:${PS2_RUNTIME_SRC}>)
target_link_libraries(ps2_iop PUBLIC ps2_spu2)

if(PS2_RUNTIME_ONLY)
  return()
endif()
# Tests below need the generated IOP modules and the disc image.
add_executable(iop_tests tests/iop_tests.cpp
  "${CMAKE_SOURCE_DIR}/runtime/src/core/mem.cpp"
  "${CMAKE_SOURCE_DIR}/runtime/src/core/iso.cpp"
  "${CMAKE_SOURCE_DIR}/runtime/src/core/log.cpp"
  "${CMAKE_SOURCE_DIR}/runtime/src/kernel/fiber.cpp")
target_link_libraries(iop_tests PRIVATE "$<LINK_GROUP:RESCAN,ps2_iop,ps2_iop_generated>")
target_include_directories(iop_tests PRIVATE "${PS2_INCLUDE}")
target_compile_definitions(iop_tests PRIVATE IOP_TEST_ISO="${PS2_ISO}")
