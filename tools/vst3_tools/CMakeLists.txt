# VST3-side test tools, built from the same pinned VST3 SDK as the plugin:
#   validator            the Steinberg VST3 validator;
#   amq_vst3_host_test   our VST3 test host (tests/plugin/vst3_host_test.cpp, Windows only).
#
# This is a separate project: adding the SDK's own CMake targets to the main build would
# clash with clap-wrapper's copy of the SDK. Build it, then run both on Am-Q.vst3:
#
#   cmake -S tools/vst3_tools -B build-vst3-tools -DFETCHCONTENT_SOURCE_DIR_AMQ_VST3SDK=<sdk>
#   cmake --build build-vst3-tools --config Release
#
# Without FETCHCONTENT_SOURCE_DIR_AMQ_VST3SDK the SDK is fetched at the pin below, which must
# match cmake/AmqPluginDeps.cmake. CI points it at the main build's _deps/amq_vst3sdk-src.
cmake_minimum_required(VERSION 3.24)

project(AmQVst3Tools LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)  # the SDK's hosting sources do not build as C++20
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

if(WIN32)
    set(sdk ${amq_vst3sdk_SOURCE_DIR})
    add_executable(amq_vst3_host_test
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/plugin/vst3_host_test.cpp
        ${sdk}/public.sdk/source/vst/hosting/plugprovider.cpp
        ${sdk}/public.sdk/source/vst/hosting/module_win32.cpp
        ${sdk}/public.sdk/source/common/memorystream.cpp)
    target_link_libraries(amq_vst3_host_test PRIVATE sdk_hosting)
    target_compile_definitions(amq_vst3_host_test PRIVATE NOMINMAX)
    if(MSVC)  # our warnings for our file only, not for the SDK sources compiled in
        set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/../../tests/plugin/vst3_host_test.cpp
            PROPERTIES COMPILE_OPTIONS "/W4;/permissive-;/utf-8")
    endif()
endif()
