# Outlaw Golf 2 (original Xbox), statically recompiled

cmake_minimum_required(VERSION 3.25)
project(game LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# the SDK: sdk/ here, two folders up inside the SDK's templates, a checkout beside this repository, else downloaded
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sdk/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../..)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp)
else()
    include(FetchContent)
    FetchContent_Declare(xbe_sdk GIT_REPOSITORY https://github.com/TGP482/XboxXBE-Recomp.git GIT_TAG main)
    FetchContent_MakeAvailable(xbe_sdk)
    set(SDK ${xbe_sdk_SOURCE_DIR})
endif()
include(${SDK}/cmake/XbeRecomp.cmake)

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")

# partitions as xbe_symbols wrote them into config/game.toml
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/game.toml)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml)
    file(STRINGS config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
endif()
# no config yet: build.bat writes it with xbe_symbols, the change reconfigures
if(NOT GAME_PARTITIONS)
    set(GAME_PARTITIONS 8)
endif()

# target name = GAME_EXE in build.bat, xbe = GAME_XBE
if(WIN32)
add_xbe_image(OutlawGolf2 default.xbe game.toml ${GAME_PARTITIONS} game)
else()
# ---- Linux: the SDL3 host with the Vulkan backend (sdk/runtime/gpu/vulkan.cpp). The game code is built from the
# player's xbe at first start into <exe>.so and dlopen'd (linux/game_code.cpp), so a build needs no game files; with
# them (assets/default.xbe), the build also carries the xbe's sha-1 and the first start skips the release check.
file(STRINGS config/game.toml GAME_XBE_PATH REGEX "^ *path *=")
string(REGEX MATCH "\"([^\"]+)\"" _ "${GAME_XBE_PATH}")
get_filename_component(GAME_XBE "${CMAKE_MATCH_1}" NAME)
if(NOT GAME_XBE)
    set(GAME_XBE default.xbe)
endif()

set(GEN_STUB ${CMAKE_CURRENT_BINARY_DIR}/gen_stub)
file(MAKE_DIRECTORY ${GEN_STUB})
set(XBE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/assets/${GAME_XBE})
if(EXISTS ${XBE_FILE})
    file(SHA1 ${XBE_FILE} xbe_sha1)
else()
    set(xbe_sha1 "0000000000000000000000000000000000000000")
    message(WARNING "assets/${GAME_XBE} not found: building without the game's sha-1, the player is asked at start")
endif()
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/image_sha1.cpp
     CONTENT "extern const char kImageSha1[] = \"${xbe_sha1}\";\n")
# a stub image_symbols.h from the config, so the SDK's HLE sources build without the xbe; with it, a release build
# bakes the recompiler's own file (run build.bat's codegen step on Linux once assets/ exists)
execute_process(COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux/gen_image_symbols.py
                ${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml ${XBE_SDK_DIR}/xdk ${GEN_STUB}/image_symbols.h
                RESULT_VARIABLE stub_failed OUTPUT_VARIABLE stub_output ERROR_VARIABLE stub_error
                OUTPUT_STRIP_TRAILING_WHITESPACE)
if(stub_failed)
    message(FATAL_ERROR "gen_image_symbols.py failed: ${stub_error}")
endif()
message(STATUS "${stub_output}")
file(WRITE ${GEN_STUB}/hook_exports.h "#pragma once\n")

# glslang turns the SDK's HLSL into SPIR-V (gpu/vk_shader.cpp)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL ON CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glslang GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git GIT_TAG 16.0.0 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glslang)

file(GLOB XBE_RUNTIME_SOURCES CONFIGURE_DEPENDS ${XBE_SDK_DIR}/runtime/*/*.cpp)
list(FILTER XBE_RUNTIME_SOURCES EXCLUDE REGEX
     "/runtime/app/(main|splash|handover|game_code|settings|game_files|system_info)\\.cpp$|/audio/xaudio2\\.cpp$|/gpu/(d3d12|texturedumper)\\.cpp$")
file(GLOB LINUX_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/linux/*.cpp)
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*/*.cpp)
set(IMGUI_SOURCES ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)

add_executable(OutlawGolf2 ${XBE_RUNTIME_SOURCES} ${LINUX_SOURCES} ${GAME_SOURCES} ${IMGUI_SOURCES}
               ${CMAKE_CURRENT_BINARY_DIR}/image_sha1.cpp)
add_dependencies(OutlawGolf2 compiler_files)
# -rdynamic: the game code .so resolves the host's symbols from the executable, as the Windows import library does
set_target_properties(OutlawGolf2 PROPERTIES ENABLE_EXPORTS ON)
target_compile_definitions(OutlawGolf2 PRIVATE XBE_DLOPEN_CODE XBE_RENDER_THREAD=0)
# x86 chars are signed, as the game's code and the runtime were written for (MSVC's default); the game code
# compiles -march=x86-64-v2, the runtime may use the same
target_compile_options(OutlawGolf2 PRIVATE -fno-strict-aliasing -fsigned-char -march=x86-64-v2)
target_include_directories(OutlawGolf2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/linux/compat    # <windows.h> for the game's Win32-style hooks
    ${XBE_SDK_DIR}/harness/compat               # Winsock and DXGI names the portable runtime uses
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${XBE_SDK_DIR}/runtime
    ${GEN_STUB})
target_include_directories(OutlawGolf2 SYSTEM PRIVATE ${imgui_SOURCE_DIR} ${glslang_SOURCE_DIR})
set_source_files_properties(${XBE_SDK_DIR}/runtime/gpu/vk_shader.cpp PROPERTIES COMPILE_DEFINITIONS ENABLE_HLSL)
# the quick menu's font (#embed, which searches its own paths, not the include ones)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(OutlawGolf2 PRIVATE --embed-dir=${XBE_SDK_DIR}/third_party/fonts)
endif()
find_library(XBE_VULKAN_LIB vulkan REQUIRED)
target_link_libraries(OutlawGolf2 PRIVATE tomlplusplus::tomlplusplus SDL3::SDL3-static
                      glslang glslang-default-resource-limits ${XBE_VULKAN_LIB} dl pthread)
endif()
