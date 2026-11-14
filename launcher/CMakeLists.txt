# Setup wizard / launcher.
# launcher_core holds everything testable without a window.
file(GLOB LAUNCHER_CORE_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(FILTER LAUNCHER_CORE_SRC EXCLUDE REGEX "/(ui|main)\\.cpp$")
add_library(launcher_core STATIC ${LAUNCHER_CORE_SRC})
target_include_directories(launcher_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(launcher_core PRIVATE -Wall -Wextra)

add_executable(launcher_tests tests/launcher_tests.cpp tests/fake_iso.cpp)
target_link_libraries(launcher_tests PRIVATE launcher_core)
target_compile_options(launcher_tests PRIVATE -Wall -Wextra -UNDEBUG)
target_compile_definitions(launcher_tests PRIVATE LAUNCHER_FAKE_TOOLS="${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_tools")
find_package(Threads REQUIRED)
target_link_libraries(launcher_core PUBLIC Threads::Threads)

# CONTROLS.md is shown on the Controls screen.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/CONTROLS.md CONTROLS_MD)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/controls_md.inc "R\"CONTROLSMD(${CONTROLS_MD})CONTROLSMD\"\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/CONTROLS.md)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR})
target_link_libraries(imgui PUBLIC SDL3::SDL3-static)

find_package(PNG REQUIRED)
add_executable(transformers-launcher src/main.cpp src/ui.cpp)
target_include_directories(transformers-launcher PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(transformers-launcher PRIVATE launcher_core imgui PNG::PNG)
target_compile_options(transformers-launcher PRIVATE -Wall -Wextra)
install(TARGETS transformers-launcher RUNTIME DESTINATION bin)
