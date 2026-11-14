find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
find_package(PNG REQUIRED)
find_package(Freetype REQUIRED)
find_package(LibLZMA REQUIRED)
find_package(ZLIB REQUIRED)
find_package(OpenGL REQUIRED)

file(GLOB GS_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Renderers/Common/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Renderers/HW/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Renderers/Null/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Renderers/SW/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Renderers/OpenGL/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Window/GSWnd.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Window/GSSetting.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Window/GSWndSDL.cpp")
list(FILTER GS_SOURCES EXCLUDE REGEX "/(linux_replay|GSdxResources|GSVertexList|GSLinuxDialog)\\.cpp$")

file(GLOB GS_RESOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/res/glsl/*.glsl"
  "${CMAKE_CURRENT_SOURCE_DIR}/res/*.fx"
  "${CMAKE_CURRENT_SOURCE_DIR}/res/*.bmp"
  "${CMAKE_CURRENT_SOURCE_DIR}/res/*.cl")

add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/GSdxResources.c
  COMMAND glib-compile-resources ${CMAKE_CURRENT_SOURCE_DIR}/res/gsdx-res.xml
    --sourcedir=${CMAKE_CURRENT_SOURCE_DIR}/res --target=${CMAKE_CURRENT_BINARY_DIR}/GSdxResources.c
    --generate-source --c-name=GSdx_res
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/res/gsdx-res.xml ${GS_RESOURCES}
  VERBATIM)
add_library(gsdx STATIC ${GS_SOURCES} ${CMAKE_CURRENT_BINARY_DIR}/GSdxResources.c)
target_include_directories(gsdx PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}> $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/common>)
target_include_directories(gsdx PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_compile_options(gsdx PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-fno-operator-names>
  $<$<COMPILE_LANGUAGE:CXX>:-Wno-class-memaccess>
  -Wno-ignored-attributes -Wno-deprecated-declarations -Wno-attributes)
target_link_libraries(gsdx PUBLIC SDL3::SDL3 OpenGL::GL PNG::PNG Freetype::Freetype LibLZMA::LibLZMA ZLIB::ZLIB PkgConfig::GIO)
