# core/dsp: framework-free DSP library. No JUCE, no CLAP. The only third-party code is the FFT
# backend (PFFFT, BSD-style; THIRD_PARTY.md), linked privately behind amq/dsp/fft.hpp (area 01 R22).
add_library(amq_dsp STATIC
    src/analog_prototype.cpp
    src/digital_design.cpp
    src/fft_pffft.cpp
    src/joint_fit.cpp
    src/natural_phase_kernel.cpp
    src/partitioned_convolver.cpp
    src/realtime_design.cpp
)
add_library(amq::dsp ALIAS amq_dsp)

target_include_directories(amq_dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(amq_dsp PUBLIC cxx_std_20)
target_link_libraries(amq_dsp PRIVATE amq_pffft)
amq_set_warnings(amq_dsp)
