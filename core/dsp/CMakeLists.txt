# core/dsp: framework-free DSP library. No JUCE, no CLAP, no third-party code.
add_library(amq_dsp STATIC
    src/analog_prototype.cpp
    src/digital_design.cpp
    src/joint_fit.cpp
    src/realtime_design.cpp
)
add_library(amq::dsp ALIAS amq_dsp)

target_include_directories(amq_dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(amq_dsp PUBLIC cxx_std_20)
amq_set_warnings(amq_dsp)
