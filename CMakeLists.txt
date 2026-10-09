cmake_minimum_required(VERSION 3.22)

project(Genisys
    VERSION 0.1.0
    DESCRIPTION "YM2612 + SN76489 FM synthesizer"
    LANGUAGES C)

# ---- Options -------------------------------------------------------------
# The plugin (JUCE) target is added in a later phase; for now the build covers
# the emulation core, its tests, and the two existing PC front-ends.
option(GENISYS_BUILD_TESTS   "Build and register the synth-core unit tests" ON)
option(GENISYS_BUILD_DESKTOP "Build the raylib desktop app (Windows + MinGW, needs raylib)" OFF)
option(GENISYS_BUILD_PC_DEMO "Build the Windows-only miniaudio test-tone demo" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

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
    engine/src/genisys_engine.c
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

    add_executable(engine_tests tests/engine_tests.c)
    target_link_libraries(engine_tests PRIVATE genisys_engine)
    genisys_set_warnings(engine_tests)
    add_test(NAME engine_tests COMMAND engine_tests)
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
