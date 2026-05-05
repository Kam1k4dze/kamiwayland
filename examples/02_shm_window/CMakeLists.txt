kscanner_add_protocols(shm_window_protos
    wl             ${PROTOCOLS_DIR}/wayland.xml
    xdg            ${PROTOCOLS_DIR}/xdg-shell.xml
    xdg_decoration ${PROTOCOLS_DIR}/xdg-decoration-unstable-v1.xml
)

add_executable(shm_window main.cpp)
set_target_properties(shm_window PROPERTIES CXX_MODULE_STD ON)
target_link_libraries(shm_window PRIVATE shm_window_protos)