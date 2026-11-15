# core/engine: the framework-free processor entry called by the plugin wrapper once per
# block piece (area 01 R3). Depends on core/params for the value slots and core/dsp.

add_library(amq_engine STATIC
    src/design_worker.cpp
    src/engine.cpp)
add_library(amq::engine ALIAS amq_engine)
target_include_directories(amq_engine PUBLIC include)
target_link_libraries(amq_engine PUBLIC amq::params amq::dsp amq::analysis)
target_compile_features(amq_engine PUBLIC cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(amq_engine PUBLIC Threads::Threads)
amq_set_warnings(amq_engine)
