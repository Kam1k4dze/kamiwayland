cmake_minimum_required(VERSION 4.2)
if (NOT DEFINED CMAKE_EXPERIMENTAL_CXX_IMPORT_STD)
    if (CMAKE_VERSION VERSION_LESS "4.3")
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD d0edc3af-4c50-42ea-a356-e2862fe7a444)
    else ()
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD 451f2fe2-a8a2-47c3-bc32-94786d8fc91b)
    endif ()
endif ()

project(my_wayland_app LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
endif ()

# kamiwayland
# Point KAMIWAYLAND_DIR at the kamiwayland source tree.
# The default works when building from inside the repository.
# For an out-of-tree project: cmake -DKAMIWAYLAND_DIR=/path/to/kamiwayland
set(KAMIWAYLAND_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.."
        CACHE PATH "Path to kamiwayland source directory")

add_subdirectory(${KAMIWAYLAND_DIR} ${CMAKE_CURRENT_BINARY_DIR}/kamiwayland)

# Protocol XML files
set(PROTOCOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/protocols"
        CACHE PATH "Directory containing Wayland protocol XML files")

# Examples
add_subdirectory(01_list_globals)
add_subdirectory(02_shm_window)
add_subdirectory(03_bouncy_box)