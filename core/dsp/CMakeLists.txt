# core/dsp: framework-free DSP library. No JUCE, no CLAP. The only third-party code is the FFT
# backend (PFFFT, BSD-style; THIRD_PARTY.md), linked privately behind amq/dsp/fft.hpp (area 01 R22).
add_library(amq_dsp STATIC
    src/analog_prototype.cpp
    src/digital_design.cpp
    src/fft_pffft.cpp
    src/joint_fit.cpp
    src/natural_phase_kernel.cpp
    src/partitioned_convolver.cpp
    src/nonuniform_convolver.cpp
    src/realtime_design.cpp
    src/dynamics_avx2.cpp
    src/dynamics_lanes.cpp
    src/spectral_apply_avx2.cpp
)
# The AVX2 kernels (dynamics, spectral apply): AVX2 for these files only, selected at run time (cpuHasAvx2), with no
# fused multiply-adds so it gives the scalar kernel's bits.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|x86|i686" OR CMAKE_GENERATOR_PLATFORM MATCHES "x64|Win32")
    set_source_files_properties(src/dynamics_avx2.cpp src/spectral_apply_avx2.cpp PROPERTIES COMPILE_DEFINITIONS AMQ_DYN_AVX2_BUILD)
    if(MSVC)
        set_source_files_properties(src/dynamics_avx2.cpp src/spectral_apply_avx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2;/fp:precise")
    else()
        set_source_files_properties(src/dynamics_avx2.cpp src/spectral_apply_avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2;-ffp-contract=off")
    endif()
endif()
add_library(amq::dsp ALIAS amq_dsp)

target_include_directories(amq_dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(amq_dsp PUBLIC cxx_std_20)
target_link_libraries(amq_dsp PRIVATE amq_pffft)
amq_set_warnings(amq_dsp)
