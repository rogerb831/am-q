# core/engine: the framework-free processor entry called by the plugin wrapper once per
# block piece (area 01 R3). Depends on core/params for the value slots and core/dsp.

add_library(amq_engine STATIC
    src/band_response.cpp
    src/design_worker.cpp
    src/engine.cpp
    src/natural_phase.cpp
    src/linear_phase.cpp
    src/spectral.cpp
    src/auto_gain.cpp
    src/eq_match_fit.cpp
    src/eq_match.cpp)
add_library(amq::engine ALIAS amq_engine)
target_include_directories(amq_engine PUBLIC include)
target_link_libraries(amq_engine PUBLIC amq::params amq::dsp amq::analysis)
target_compile_features(amq_engine PUBLIC cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(amq_engine PUBLIC Threads::Threads)
amq_set_warnings(amq_engine)
