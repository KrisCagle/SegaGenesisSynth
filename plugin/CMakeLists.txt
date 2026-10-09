# The Genisys plugin, built with JUCE (fetched by the top-level CMakeLists).

set(GENISYS_PLUGIN_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND GENISYS_PLUGIN_FORMATS AU)
endif()

juce_add_plugin(Genisys
    PRODUCT_NAME "Genisys"
    COMPANY_NAME "Kris Cagle"
    BUNDLE_ID "com.kriscagle.genisys"
    DESCRIPTION "16-bit console FM synthesizer (YM2612 + SN76489)"
    VERSION ${PROJECT_VERSION}

    # These two codes identify the plugin to every DAW. Changing them after
    # a release makes hosts treat it as a different plugin, so saved
    # projects would lose it. Never change them.
    PLUGIN_MANUFACTURER_CODE Kcag
    PLUGIN_CODE Gnsy

    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE

    VST3_CATEGORIES Instrument Synth
    AU_MAIN_TYPE kAudioUnitType_MusicDevice

    # Installing into the system plugin folder needs admin rights; the
    # release packaging handles installation instead.
    COPY_PLUGIN_AFTER_BUILD FALSE

    FORMATS ${GENISYS_PLUGIN_FORMATS})

# The plugin's own code lives in an INTERFACE library so the plugin and its
# test program can both compile it. (JUCE compiles module sources into each
# target that links them, so they can't share one static library.)
add_library(GenisysSharedCode INTERFACE)
target_sources(GenisysSharedCode INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Parameters.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/PluginProcessor.cpp)
target_include_directories(GenisysSharedCode INTERFACE Source)
target_compile_definitions(GenisysSharedCode INTERFACE
    JUCE_WEB_BROWSER=0          # no embedded browser needed
    JUCE_USE_CURL=0             # no network access needed
    JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(GenisysSharedCode INTERFACE
    genisys_engine
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

target_link_libraries(Genisys PRIVATE GenisysSharedCode)

if(GENISYS_BUILD_TESTS)
    juce_add_console_app(plugin_tests PRODUCT_NAME "Genisys Plugin Tests")
    target_sources(plugin_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/plugin_tests.cpp)
    target_link_libraries(plugin_tests PRIVATE GenisysSharedCode)
    add_test(NAME plugin_tests COMMAND plugin_tests)
endif()
