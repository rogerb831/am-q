# Accuracy harness: the sweep logic is a small library so tests can reuse it,
# plus a command-line driver that runs the full grid and writes a report.
add_library(amq_accuracy STATIC accuracy.cpp)
add_library(amq::accuracy ALIAS amq_accuracy)
target_include_directories(amq_accuracy PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(amq_accuracy PUBLIC amq::dsp)
amq_set_warnings(amq_accuracy)

add_executable(amq_accuracy_harness main.cpp)
target_link_libraries(amq_accuracy_harness PRIVATE amq::accuracy)
amq_set_warnings(amq_accuracy_harness)
