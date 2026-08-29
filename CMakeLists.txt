cmake_minimum_required(VERSION 3.22)

# Both slices in one build. Ableton Live's plug-in scanner runs as x86-64 even
# on Apple Silicon, so an arm64-only bundle is rejected outright ("doesn't
# contain a version for the current architecture") and never reaches the browser.
# Must be set before project(): after it, the toolchain is already configured
# and CMake ignores the change.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build architectures")
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")

project(HardCap VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# On by default so local builds land straight in the DAW. CI turns it off --
# the Windows destination is under Program Files and copying there is neither
# wanted nor reliably permitted on a runner.
option(HARDCAP_COPY_AFTER_BUILD
       "Copy the built VST3 into the user plug-in folder after each build" ON)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        9.0.1
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

#===============================================================================
juce_add_plugin(HardCap
    COMPANY_NAME                "miruu"
    COMPANY_WEBSITE             "https://github.com/miruu-xyz/hardcap"
    BUNDLE_ID                   xyz.miruu.hardcap
    PLUGIN_MANUFACTURER_CODE    Miru
    PLUGIN_CODE                 Hcap
    FORMATS                     VST3 Standalone
    PRODUCT_NAME                "HardCap"
    VST3_CATEGORIES             "Fx" "Distortion" "Dynamics"
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     ${HARDCAP_COPY_AFTER_BUILD})

target_sources(HardCap PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(HardCap PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_REPORT_APP_USAGE=0)

target_link_libraries(HardCap
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

#===============================================================================
# Engine self-check. Deliberately JUCE-free so it builds and runs in seconds.
enable_testing()

add_executable(hardcap_engine_test tests/engine_test.cpp)
target_include_directories(hardcap_engine_test PRIVATE Source)
target_compile_features(hardcap_engine_test PRIVATE cxx_std_20)

add_test(NAME engine COMMAND hardcap_engine_test)

# Integration smoke test against the real AudioProcessor: bus layouts,
# oversampling, state round-trip. Links the plugin's shared-code target.
juce_add_console_app(hardcap_host_test PRODUCT_NAME "hardcap_host_test")

target_sources(hardcap_host_test PRIVATE tests/host_test.cpp)

target_link_libraries(hardcap_host_test PRIVATE
    HardCap
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# The console-app wrapper wants JUCE_STANDALONE_APPLICATION=1 while the plugin's
# shared code is built with 0. Nothing here branches on it; silence the clash.
target_compile_options(hardcap_host_test PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wno-macro-redefined>)

add_test(NAME host COMMAND hardcap_host_test)

#===============================================================================
# DSP measurement tools. Neither is a test, and both are kept out of the default
# build -- CI would otherwise link two more JUCE console apps on every runner for
# numbers nobody reads there. Build them by hand when touching the DSP:
#
#   cmake --build build --target hardcap_bench hardcap_alias
#
# bench  -- where the CPU goes, HQ on vs off, and the HQ switch's continuity.
# alias  -- what each oversampling factor and filter choice is actually buying.
juce_add_console_app(hardcap_bench PRODUCT_NAME "hardcap_bench")

target_sources(hardcap_bench PRIVATE tests/bench.cpp)

target_link_libraries(hardcap_bench PRIVATE
    HardCap
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

target_compile_options(hardcap_bench PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wno-macro-redefined>)

# Aliasing measurement, same deal -- build and run by hand.
juce_add_console_app(hardcap_alias PRODUCT_NAME "hardcap_alias")
target_sources(hardcap_alias PRIVATE tests/alias.cpp)
target_link_libraries(hardcap_alias PRIVATE
    HardCap juce::juce_audio_utils juce::juce_dsp
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
target_compile_options(hardcap_alias PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wno-macro-redefined>)

set_target_properties(hardcap_bench hardcap_alias PROPERTIES EXCLUDE_FROM_ALL TRUE)

