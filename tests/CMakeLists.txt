include(FetchContent)

# doctest (MIT). Recorded in THIRD_PARTY.md. Test-only; never linked into product code.
FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
    GIT_SHALLOW    TRUE)
set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(doctest)

add_executable(amq_dsp_tests
    test_main.cpp
    test_conventions.cpp
    test_db_smoother.cpp
    test_fft.cpp
    test_natural_phase_kernel.cpp
    test_latency.cpp
    test_analog_prototype.cpp
    test_digital_design.cpp
    test_refined_design.cpp
    test_accuracy_smoke.cpp
)
target_link_libraries(amq_dsp_tests PRIVATE amq::dsp amq::accuracy doctest::doctest)
amq_set_warnings(amq_dsp_tests)

add_test(NAME amq_dsp_tests COMMAND amq_dsp_tests)

add_executable(amq_params_tests test_main.cpp params/test_params.cpp)
target_link_libraries(amq_params_tests PRIVATE amq::params doctest::doctest)
target_compile_definitions(amq_params_tests PRIVATE
    AMQ_HOST_IDS_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/params/host_ids.golden.txt")
amq_set_warnings(amq_params_tests)
add_test(NAME amq_params_tests COMMAND amq_params_tests)

add_executable(amq_analysis_tests test_main.cpp analysis/test_analysis.cpp analysis/test_analyzer.cpp)
target_link_libraries(amq_analysis_tests PRIVATE amq::analysis amq::engine doctest::doctest)
amq_set_warnings(amq_analysis_tests)
add_test(NAME amq_analysis_tests COMMAND amq_analysis_tests)

add_executable(amq_engine_tests test_main.cpp engine/test_engine.cpp engine/test_bands.cpp engine/test_refine.cpp engine/test_midside.cpp engine/test_response.cpp engine/test_natural_phase.cpp)
target_link_libraries(amq_engine_tests PRIVATE amq::engine doctest::doctest)
amq_set_warnings(amq_engine_tests)
add_test(NAME amq_engine_tests COMMAND amq_engine_tests)

add_executable(amq_ui_model_tests test_main.cpp ui/test_ui_model.cpp)
target_link_libraries(amq_ui_model_tests PRIVATE amq::ui_model doctest::doctest)
amq_set_warnings(amq_ui_model_tests)
add_test(NAME amq_ui_model_tests COMMAND amq_ui_model_tests)

# Plugin binary checks in our own CLAP host (Windows for now). The off-main-thread case aborts
# by design under AMQ_STRICT_HOST_CHECKS, so the test runs on the shipping configuration only.
if(AMQ_BUILD_PLUGIN AND WIN32 AND NOT AMQ_STRICT_HOST_CHECKS)
    add_executable(amq_plugin_host_test plugin/plugin_host_test.cpp)
    target_link_libraries(amq_plugin_host_test PRIVATE clap)
    amq_set_warnings(amq_plugin_host_test)
    add_dependencies(amq_plugin_host_test AmQ_clap)
    add_test(NAME amq_plugin_host_test COMMAND amq_plugin_host_test $<TARGET_FILE:AmQ_clap>)

    # Editor in a real window: needs a desktop session, so it is not part of ctest by default.
    add_executable(amq_plugin_gui_test plugin/plugin_gui_test.cpp)
    target_link_libraries(amq_plugin_gui_test PRIVATE clap psapi)
    target_compile_definitions(amq_plugin_gui_test PRIVATE UNICODE _UNICODE)
    add_dependencies(amq_plugin_gui_test AmQ_clap)
endif()
