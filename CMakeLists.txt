cmake_minimum_required(VERSION 3.22)

project(Genisys
    VERSION 0.1.0
    DESCRIPTION "YM2612 + SN76489 FM synthesizer"
    LANGUAGES C)

# ---- Options -------------------------------------------------------------
# JUCE doesn't support MinGW, so the plugin is off by default there (that
# toolchain is only used for the raylib desktop app).
if(MINGW)
    set(GENISYS_PLUGIN_DEFAULT OFF)
else()
    set(GENISYS_PLUGIN_DEFAULT ON)
endif()
option(GENISYS_BUILD_PLUGIN  "Build the Genisys plugin (VST3/AU/Standalone) with JUCE" ${GENISYS_PLUGIN_DEFAULT})
option(GENISYS_BUILD_TESTS   "Build and register the unit tests" ON)
option(GENISYS_BUILD_TOOLS   "Build command-line tools (demo WAV renderer)" ON)
option(GENISYS_BUILD_DESKTOP "Build the raylib desktop app (Windows + MinGW, needs raylib)" OFF)
option(GENISYS_BUILD_PC_DEMO "Build the Windows-only miniaudio test-tone demo" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# The engine libraries get linked into the plugin, which is a shared library
# (.vst3/.component), so they must be position-independent on Linux/macOS.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Keep every compiler's warnings on for our own targets, so a warning that
# only GCC or only MSVC notices still shows up in CI.
function(genisys_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

# ---- synth-core: the platform-independent chip emulation ----------------
add_library(synth_core STATIC
    synth-core/src/psg.c
    synth-core/src/ym2612.c)
target_include_directories(synth_core PUBLIC synth-core/include)
genisys_set_warnings(synth_core)

# libm is a separate library on Linux/macOS toolchains; MSVC has no libm.
find_library(MATH_LIBRARY m)
if(MATH_LIBRARY)
    target_link_libraries(synth_core PUBLIC ${MATH_LIBRARY})
endif()

# ---- engine: voices, patches, resampling; shared by every front-end -----
add_library(genisys_engine STATIC
    engine/src/genisys_drums.c
    engine/src/genisys_engine.c
    engine/src/genisys_patch_io.c
    engine/src/genisys_presets.c
    engine/src/genisys_psg.c
    engine/src/resampler.c)
target_include_directories(genisys_engine PUBLIC engine/include)
target_link_libraries(genisys_engine PUBLIC synth_core)
genisys_set_warnings(genisys_engine)

# ---- Tests ---------------------------------------------------------------
if(GENISYS_BUILD_TESTS)
    enable_testing()
    foreach(test_name
            psg_register_tests
            ym2612_operator_tests
            ym2612_channel_tests
            ym2612_chip_tests)
        add_executable(${test_name} tests/${test_name}.c)
        target_link_libraries(${test_name} PRIVATE synth_core)
        genisys_set_warnings(${test_name})
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()

    foreach(test_name engine_tests patch_io_tests)
        add_executable(${test_name} tests/${test_name}.c)
        target_link_libraries(${test_name} PRIVATE genisys_engine)
        genisys_set_warnings(${test_name})
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()

# ---- Tools -------------------------------------------------------------------
if(GENISYS_BUILD_TOOLS)
    foreach(tool_name genisys_render preset_levels)
        add_executable(${tool_name} tools/${tool_name}.c)
        target_link_libraries(${tool_name} PRIVATE genisys_engine)
        genisys_set_warnings(${tool_name})
        if(MSVC)
            # fopen is standard C; MSVC's suggested fopen_s is Windows-only.
            target_compile_definitions(${tool_name} PRIVATE _CRT_SECURE_NO_WARNINGS)
        endif()
    endforeach()
endif()

# ---- raylib desktop app (to be replaced by the plugin's Standalone app) --
if(GENISYS_BUILD_DESKTOP)
    if(NOT WIN32 OR MSVC)
        message(FATAL_ERROR "The desktop app needs Windows (winmm MIDI) and GCC/MinGW (raylib from MSYS2, C11 atomics).")
    endif()
    # Uses raylib's CMake package if installed, otherwise a plain -lraylib
    # (e.g. MSYS2's mingw-w64-ucrt-x86_64-raylib).
    find_package(raylib QUIET)
    add_executable(genisys_desktop
        desktop/main.c
        desktop/midi_input.c)
    target_link_libraries(genisys_desktop PRIVATE genisys_engine raylib opengl32 gdi32 winmm)
    genisys_set_warnings(genisys_desktop)
endif()

# ---- PC test-tone demo (Windows-only: uses <windows.h> Sleep) ------------
if(GENISYS_BUILD_PC_DEMO)
    if(NOT WIN32)
        message(FATAL_ERROR "The PC demo is Windows-only.")
    endif()
    add_executable(genisys_pc_demo pc/main.c)
    target_include_directories(genisys_pc_demo PRIVATE third_party)
    target_link_libraries(genisys_pc_demo PRIVATE synth_core winmm ole32)
endif()

# ---- Plugin (JUCE) ------------------------------------------------------------
if(GENISYS_BUILD_PLUGIN)
    if(MINGW)
        message(FATAL_ERROR "JUCE plugins can't be built with MinGW. Use MSVC on Windows.")
    endif()

    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    # JUCE is downloaded at configure time, pinned to an exact release and
    # checksum so every build (local and CI) uses identical, verified code.
    include(FetchContent)
    FetchContent_Declare(JUCE
        URL https://github.com/juce-framework/JUCE/archive/refs/tags/9.0.2.tar.gz
        URL_HASH SHA256=16d01c27e8327f3644306cc5c223aa6e21af1cc1ec62faa709fc0dced57d208a
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(JUCE)

    add_subdirectory(plugin)
endif()
