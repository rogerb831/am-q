# FFT backend for core/dsp (area 01 R22): PFFFT, BSD-style licence (FFTPACKv5 / UCAR plus the
# PFFFT authors), cleared by Counsel on 2026-11-15. Pinned; recorded in THIRD_PARTY.md.
#
# The repository also carries benchmark-only copies of other FFT libraries (KissFFT,
# PocketFFT, Green FFT, FFTPACK); none of them is compiled. SOURCE_SUBDIR points at a
# directory without a CMakeLists.txt, so the tree is only populated, and Am-Q builds just the
# two single-precision core files in its own target.
#
# To reuse an existing clone: -DFETCHCONTENT_SOURCE_DIR_AMQ_PFFFT=<path>
include(FetchContent)

FetchContent_Declare(amq_pffft
    GIT_REPOSITORY https://github.com/marton78/pffft.git
    GIT_TAG        179a48d486cbee8ec5416e5c163ad575c2efe1f1  # v1.3.0
    SOURCE_SUBDIR  amq-do-not-add)
FetchContent_MakeAvailable(amq_pffft)

add_library(amq_pffft STATIC
    ${amq_pffft_SOURCE_DIR}/src/pffft.c
    ${amq_pffft_SOURCE_DIR}/src/pffft_common.c)
target_include_directories(amq_pffft
    PUBLIC  ${amq_pffft_SOURCE_DIR}/include
    PRIVATE ${amq_pffft_SOURCE_DIR}/src ${amq_pffft_SOURCE_DIR}/include/pffft)
target_compile_definitions(amq_pffft
    PUBLIC  PFFFT_STATIC_DEFINE
    PRIVATE _USE_MATH_DEFINES PFFFT_SCALVEC_ENABLED=1)
set_target_properties(amq_pffft PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(amq_pffft PRIVATE /W0)  # third-party code: no warnings of ours
else()
    target_compile_options(amq_pffft PRIVATE -w)
    target_link_libraries(amq_pffft PRIVATE m)
endif()
