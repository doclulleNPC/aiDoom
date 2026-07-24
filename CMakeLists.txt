# CMakeLists.txt -- unified cross-platform build for BuddyDoom (SDL3).
#
# Builds these targets and stages them into run/ :
#   buddydoom       - the game (console subsystem; keeps stdout for AI director logs)
#   buddydoom_config - SDL3 settings editor (GUI)
#   gpumon_sdl      - SDL3 GPU monitor (GUI)
#   launcher        - SDL3 IWAD / mode picker (GUI)
#   extractor       - SDL3 Heretic/Hexen/FreeDoom2 asset extractor (GUI)
#
# SDL3 is found via find_package; by default it looks for a sibling ../SDL3 SDK
# (next to this repo) and then system locations.  Override with CMAKE_PREFIX_PATH.
# Configure + build:
#   cmake -B build           # Windows: uses ../SDL3 SDK (run in a VS env)
#   cmake --build build      # Linux/macOS: uses system SDL3
#
cmake_minimum_required(VERSION 3.16)
project(BuddyDoom C)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# SDL3: prefer a system install, else a sibling ../SDL3 SDK (relative to the
# repo root, i.e. next to this checkout).  Override with -DCMAKE_PREFIX_PATH=...
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/../SDL3")
find_package(SDL3 REQUIRED CONFIG)

set(RUNDIR ${CMAKE_SOURCE_DIR}/run)

# ---------------------------------------------------------------------------
# Helper: per-toolchain quirks shared by every target.
#   - the 1996 K&R-ish engine needs -fno-strict-aliasing (or -O2 miscompiles the
#     type-punning) and a few -Wno-* on modern gcc/clang
#   - MSVC just needs the CRT deprecation defines
# ---------------------------------------------------------------------------
function(buddydoom_common tgt)
    target_compile_definitions(${tgt} PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(${tgt} PRIVATE SDL3::SDL3)
    if(MSVC)
        target_compile_definitions(${tgt} PRIVATE
            _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE
            _WINSOCK_DEPRECATED_NO_WARNINGS)
    else()
        target_compile_options(${tgt} PRIVATE
            -fno-strict-aliasing -fcommon
            -Wno-implicit-int -Wno-implicit-function-declaration
            -Wno-int-conversion)
    endif()
    # Stage the built binary next to the game in run/.
    add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${RUNDIR}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${tgt}> ${RUNDIR}/)
endfunction()

# ---------------------------------------------------------------------------
# buddydoom -- the game
# ---------------------------------------------------------------------------
file(GLOB BUDDYDOOM_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/files/*.c)
add_executable(buddydoom ${BUDDYDOOM_SRC})
target_include_directories(buddydoom PRIVATE ${CMAKE_SOURCE_DIR}/files)
buddydoom_common(buddydoom)
if(WIN32)
    target_sources(buddydoom PRIVATE ${CMAKE_SOURCE_DIR}/files/buddydoom.rc)
    target_link_libraries(buddydoom PRIVATE ws2_32)           # i_net.c / p_ai_llm.c
    # copy the SDL3 runtime DLL next to the exe + into run/
    add_custom_command(TARGET buddydoom POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:buddydoom>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL3::SDL3> ${RUNDIR}/)
endif()

# -------------------------------------------------------------------
# tools -- buddydoom_config + gpumon_sdl + launcher (SDL3 GUI apps that own main())
# -------------------------------------------------------------------
foreach(tool buddydoom_config gpumon_sdl launcher extractor director)
    add_executable(${tool} ${CMAKE_SOURCE_DIR}/tools/${tool}.c)
    target_include_directories(${tool} PRIVATE ${CMAKE_SOURCE_DIR}/tools)
    buddydoom_common(${tool})
    set_target_properties(${tool} PROPERTIES WIN32_EXECUTABLE TRUE)  # GUI, no console
    if(MSVC)
        # WIN32_EXECUTABLE -> WINDOWS subsystem; our entry is main(), so override.
        target_link_options(${tool} PRIVATE /ENTRY:mainCRTStartup)
    endif()
endforeach()

if(WIN32)
    target_link_libraries(director PRIVATE ws2_32 psapi)
endif()

# The GPU monitor ships as plain "gpumon" (source file is gpumon_sdl.c).
set_target_properties(gpumon_sdl PROPERTIES OUTPUT_NAME gpumon)

# Launcher ships as plain "launcher" (source file is launcher.c).
set_target_properties(launcher PROPERTIES OUTPUT_NAME launcher)

# tools: embed the shared buddydoom.ico as the .exe icon on Windows.
if(WIN32)
    target_sources(gpumon_sdl    PRIVATE ${CMAKE_SOURCE_DIR}/tools/gpumon.rc)
    target_sources(buddydoom_config PRIVATE ${CMAKE_SOURCE_DIR}/tools/buddydoom_config.rc)
    target_sources(launcher     PRIVATE ${CMAKE_SOURCE_DIR}/tools/launcher.rc)
    target_sources(extractor    PRIVATE ${CMAKE_SOURCE_DIR}/tools/extractor.rc)
endif()

if(APPLE)
    # On macOS, if MACOSX_BUNDLE is enabled, set the icon file.
    set_source_files_properties(${CMAKE_SOURCE_DIR}/buddydoom.icns PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    target_sources(buddydoom PRIVATE ${CMAKE_SOURCE_DIR}/buddydoom.icns)
    set_target_properties(buddydoom PROPERTIES MACOSX_BUNDLE_ICON_FILE buddydoom.icns)
    foreach(tool buddydoom_config gpumon_sdl launcher extractor director)
        target_sources(${tool} PRIVATE ${CMAKE_SOURCE_DIR}/buddydoom.icns)
        set_target_properties(${tool} PROPERTIES MACOSX_BUNDLE_ICON_FILE buddydoom.icns)
    endforeach()
endif()
