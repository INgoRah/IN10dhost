cmake_minimum_required(VERSION 3.16)
project(IN10dfsd VERSION 0.1 LANGUAGES CXX)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake" ${CMAKE_MODULE_PATH})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ----------------------------
# Build options
# ----------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(USE_I2C "Build with I2C support" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE3 REQUIRED fuse3)
if(FUSE3_FOUND)
    message(STATUS "Using fuse: ${FUSE3_VERSION}")
endif()
find_package(nlohmann_json 3.10.5 REQUIRED)

# ----------------------------
# Build options for dependencies
# ----------------------------
option(TESTING "Build Testing" OFF)
option(NO_TERM "No terminal available" OFF)
option(SANITIZE "Build with sanitizer" OFF)

option(USE_GPIO "Build with libgpiod support" ON)

if(USE_GPIO)
    pkg_check_modules(GPIOD REQUIRED libgpiod)

    # Get the version and check if it's v1 or v2
    if(GPIOD_FOUND)
        if(GPIOD_VERSION VERSION_GREATER_EQUAL "2.0")
            message(STATUS "Using libgpiod v2: ${GPIOD_VERSION}")
            add_compile_definitions(GPIOD_V2)
        else()
            message(STATUS "Using libgpiod v1: ${GPIOD_VERSION}")
            add_compile_definitions(GPIOD_V1)
        endif()
    endif()
    add_compile_definitions(USE_GPIO)
endif()
if(USE_I2C)
    #find_package(I2C REQUIRED)
    message(STATUS "Using i2c ${I2C_VERSION}")
    add_compile_definitions(USE_I2C)
endif()

#include_directories("/usr/include/")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(USE_DEBUG)
    add_compile_options(-g)
endif()

# Include the library subdirectory
if(TESTING)
    file(COPY test/plugin/invalid_symbol.cpp DESTINATION plugin)
endif()
add_subdirectory(plugin)

if(TESTING)
    find_package(GTest REQUIRED)
    include(GoogleTest)
    add_compile_definitions(TESTING)
endif()
if(NO_TERM)
    # used for github action
    add_compile_definitions(NO_TERM)
endif()

# ----------------------------
# Compiler warnings
# ----------------------------
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -std=c++20
    -Wno-psabi
    -DFUSE_USE_VERSION=30
)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(USE_DEBUG)
    if(SANITIZE)
        add_compile_options(
            -fsanitize=address
            -fno-omit-frame-pointer)
    endif()
endif()

if(TESTING)
    add_compile_options(
        --coverage
        -O0
    )
endif()

# ----------------------------
# Sources
# ----------------------------
add_executable(IN10dfsd
    src/logger.cpp
    src/fs.cpp
    src/ard_i2c.cpp
    src/ds2482.cpp
    src/ow_dev.cpp
    src/ds2408.cpp
    src/ds2450.cpp
    src/ds1820.cpp
    src/ow_devices.cpp
    src/plugins.cpp
    src/switch_handler.cpp
)
if(TESTING)
    target_compile_definitions(IN10dfsd PRIVATE TESTING=1)
    target_sources(IN10dfsd
        PRIVATE
        test/main.cpp
        test/fs.cpp
        test/devices.cpp
        test/switches.cpp
        test/plugins.cpp
    )
    set(test GTest::gtest_main)

    gtest_discover_tests(IN10dfsd)
else()
    target_sources(IN10dfsd
        PRIVATE
        src/main.cpp
    )
endif()

include_directories(
    include
    ${FUSE3_INCLUDE_DIRS}
)

target_include_directories(IN10dfsd
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${GPIOD_INCLUDE_DIRS}
)

# ----------------------------
# Linux-specific libraries
# ----------------------------
target_link_libraries(IN10dfsd
    PRIVATE
        pthread
        dl
        ${test}
        ${GPIOD_LIBRARIES}
        ${FUSE3_LIBRARIES}
        nlohmann_json::nlohmann_json
)

if(TESTING)
    target_link_options(IN10dfsd
        PRIVATE
            -static-libstdc++
            -static-libgcc
            --coverage -g -O0
    )
else()
    target_link_options(IN10dfsd
        PRIVATE
            -static-libstdc++
            -static-libgcc
            -static
    )
endif()
if(SANITIZE)
    target_link_options(IN10dfsd
        PRIVATE
       -fsanitize=address
    )
endif()

# ----------------------------
# Install (optional)
# ----------------------------
install(TARGETS IN10dfsd
   RUNTIME DESTINATION /usr/bin
)


