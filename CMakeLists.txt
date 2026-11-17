cmake_minimum_required(VERSION 3.22)
project(Spurwerk VERSION 1.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(JUCE_DIR "$ENV{USERPROFILE}/JUCE" CACHE PATH "Path to the JUCE source tree")
add_subdirectory(${JUCE_DIR} JUCE)

add_compile_definitions(SW_VERSION="${PROJECT_VERSION}")

set(SW_CORE
    Source/ModFile.cpp
    Source/TrackerFormats.cpp
    Source/MedFormat.cpp
    Source/AmigaFormats.cpp
    Source/YmFile.cpp
    Source/CosoFile.cpp
    Source/Engine.cpp
    Source/Export.cpp
    Source/SampleShare.cpp
    Source/DawProject.cpp
    Source/AlsExport.cpp
    Source/Convert.cpp
    Source/ModArchive.cpp
    Source/UpdateCheck.cpp)

set(SW_UI
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/PatternView.cpp
    Source/SampleView.cpp
    Source/ArchiveTab.cpp)

juce_add_plugin(Spurwerk
    VERSION ${PROJECT_VERSION}
    COMPANY_NAME "Un(d)AbtanzBAR"
    PRODUCT_NAME "Spurwerk"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    PLUGIN_MANUFACTURER_CODE Yako
    PLUGIN_CODE SpWk
    VST3_CATEGORIES Instrument Sampler
    FORMATS VST3 Standalone
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png"
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(Spurwerk PRIVATE ${SW_UI} ${SW_CORE})

set(SW_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=1 JUCE_DISPLAY_SPLASH_SCREEN=0)
target_compile_definitions(Spurwerk PUBLIC ${SW_DEFS})

if (MSVC)
    target_compile_options(Spurwerk PRIVATE /utf-8)
endif()

target_link_libraries(Spurwerk
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Engine regression test (console)
juce_add_console_app(EngineTest PRODUCT_NAME "EngineTest")
target_sources(EngineTest PRIVATE Tools/EngineTest.cpp ${SW_CORE})
target_compile_definitions(EngineTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
if (MSVC)
    target_compile_options(EngineTest PRIVATE /utf-8)
endif()
target_link_libraries(EngineTest PRIVATE juce::juce_audio_formats juce::juce_gui_basics
    PUBLIC juce::juce_recommended_config_flags)

# Renders the editor to PNG files (UI checks without a DAW)
juce_add_gui_app(UiShot PRODUCT_NAME "UiShot")
target_sources(UiShot PRIVATE Tools/UiShot.cpp ${SW_UI} ${SW_CORE} Player/PlayerProcessor.cpp Player/PlayerEditor.cpp)
target_compile_definitions(UiShot PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 SW_NO_PLUGIN_FACTORY=1
    JucePlugin_Name="Spurwerk" JucePlugin_VersionString="${PROJECT_VERSION}")
if (MSVC)
    target_compile_options(UiShot PRIVATE /utf-8)
endif()
target_link_libraries(UiShot PRIVATE juce::juce_audio_utils PUBLIC juce::juce_recommended_config_flags)

# Minimal VST3 host test (loads the built plugin like a DAW)
juce_add_console_app(HostTest PRODUCT_NAME "HostTest")
target_sources(HostTest PRIVATE Tools/HostTest.cpp)
target_compile_definitions(HostTest PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(HostTest PRIVATE juce::juce_audio_processors juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags)

# Spurwerk Player: tiny one-sample sampler (picks samples from a running Spurwerk)
juce_add_plugin(SpurwerkPlayer
    VERSION ${PROJECT_VERSION}
    COMPANY_NAME "Un(d)AbtanzBAR"
    PRODUCT_NAME "Spurwerk Player"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    PLUGIN_MANUFACTURER_CODE Yako
    PLUGIN_CODE SpPl
    VST3_CATEGORIES Instrument Sampler
    FORMATS VST3
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png"
    COPY_PLUGIN_AFTER_BUILD FALSE)
target_sources(SpurwerkPlayer PRIVATE Player/PlayerProcessor.cpp Player/PlayerEditor.cpp ${SW_CORE})
target_compile_definitions(SpurwerkPlayer PUBLIC ${SW_DEFS})
if (MSVC)
    target_compile_options(SpurwerkPlayer PRIVATE /utf-8)
endif()
target_link_libraries(SpurwerkPlayer
    PRIVATE juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)
