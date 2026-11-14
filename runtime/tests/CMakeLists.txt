add_executable(ee_ops_tests ee_ops_tests.cpp)
target_include_directories(ee_ops_tests PRIVATE ${PS2_INCLUDE})
target_compile_options(ee_ops_tests PRIVATE -O2 -Wall -Wextra)
add_test(NAME ee_ops_tests COMMAND ee_ops_tests)

find_package(PkgConfig REQUIRED)
pkg_check_modules(IPU_FFMPEG IMPORTED_TARGET libavcodec libavformat libavutil libswscale)
add_executable(ipu_tests ipu_tests.cpp
  ${CMAKE_SOURCE_DIR}/runtime/src/core/mem.cpp
  ${CMAKE_SOURCE_DIR}/runtime/src/core/log.cpp
  ${CMAKE_SOURCE_DIR}/runtime/src/hw/ipu.cpp
  ${CMAKE_SOURCE_DIR}/runtime/src/hw/ipu/Mpeg.cpp
  ${CMAKE_SOURCE_DIR}/runtime/src/hw/ipu/Idct.cpp
  ${CMAKE_SOURCE_DIR}/runtime/src/hw/ipu/yuv2rgb.cpp)
target_include_directories(ipu_tests PRIVATE ${PS2_INCLUDE} ${CMAKE_SOURCE_DIR}/runtime/src)
target_compile_options(ipu_tests PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
if(IPU_FFMPEG_FOUND)
  target_link_libraries(ipu_tests PRIVATE PkgConfig::IPU_FFMPEG)
  target_compile_definitions(ipu_tests PRIVATE
    IPU_SAMPLE_MOVIE="${CMAKE_SOURCE_DIR}/iso/extract/FMV/PMH_LOGO.PSS"
    IPU_OUTPUT_DIR="${CMAKE_BINARY_DIR}")
  target_compile_definitions(ipu_tests PRIVATE IPU_FFMPEG=1)
endif()
add_test(NAME ipu_tests COMMAND ipu_tests)
