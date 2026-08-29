cmake_minimum_required(VERSION 3.22)

project(HardCap VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build architectures")
endif()

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
    COPY_PLUGIN_AFTER_BUILD     FALSE)

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
