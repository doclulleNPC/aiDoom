# CMakeLists.txt -- unified cross-platform build for aiDoom (SDL3).
#
# Builds three targets and stages them into run/ :
#   aidoom          - the game (console subsystem; keeps stdout for AI director logs)
#   aidoom_config   - SDL3 settings editor (GUI)
#   gpumon_sdl      - SDL3 GPU monitor (GUI)
#
# SDL3 is found via find_package; by default it looks for a sibling ../SDL3 SDK
# (next to this repo) and then system locations.  Override with CMAKE_PREFIX_PATH.
# Configure + build:
#   cmake -B build           # Windows: uses ../SDL3 SDK (run in a VS env)
#   cmake --build build      # Linux/macOS: uses system SDL3
#
cmake_minimum_required(VERSION 3.16)
project(aiDoom C)

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
function(aidoom_common tgt)
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
# aidoom -- the game
# ---------------------------------------------------------------------------
file(GLOB AIDOOM_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/files/*.c)
add_executable(aidoom ${AIDOOM_SRC})
target_include_directories(aidoom PRIVATE ${CMAKE_SOURCE_DIR}/files)
aidoom_common(aidoom)
if(WIN32)
    target_sources(aidoom PRIVATE ${CMAKE_SOURCE_DIR}/files/aidoom.rc)
    target_link_libraries(aidoom PRIVATE ws2_32)           # i_net.c / p_ai_llm.c
    # copy the SDL3 runtime DLL next to the exe + into run/
    add_custom_command(TARGET aidoom POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:aidoom>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL3::SDL3> ${RUNDIR}/)
endif()

# ---------------------------------------------------------------------------
# tools -- aidoom_config + gpumon_sdl (SDL3 GUI apps that own main())
# ---------------------------------------------------------------------------
foreach(tool aidoom_config gpumon_sdl aicoop_config)
    add_executable(${tool} ${CMAKE_SOURCE_DIR}/tools/${tool}.c)
    target_include_directories(${tool} PRIVATE ${CMAKE_SOURCE_DIR}/tools)
    aidoom_common(${tool})
    set_target_properties(${tool} PROPERTIES WIN32_EXECUTABLE TRUE)  # GUI, no console
    if(MSVC)
        # WIN32_EXECUTABLE -> WINDOWS subsystem; our entry is main(), so override.
        target_link_options(${tool} PRIVATE /ENTRY:mainCRTStartup)
    endif()
endforeach()

# The GPU monitor ships as plain "gpumon" (source file is gpumon_sdl.c).
set_target_properties(gpumon_sdl PROPERTIES OUTPUT_NAME gpumon)

# tools: embed the shared aidoom.ico as the .exe icon on Windows.
if(WIN32)
    target_sources(gpumon_sdl    PRIVATE ${CMAKE_SOURCE_DIR}/tools/gpumon.rc)
    target_sources(aidoom_config PRIVATE ${CMAKE_SOURCE_DIR}/tools/aidoom_config.rc)
    target_sources(aicoop_config PRIVATE ${CMAKE_SOURCE_DIR}/tools/aicoop_config.rc)
endif()
