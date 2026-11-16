# The Steinberg VST3 validator, built from the same pinned VST3 SDK as the plugin.
#
# This is a separate project: adding the SDK's own CMake targets to the main build would
# clash with clap-wrapper's copy of the SDK. Build it once, then run it on Am-Q.vst3:
#
#   cmake -S tools/vst3_validator -B build-vst3-validator -DFETCHCONTENT_SOURCE_DIR_AMQ_VST3SDK=<sdk>
#   cmake --build build-vst3-validator --config Release --target validator
#
# Without FETCHCONTENT_SOURCE_DIR_AMQ_VST3SDK the SDK is fetched at the pin below, which must
# match cmake/AmqPluginDeps.cmake. CI points it at the main build's _deps/amq_vst3sdk-src.
cmake_minimum_required(VERSION 3.24)

cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(AmQVst3Validator LANGUAGES C CXX)

include(FetchContent)
FetchContent_Declare(amq_vst3sdk
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
    GIT_TAG        v3.8.0_build_66
    GIT_SHALLOW    TRUE
    GIT_SUBMODULES base pluginterfaces public.sdk cmake
    SOURCE_SUBDIR  amq-do-not-add)
FetchContent_MakeAvailable(amq_vst3sdk)

set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES ON CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
add_subdirectory(${amq_vst3sdk_SOURCE_DIR} ${CMAKE_BINARY_DIR}/vst3sdk)
