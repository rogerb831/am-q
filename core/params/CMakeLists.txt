# core/params: the Am-Q parameter table (areas 09, 10, 11, 21). Framework-free; depends only on
# core/dsp for the shape and slope enums and the frequency limits.

add_library(amq_params STATIC
    src/mappings.cpp
    src/param_specs.cpp
    src/parameter_table.cpp
    src/shape_info.cpp)
add_library(amq::params ALIAS amq_params)
target_include_directories(amq_params PUBLIC include)
target_link_libraries(amq_params PUBLIC amq::dsp)
target_compile_features(amq_params PUBLIC cxx_std_20)
amq_set_warnings(amq_params)
