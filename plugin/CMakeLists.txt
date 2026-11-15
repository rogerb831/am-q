# Am-Q plugin: CLAP on clap-helpers; VST3 from the same static library via clap-wrapper's
# clap-first build (docs/plan/decisions.md D1).

add_library(amq_plugin_impl STATIC
    src/amq_plugin.cpp
    src/entry.cpp)
target_include_directories(amq_plugin_impl PUBLIC src)
target_link_libraries(amq_plugin_impl PUBLIC amq::dsp amq::params amq::engine amq::ui clap clap-helpers)
target_compile_definitions(amq_plugin_impl PUBLIC AMQ_STRICT_HOST_CHECKS=$<BOOL:${AMQ_STRICT_HOST_CHECKS}>)
amq_set_warnings(amq_plugin_impl)

# Am-Q.clap and Am-Q.vst3.
make_clapfirst_plugins(
    TARGET_NAME AmQ
    IMPL_TARGET amq_plugin_impl
    OUTPUT_NAME "Am-Q"
    ENTRY_SOURCE "src/clap_entry.cpp"
    BUNDLE_IDENTIFIER "dev.am-q.eq"
    BUNDLE_VERSION ${PROJECT_VERSION}
    PLUGIN_FORMATS CLAP VST3
    WINDOWS_FOLDER_VST3 TRUE
    COPY_AFTER_BUILD FALSE)
