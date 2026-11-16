# core/analysis: processor-to-GUI snapshots (areas 11, 12, 14): snapshot types, the lock-free
# triple buffer, level meters and the publisher; and the analyzer engine core (areas 17, 19):
# audio taps, framer, FFT and frame fan-out, and the display spectrum consumer.
# Framework-free; depends on core/dsp (FFT, log grid).

add_library(amq_analysis STATIC
    src/analyzer.cpp
    src/audio_tap.cpp
    src/display_spectrum.cpp
    src/publisher.cpp
    src/match_capture.cpp
    src/match_curve.cpp)
add_library(amq::analysis ALIAS amq_analysis)
target_include_directories(amq_analysis PUBLIC include)
target_compile_features(amq_analysis PUBLIC cxx_std_20)
target_link_libraries(amq_analysis PUBLIC amq::dsp)
find_package(Threads REQUIRED)
target_link_libraries(amq_analysis PUBLIC Threads::Threads)
amq_set_warnings(amq_analysis)
