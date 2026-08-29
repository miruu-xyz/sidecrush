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
# The editor's typeface and the two real icons from the Figma file. Zalando Sans
# Expanded is OFL (Resources/fonts/OFL.txt); only the roman variable face ships,
# and its default instance is the Regular the design uses.
juce_add_binary_data(HardCapAssets SOURCES
    Resources/fonts/ZalandoSansExpanded.ttf
    Resources/icons/settings.svg
    Resources/icons/close.svg)

# Binary data lands in a static lib that gets linked into a shared plugin
# module, so it has to be position-independent.
set_target_properties(HardCapAssets PROPERTIES POSITION_INDEPENDENT_CODE TRUE)

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
        HardCapAssets
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
foreach(tool bench alias)
    juce_add_console_app(hardcap_${tool} PRODUCT_NAME "hardcap_${tool}")

    target_sources(hardcap_${tool} PRIVATE tests/${tool}.cpp)

    target_link_libraries(hardcap_${tool} PRIVATE
        HardCap
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

    target_compile_options(hardcap_${tool} PRIVATE
        $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wno-macro-redefined>)

    set_target_properties(hardcap_${tool} PROPERTIES EXCLUDE_FROM_ALL TRUE)
endforeach()

#===============================================================================
# Headless editor snapshot. The only other way to look at the GUI is a DAW or the
# Standalone, and the Standalone wrapper opens the default audio input *and*
# output -- which on a laptop means it can feed back through the monitors. This
# renders the editor offscreen instead. Not a test; built on demand:
#
#   cmake --build build --target hardcap_shot
#   build/hardcap_shot_artefacts/Release/hardcap_shot out.png ceiling=-24 hover=clip
juce_add_console_app(hardcap_shot PRODUCT_NAME "hardcap_shot")

target_sources(hardcap_shot PRIVATE tools/shot.cpp)
target_include_directories(hardcap_shot PRIVATE Source)

target_link_libraries(hardcap_shot PRIVATE
    HardCap
    HardCapAssets
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

target_compile_options(hardcap_shot PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wno-macro-redefined>)

set_target_properties(hardcap_shot PROPERTIES EXCLUDE_FROM_ALL TRUE)
