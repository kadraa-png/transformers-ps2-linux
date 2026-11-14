add_library(ps2_platform STATIC platform_sdl.cpp)
target_include_directories(ps2_platform PUBLIC $<BUILD_INTERFACE:${PS2_INCLUDE}>)
target_link_libraries(ps2_platform PUBLIC SDL3::SDL3-static)

add_executable(platform_test platform_test.cpp)
target_link_libraries(platform_test PRIVATE ps2_platform)
# platform_test no longer links; build it explicitly when reviving it.
set_target_properties(platform_test PROPERTIES EXCLUDE_FROM_ALL ON)
