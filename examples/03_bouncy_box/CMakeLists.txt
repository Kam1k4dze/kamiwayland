kscanner_add_protocols(bouncy_box_protos
    wl             ${PROTOCOLS_DIR}/wayland.xml
    xdg            ${PROTOCOLS_DIR}/xdg-shell.xml
    xdg_decoration ${PROTOCOLS_DIR}/xdg-decoration-unstable-v1.xml
)

add_executable(bouncy_box main.cpp)
set_target_properties(bouncy_box PROPERTIES CXX_MODULE_STD ON)
target_link_libraries(bouncy_box PRIVATE bouncy_box_protos)