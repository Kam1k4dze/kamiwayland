cmake_minimum_required(VERSION 4.2)
if (NOT DEFINED CMAKE_EXPERIMENTAL_CXX_IMPORT_STD)
    if (CMAKE_VERSION VERSION_LESS "4.3")
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD d0edc3af-4c50-42ea-a356-e2862fe7a444)
    else ()
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD 451f2fe2-a8a2-47c3-bc32-94786d8fc91b)
    endif ()
endif ()
project(kamiwayland)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/KLFetch.cmake)

kl_fetch(tinyxml2
        https://github.com/leethomason/tinyxml2/archive/refs/tags/11.0.0.tar.gz
        5556deb5081fb246ee92afae73efd943c889cef0cafea92b0b82422d6a18f289
        EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(tinyxml2)

# Library

add_library(kamiwayland)

target_sources(kamiwayland
        PUBLIC FILE_SET CXX_MODULES FILES
        src/kamiwayland.cppm
        src/wire.cppm
        src/connection.cppm
        src/display.cppm
)

set_target_properties(kamiwayland PROPERTIES CXX_MODULE_STD ON)

# Scanner

add_executable(kscanner)

target_sources(kscanner
        PUBLIC FILE_SET CXX_MODULES FILES
        scanner/kscanner.cppm
        scanner/parser.cppm
        scanner/codegen.cppm
        PRIVATE
        scanner/main.cpp
        scanner/codegen.cpp
)

set_target_properties(kscanner PROPERTIES CXX_MODULE_STD ON)
target_link_libraries(kscanner PRIVATE tinyxml2)

# kscanner_add_protocols(target name xml [name xml ...])
#
# Creates a library target with compiled Wayland protocol modules.
# Each name/xml pair produces <name>.cppm via kscanner --name <name>.
#
# Example:
#   kscanner_add_protocols(my_protos
#       wl         /path/to/wayland.xml
#       xdg_output /path/to/xdg-output-unstable-v1.xml
#   )
#   target_link_libraries(my_exe PRIVATE my_protos)
function(kscanner_add_protocols target_name)
    set(gen ${CMAKE_CURRENT_BINARY_DIR}/kscanner_${target_name})
    file(MAKE_DIRECTORY ${gen})

    set(outputs)
    set(inputs)
    set(cmd_args)

    set(args ${ARGN})
    list(LENGTH args len)
    set(i 0)
    while (i LESS len)
        list(GET args ${i} ns)
        math(EXPR i "${i} + 1")
        list(GET args ${i} xml)
        math(EXPR i "${i} + 1")
        list(APPEND outputs ${gen}/${ns}.cppm)
        list(APPEND inputs ${xml})
        list(APPEND cmd_args --name ${ns} ${xml})
    endwhile ()

    add_custom_command(
            OUTPUT ${outputs}
            COMMAND kscanner ${cmd_args} -o ${gen}
            DEPENDS kscanner ${inputs}
            VERBATIM
    )

    add_library(${target_name})
    target_sources(${target_name}
            PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${gen} FILES ${outputs}
    )
    set_target_properties(${target_name} PROPERTIES CXX_MODULE_STD ON)
    target_link_libraries(${target_name} PUBLIC kamiwayland)
endfunction()

