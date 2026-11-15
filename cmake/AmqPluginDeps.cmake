# Plugin-side third-party dependencies, pinned (THIRD_PARTY.md). Fetched at configure time.
#
# To reuse existing clones instead of downloading, set FETCHCONTENT_SOURCE_DIR_<NAME>, e.g.
#   -DFETCHCONTENT_SOURCE_DIR_AMQ_CLAP=spikes/clap-native/deps/clap
# for AMQ_CLAP, AMQ_CLAP_HELPERS, AMQ_CLAP_WRAPPER, AMQ_VST3SDK and AMQ_VISAGE.
include(FetchContent)

FetchContent_Declare(amq_clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG        1.2.10
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL)

# No release tags; main needs CLAP 1.2.10's draft param-hovered extension.
FetchContent_Declare(amq_clap_helpers
    GIT_REPOSITORY https://github.com/free-audio/clap-helpers.git
    GIT_TAG        a380c97
    EXCLUDE_FROM_ALL)

# Only the source tree is needed (clap-wrapper builds the parts it uses), so point
# SOURCE_SUBDIR at a directory without a CMakeLists.txt: populate, don't add.
FetchContent_Declare(amq_vst3sdk
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
    GIT_TAG        v3.8.0_build_66
    GIT_SHALLOW    TRUE
    GIT_SUBMODULES base pluginterfaces public.sdk cmake
    SOURCE_SUBDIR  amq-do-not-add)

FetchContent_Declare(amq_clap_wrapper
    GIT_REPOSITORY https://github.com/free-audio/clap-wrapper.git
    GIT_TAG        v0.17.0
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL)

FetchContent_Declare(amq_visage
    GIT_REPOSITORY https://github.com/VitalAudio/visage.git
    GIT_TAG        v1.0.0
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL)

FetchContent_MakeAvailable(amq_clap amq_clap_helpers amq_vst3sdk)

set(VISAGE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(VISAGE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(amq_visage)

set(CLAP_SDK_ROOT ${amq_clap_SOURCE_DIR} CACHE PATH "" FORCE)
set(VST3_SDK_ROOT ${amq_vst3sdk_SOURCE_DIR} CACHE PATH "" FORCE)
set(CLAP_WRAPPER_OUTPUT_NAME "Am-Q" CACHE STRING "" FORCE)
set(CLAP_WRAPPER_DONT_ADD_TARGETS TRUE CACHE BOOL "" FORCE)
set(CLAP_WRAPPER_CXX_STANDARD 20 CACHE STRING "" FORCE)
FetchContent_MakeAvailable(amq_clap_wrapper)
