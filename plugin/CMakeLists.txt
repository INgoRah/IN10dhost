cmake_minimum_required(VERSION 3.12)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# QuickJS is optional: without it only the scripting plugin is skipped,
# the other plugins and the daemon itself build unchanged.
find_package(PkgConfig REQUIRED)
pkg_check_modules(QJS qjs)

file(GLOB PLUGIN_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# faulty.cpp exists only to drive the loader's error handling from the
# tests; it must never end up on a target
if(NOT TESTING)
    list(REMOVE_ITEM PLUGIN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/faulty.cpp")
endif()

if(QJS_FOUND)
    message(STATUS "Using quickjs: ${QJS_VERSION}")
else()
    message(STATUS "quickjs not found, skipping the jsengine plugin")
    list(REMOVE_ITEM PLUGIN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/jsengine.cpp")
endif()

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

    # the scripting plugin is the only one with an external dependency.
    # This links the shared libqjs on purpose: the libqjs.a that also
    # ships in libqjs-dev is built without -fPIC and cannot go into a
    # shared module, so the target needs libqjs0 installed at runtime.
    if(PLUGIN_NAME STREQUAL "jsengine")
        target_include_directories(${PLUGIN_NAME} PRIVATE ${QJS_INCLUDE_DIRS})
        target_link_directories(${PLUGIN_NAME} PRIVATE ${QJS_LIBRARY_DIRS})
        target_link_libraries(${PLUGIN_NAME} PRIVATE ${QJS_LIBRARIES})
    endif()

    #  Direct the compilation output to build-native/bin/
    # This names the file "lib[filename].so" inside the binary directory
    set_target_properties(${PLUGIN_NAME} PROPERTIES
        PREFIX "lib"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    # the fault injection fixture stays in the build tree
    if(NOT PLUGIN_NAME STREQUAL "faulty")
        install(TARGETS ${PLUGIN_NAME}
            LIBRARY DESTINATION /opt/lib/IN10dfsd/
        )
    endif()

endforeach()

if(QJS_FOUND)
    # so the path the example script documents for itself actually exists
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/example.js
        DESTINATION /opt/lib/IN10dfsd/
    )
endif()
