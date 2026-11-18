# Am-Q editor (docs/plan/decisions.md D1, areas 12-15).
#   amq_ui_model: framework-free editor logic (gestures, attachments, graph model), unit-tested
#                 without a window.
#   amq_ui:       the Visage frames that draw the model and forward input.

add_library(amq_ui_model STATIC
    src/graph_model.cpp
    src/undo_controller.cpp
    src/band_ops.cpp
    src/band_menu.cpp
    src/key_dispatcher.cpp
    src/band_panel.cpp
    src/eq_match_apply.cpp
    src/display_format.cpp
    src/editor_layout.cpp
    src/graph_accessible.cpp)
add_library(amq::ui_model ALIAS amq_ui_model)
target_include_directories(amq_ui_model PUBLIC include)
target_link_libraries(amq_ui_model PUBLIC amq::params amq::dsp amq::analysis)
amq_set_warnings(amq_ui_model)

if(AMQ_BUILD_PLUGIN)
    add_library(amq_ui STATIC
        src/editor.cpp
        src/editor_theme.cpp
        src/file_dialog.cpp)
    if(WIN32)
        target_link_libraries(amq_ui PRIVATE comdlg32)
    endif()
    add_library(amq::ui ALIAS amq_ui)
    target_include_directories(amq_ui PUBLIC include)
    target_link_libraries(amq_ui PUBLIC amq::ui_model clap PRIVATE visage VisageEmbeddedFonts)
    amq_set_warnings(amq_ui)
endif()
