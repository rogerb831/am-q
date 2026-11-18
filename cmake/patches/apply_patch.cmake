# Applies a patch to a fetched dependency once (cmake -DPATCH=<file> -P apply_patch.cmake, run in
# the dependency's source directory). Already applied: nothing to do. Neither applies cleanly: fail,
# so a dependency update that breaks the patch is caught at configure time.
find_package(Git REQUIRED)
execute_process(COMMAND ${GIT_EXECUTABLE} apply --reverse --check --ignore-whitespace ${PATCH}
                RESULT_VARIABLE already OUTPUT_QUIET ERROR_QUIET)
if(already EQUAL 0)
    message(STATUS "Patch already applied: ${PATCH}")
    return()
endif()
execute_process(COMMAND ${GIT_EXECUTABLE} apply --ignore-whitespace ${PATCH} RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Could not apply ${PATCH}")
endif()
message(STATUS "Applied patch: ${PATCH}")
