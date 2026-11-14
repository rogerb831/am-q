cmake_minimum_required(VERSION 3.24)

project(AmQ
    VERSION 0.0.1
    DESCRIPTION "Am-Q parametric EQ - framework-free DSP core (Phase 0 sandbox)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AMQ_BUILD_TESTS "Build unit tests" ON)
option(AMQ_BUILD_TOOLS "Build developer tools (accuracy harness)" ON)

# Warnings for our own targets only.
function(amq_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_subdirectory(core/dsp)

if(AMQ_BUILD_TOOLS)
    add_subdirectory(tools/accuracy_harness)
endif()

if(AMQ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
