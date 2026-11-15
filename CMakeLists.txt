cmake_minimum_required(VERSION 3.24)

# Static MSVC runtime everywhere, to match clap-wrapper's default (must precede project()).
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(AmQ
    VERSION 0.0.1
    DESCRIPTION "Am-Q parametric EQ"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AMQ_BUILD_TESTS "Build unit tests" ON)
option(AMQ_BUILD_TOOLS "Build developer tools (accuracy harness)" ON)
option(AMQ_BUILD_PLUGIN "Build the CLAP and VST3 plugin and its editor (fetches CLAP, clap-helpers, clap-wrapper, VST3 SDK, Visage)" ON)
# OFF (default, shipping): clap-helpers host checks off. ON: Terminate + Maximal, so any host
# contract violation aborts the process. Development only; never ship ON (docs/plan/decisions.md D1).
option(AMQ_STRICT_HOST_CHECKS "Abort on CLAP host contract violations (development builds only)" OFF)

if(MSVC)
    add_compile_definitions(NOMINMAX)  # windows.h min/max macros break the VST3 SDK headers
    # PDBs for Release so host crash dumps can be symbolised; keep the optimised linker defaults.
    add_compile_options($<$<CONFIG:Release>:/Zi>)
    add_link_options($<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
endif()

# Warnings for our own targets only.
function(amq_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_subdirectory(core/dsp)
add_subdirectory(core/params)
add_subdirectory(core/analysis)
add_subdirectory(core/engine)

if(AMQ_BUILD_PLUGIN)
    include(cmake/AmqPluginDeps.cmake)
endif()
add_subdirectory(ui)  # the editor model always; the Visage editor with AMQ_BUILD_PLUGIN
if(AMQ_BUILD_PLUGIN)
    add_subdirectory(plugin)
endif()

if(AMQ_BUILD_TOOLS)
    add_subdirectory(tools/accuracy_harness)
endif()

if(AMQ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
