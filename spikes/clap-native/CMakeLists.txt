# Am-Q framework spike: CLAP-native plugin + clap-wrapper VST3 + Visage editor.
# Spike code only. Dependencies are cloned into deps/ (see README.md); nothing here is
# used by core/.
cmake_minimum_required(VERSION 3.24)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # match clap-wrapper

project(AmQSpike LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DEPS ${CMAKE_CURRENT_SOURCE_DIR}/deps)
add_compile_definitions(NOMINMAX)  # windows.h min/max macros break the VST3 SDK headers
if(MSVC)  # PDBs for Release so host crash dumps can be symbolised; codegen unchanged
    add_compile_options($<$<CONFIG:Release>:/Zi>)
    add_link_options($<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
endif()

function(amq_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    endif()
endfunction()

# Framework-free DSP core from the main repo.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/dsp ${CMAKE_BINARY_DIR}/core_dsp)

# CLAP SDK and helpers (MIT).
add_subdirectory(${DEPS}/clap ${CMAKE_BINARY_DIR}/clap EXCLUDE_FROM_ALL)
add_subdirectory(${DEPS}/clap-helpers ${CMAKE_BINARY_DIR}/clap-helpers EXCLUDE_FROM_ALL)

# Visage (MIT): GPU UI, Direct3D 11 on Windows.
set(VISAGE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(VISAGE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${DEPS}/visage ${CMAKE_BINARY_DIR}/visage EXCLUDE_FROM_ALL)

# clap-wrapper 0.17.0 (MIT) with the VST3 SDK 3.8.0 (MIT).
set(CLAP_SDK_ROOT ${DEPS}/clap CACHE PATH "" FORCE)
set(VST3_SDK_ROOT ${DEPS}/vst3sdk CACHE PATH "" FORCE)
set(CLAP_WRAPPER_OUTPUT_NAME "AmQSpike" CACHE STRING "" FORCE)
set(CLAP_WRAPPER_DONT_ADD_TARGETS TRUE CACHE BOOL "" FORCE)
set(CLAP_WRAPPER_CXX_STANDARD 20 CACHE STRING "" FORCE)
add_subdirectory(${DEPS}/clap-wrapper ${CMAKE_BINARY_DIR}/clap-wrapper EXCLUDE_FROM_ALL)

# The plugin implementation, shared by the .clap and the .vst3.
add_library(amq_spike_impl STATIC
    src/spike_plugin.cpp
    src/spike_editor.cpp
    src/entry.cpp)
# clap-first: the impl library is linked into each format's module.
target_include_directories(amq_spike_impl PUBLIC src)
target_link_libraries(amq_spike_impl PUBLIC amq::dsp clap clap-helpers visage)
target_compile_definitions(amq_spike_impl PUBLIC NOMINMAX)
# OFF (default, shipping): clap-helpers host checks off. ON: Terminate + Maximal, so any host
# contract violation aborts. Never ship ON: it kills hosts that use VST3's looser threading.
option(AMQ_STRICT_HOST_CHECKS "Abort on CLAP host contract violations (development only)" OFF)
target_compile_definitions(amq_spike_impl PUBLIC AMQ_STRICT_HOST_CHECKS=$<BOOL:${AMQ_STRICT_HOST_CHECKS}>)
amq_set_warnings(amq_spike_impl)

# AmQSpike.clap and AmQSpike.vst3 from the same static implementation (clap-first).
make_clapfirst_plugins(
    TARGET_NAME AmQSpike
    IMPL_TARGET amq_spike_impl
    OUTPUT_NAME "AmQSpike"
    ENTRY_SOURCE "src/clap_entry.cpp"
    PLUGIN_FORMATS CLAP VST3
    WINDOWS_FOLDER_VST3 TRUE
    COPY_AFTER_BUILD FALSE)

# Test host for exact-sample automation and latency notification (CLAP).
add_executable(clap_host_test tests/clap_host_test.cpp)
target_link_libraries(clap_host_test PRIVATE clap)
target_compile_definitions(clap_host_test PRIVATE NOMINMAX)

# GUI test host (criterion 4 evidence in our own host).
add_executable(clap_gui_test tests/clap_gui_test.cpp)
target_link_libraries(clap_gui_test PRIVATE clap)
target_compile_definitions(clap_gui_test PRIVATE NOMINMAX UNICODE _UNICODE)
