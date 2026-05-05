kscanner_add_protocols(list_globals_protos
    wl ${PROTOCOLS_DIR}/wayland.xml
)

add_executable(list_globals main.cpp)
set_target_properties(list_globals PROPERTIES CXX_MODULE_STD ON)
target_link_libraries(list_globals PRIVATE list_globals_protos)