cmake_minimum_required(VERSION 3.12)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


file(GLOB PLUGIN_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
# Loop through each discovered source file
foreach(SOURCE_FILE ${PLUGIN_SOURCES})
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -std=c++20
    )
    # Suppress the ABI change warnings globally
    add_compile_options(-Wno-psabi)

    # Extract the filename without directory path or extension (e.g., "basic")
    get_filename_component(PLUGIN_NAME ${SOURCE_FILE} NAME_WE)

    # Create a unique MODULE target for this file (handles -shared and -fPIC)
    add_library(${PLUGIN_NAME} MODULE ${SOURCE_FILE})

    # Apply include directories
    target_include_directories(${PLUGIN_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        /usr/include
    )

    #  Direct the compilation output to build-native/bin/
    # This names the file "lib[filename].so" inside the binary directory
    set_target_properties(${PLUGIN_NAME} PROPERTIES
        PREFIX "lib"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

endforeach()