cmake_minimum_required(VERSION 3.16)

# This directory is normally included by the top-level Meteoris project.
# Keep it valid as a standalone CMake source as well, so accidental/direct
# configuration never falls back to CMake's implicit Project project.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../VERSION" METEORIS_SIM_VERSION LIMIT_COUNT 1)
    string(STRIP "${METEORIS_SIM_VERSION}" METEORIS_SIM_VERSION)
    project(MeteorisSim VERSION ${METEORIS_SIM_VERSION} LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 11)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

find_package(SoapySDR CONFIG REQUIRED)

SOAPY_SDR_MODULE_UTIL(
    TARGET meteorisSim
    SOURCES SoapyMeteorisSim.cpp
)

# Keep the freshly built plugin in a predictable directory. run_sim.sh uses
# this path so simulator tests cannot silently pick up an older installed
# meteoris_sim module.
set_target_properties(meteorisSim PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/soapy-modules"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/soapy-modules"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(meteorisSim PRIVATE -Wall -Wextra -Wpedantic)
elseif(MSVC)
    target_compile_options(meteorisSim PRIVATE /W4 /utf-8)
endif()
