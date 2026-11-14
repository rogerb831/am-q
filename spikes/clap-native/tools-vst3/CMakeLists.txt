# VST3 tooling for the spike: the SDK's validator and a minimal VST3 test host.
# Separate project so the SDK's targets do not clash with clap-wrapper's copy.
cmake_minimum_required(VERSION 3.24)
project(AmQSpikeVst3Tools LANGUAGES C CXX)

set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../deps/vst3sdk ${CMAKE_BINARY_DIR}/vst3sdk)

add_executable(vst3_host_test
    vst3_host_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../deps/vst3sdk/public.sdk/source/vst/hosting/plugprovider.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../deps/vst3sdk/public.sdk/source/vst/hosting/module_win32.cpp)
target_link_libraries(vst3_host_test PRIVATE sdk_hosting)
target_compile_definitions(vst3_host_test PRIVATE NOMINMAX)
