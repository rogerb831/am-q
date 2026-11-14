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
    test_analog_prototype.cpp
    test_digital_design.cpp
    test_refined_design.cpp
    test_accuracy_smoke.cpp
)
target_link_libraries(amq_dsp_tests PRIVATE amq::dsp amq::accuracy doctest::doctest)
amq_set_warnings(amq_dsp_tests)

add_test(NAME amq_dsp_tests COMMAND amq_dsp_tests)
