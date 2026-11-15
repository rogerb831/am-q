# core/analysis: processor-to-GUI snapshots (areas 11, 12, 14): snapshot types, the lock-free
# triple buffer, level meters and the publisher. Framework-free; no dependencies.

add_library(amq_analysis STATIC src/publisher.cpp)
add_library(amq::analysis ALIAS amq_analysis)
target_include_directories(amq_analysis PUBLIC include)
target_compile_features(amq_analysis PUBLIC cxx_std_20)
amq_set_warnings(amq_analysis)
