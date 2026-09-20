cmake_minimum_required(VERSION 3.16)

# VERSION is the single authoritative package version. Keep release version
# changes to this one file; CMake, C++, Python tools, packaging, and docs derive
# their release version from it.
file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" METEORIS_VERSION LIMIT_COUNT 1)
string(STRIP "${METEORIS_VERSION}" METEORIS_VERSION)
if(NOT METEORIS_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "Invalid VERSION '${METEORIS_VERSION}'; expected MAJOR.MINOR.PATCH")
endif()

project(Meteoris
    VERSION ${METEORIS_VERSION}
    DESCRIPTION "Real-time SDR meteor detector, recorder and simulator"
    LANGUAGES C CXX
)

# Supported target matrix from one source tree:
#   - Linux x86/x86_64
#   - Linux ARM64
#   - Windows x86/x86_64
# Windows ARM/ARM64 is intentionally excluded.
if(WIN32)
    string(TOUPPER "${CMAKE_SYSTEM_PROCESSOR}" METEORIS_SYSTEM_PROCESSOR_UPPER)
    string(TOUPPER "${CMAKE_GENERATOR_PLATFORM}" METEORIS_GENERATOR_PLATFORM_UPPER)
    string(TOUPPER "${MSVC_CXX_ARCHITECTURE_ID}" METEORIS_MSVC_ARCH_UPPER)
    if(METEORIS_SYSTEM_PROCESSOR_UPPER MATCHES "ARM" OR
       METEORIS_GENERATOR_PLATFORM_UPPER MATCHES "ARM" OR
       METEORIS_MSVC_ARCH_UPPER MATCHES "ARM")
        message(FATAL_ERROR
            "Windows ARM/ARM64 is not a supported Meteoris target. "
            "Use Windows x86/x64, or Linux ARM64.")
    endif()
endif()

configure_file(
    "${CMAKE_SOURCE_DIR}/src/version.hpp.in"
    "${CMAKE_BINARY_DIR}/generated/version.hpp"
    @ONLY
)

# Make VERSION available beside build-tree Python commands as well.
configure_file(
    "${CMAKE_SOURCE_DIR}/VERSION"
    "${CMAKE_BINARY_DIR}/VERSION"
    COPYONLY
)

option(METEORIS_BUILD_SIM "Build the SoapySDR meteoris_sim plugin" ON)
option(METEORIS_NATIVE_OPTIMIZATION "Optimize generated code for the build host CPU" ON)
option(METEORIS_FAST_MATH "Enable fast floating-point math for DSP kernels" ON)
option(METEORIS_USE_FFTW "Use FFTW3 for FFT instead of internal Radix-2 FFT" ON)
option(METEORIS_X86_AVX2 "Build the optional x86/x64 AVX2 runtime-dispatch kernel" ON)
option(METEORIS_INSTALL_PLOT "Install the meteoris_plot viewer tool" ON)
option(METEORIS_INSTALL_RECOVER_HDF5 "Install the meteoris_recover_hdf5 recovery tool" ON)
option(METEORIS_INSTALL_CONFIG_TOOL "Install the interactive meteoris_config tool" ON)

option(METEORIS_INSTALL_WEB "Install the meteoris_web browser gateway" ON)
set(METEORIS_CPU_TUNE "" CACHE STRING "Optional CPU tuning value used as -mcpu=<value>")

if(CMAKE_CROSSCOMPILING AND METEORIS_NATIVE_OPTIMIZATION)
    message(STATUS "Cross-compiling detected: forcing METEORIS_NATIVE_OPTIMIZATION=OFF")
    set(METEORIS_NATIVE_OPTIMIZATION OFF CACHE BOOL
        "Optimize generated code for the build host CPU" FORCE)
endif()

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(METEORIS_SOAPY_TARGET "")
set(METEORIS_SOAPY_INCLUDE_DIRS "")
set(METEORIS_SOAPY_HAS_CONFIG_PACKAGE OFF)

# Prefer SoapySDR CMake config package when available.
find_package(SoapySDR CONFIG QUIET)
if(SoapySDR_FOUND)
    set(METEORIS_SOAPY_HAS_CONFIG_PACKAGE ON)
    if(TARGET SoapySDR::SoapySDR)
        set(METEORIS_SOAPY_TARGET SoapySDR::SoapySDR)
    elseif(SoapySDR_LIBRARIES)
        set(METEORIS_SOAPY_TARGET ${SoapySDR_LIBRARIES})
        set(METEORIS_SOAPY_INCLUDE_DIRS ${SoapySDR_INCLUDE_DIRS})
    endif()
endif()

# Fall back to pkg-config for distributions that do not ship SoapySDRConfig.cmake.
if(NOT METEORIS_SOAPY_TARGET)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SOAPYSDR QUIET IMPORTED_TARGET SoapySDR)
        if(SOAPYSDR_FOUND)
            set(METEORIS_SOAPY_TARGET PkgConfig::SOAPYSDR)
            set(METEORIS_SOAPY_INCLUDE_DIRS ${SOAPYSDR_INCLUDE_DIRS})
        endif()
    endif()
endif()

if(NOT METEORIS_SOAPY_TARGET)
    message(FATAL_ERROR
        "SoapySDR not found. Install development files and/or set SoapySDR_DIR or CMAKE_PREFIX_PATH.")
endif()

find_package(HDF5 REQUIRED COMPONENTS C)
if(HDF5_VERSION VERSION_LESS "1.10")
    message(FATAL_ERROR "Meteoris SWMR support requires HDF5 >= 1.10")
endif()
find_package(Threads REQUIRED)

# spdlog can be consumed either through its CMake package or directly from an
# installed header tree.  The header-only path is particularly useful on
# Windows because an spdlog installation produced from a build tree can leave
# a non-relocatable spdlogConfig.cmake that still references the original
# source directory.
set(METEORIS_SPDLOG_INCLUDE_DIR "" CACHE PATH
    "Use spdlog headers from this directory instead of loading spdlogConfig.cmake")

if(METEORIS_SPDLOG_INCLUDE_DIR)
    if(NOT EXISTS "${METEORIS_SPDLOG_INCLUDE_DIR}/spdlog/spdlog.h")
        message(FATAL_ERROR
            "METEORIS_SPDLOG_INCLUDE_DIR does not contain spdlog/spdlog.h: ${METEORIS_SPDLOG_INCLUDE_DIR}")
    endif()
    add_library(meteoris_spdlog_header_only INTERFACE)
    target_include_directories(meteoris_spdlog_header_only INTERFACE
        "${METEORIS_SPDLOG_INCLUDE_DIR}")
    set(METEORIS_SPDLOG_TARGET meteoris_spdlog_header_only)
    message(STATUS "Using spdlog header-only from: ${METEORIS_SPDLOG_INCLUDE_DIR}")
else()
    find_package(spdlog CONFIG REQUIRED)
    set(METEORIS_SPDLOG_TARGET spdlog::spdlog)
endif()

# Optional FFTW3 support for FFT backend
if(METEORIS_USE_FFTW)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFTW3F IMPORTED_TARGET fftw3f)
        if(FFTW3F_FOUND)
            message(STATUS "Found FFTW3f: ${FFTW3F_LIBRARIES}")
            set(METEORIS_FFT_LIBRARIES PkgConfig::FFTW3F)
        else()
            message(WARNING "FFTW3f not found via pkg-config, falling back to CMake find_package")
            find_package(FFTW3 COMPONENTS FLOAT)
            if(FFTW3_FOUND)
                set(METEORIS_FFT_LIBRARIES FFTW3::FFTW3F)
            else()
                message(WARNING "FFTW3f not found, building with internal Radix-2 FFT")
                set(METEORIS_USE_FFTW OFF)
            endif()
        endif()
    else()
        find_package(FFTW3 COMPONENTS FLOAT)
        if(FFTW3_FOUND)
            set(METEORIS_FFT_LIBRARIES FFTW3::FFTW3F)
        else()
            message(WARNING "FFTW3f not found, building with internal Radix-2 FFT")
            set(METEORIS_USE_FFTW OFF)
        endif()
    endif()
endif()

if(METEORIS_USE_FFTW)
    message(STATUS "Building with FFTW3f FFT backend")
else()
    message(STATUS "Building with internal Radix-2 FFT backend")
endif()

set(METEORIS_X86_TARGET OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|i.86|x86|X86)$" OR
   MSVC_CXX_ARCHITECTURE_ID MATCHES "^(X86|x64|X64)$")
    set(METEORIS_X86_TARGET ON)
endif()

add_executable(meteoris
    src/meteoris.cpp
    src/dsp/simd_dot.cpp
    src/detector/detector_registry.cpp
    src/detector/echoes_automatic_detector.cpp
    src/detector/peak_tracker_detector.cpp
    src/fft/fft_backend.cpp
    src/network/network_server.cpp
)

# Keep the general x86/x64 binary at the platform baseline and isolate AVX2 in
# one translation unit. The common dispatcher checks CPU + OS support once at
# runtime before calling this kernel, so the same binary also runs on non-AVX2
# x86/x64 systems.
if(METEORIS_X86_TARGET AND METEORIS_X86_AVX2)
    target_sources(meteoris PRIVATE src/dsp/simd_dot_avx2.cpp)
    target_compile_definitions(meteoris PRIVATE METEORIS_HAVE_AVX2_KERNEL=1)
    if(MSVC)
        set_source_files_properties(src/dsp/simd_dot_avx2.cpp PROPERTIES
            COMPILE_OPTIONS "/arch:AVX2")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set_source_files_properties(src/dsp/simd_dot_avx2.cpp PROPERTIES
            COMPILE_OPTIONS "-mavx2")
    endif()
    message(STATUS "x86/x64 SIMD: AVX2 runtime-dispatch kernel enabled")
elseif(METEORIS_X86_TARGET)
    message(STATUS "x86/x64 SIMD: AVX2 kernel disabled; scalar baseline only")
endif()

target_include_directories(meteoris PRIVATE
    src
    "${CMAKE_BINARY_DIR}/generated"
    ${HDF5_INCLUDE_DIRS}
    ${METEORIS_SOAPY_INCLUDE_DIRS}
    ${FFTW3F_INCLUDE_DIRS}
)

if(TARGET HDF5::HDF5)
    set(METEORIS_HDF5_TARGET HDF5::HDF5)
else()
    set(METEORIS_HDF5_TARGET ${HDF5_LIBRARIES})
endif()

target_link_libraries(meteoris PRIVATE
    ${METEORIS_SOAPY_TARGET}
    ${METEORIS_HDF5_TARGET}
    Threads::Threads
    ${METEORIS_SPDLOG_TARGET}
)

if(WIN32)
    target_link_libraries(meteoris PRIVATE ws2_32)
endif()

if(METEORIS_USE_FFTW)
    target_link_libraries(meteoris PRIVATE ${METEORIS_FFT_LIBRARIES})
    target_compile_definitions(meteoris PRIVATE METEORIS_USE_FFTW)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(meteoris PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )

    if(METEORIS_FAST_MATH)
        target_compile_options(meteoris PRIVATE
            -ffast-math
            -fno-math-errno
        )
    endif()

    if(METEORIS_CPU_TUNE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm|armv7l|armv8l)$")
            target_compile_options(meteoris PRIVATE -mcpu=${METEORIS_CPU_TUNE})
        else()
            message(WARNING
                "Ignoring METEORIS_CPU_TUNE='${METEORIS_CPU_TUNE}' on non-ARM target "
                "processor '${CMAKE_SYSTEM_PROCESSOR}'")
        endif()
    elseif(METEORIS_NATIVE_OPTIMIZATION)
        if(METEORIS_X86_TARGET)
            # Tune scheduling for the build host without raising the required
            # x86 ISA baseline; AVX2 is isolated in its dispatched source file.
            target_compile_options(meteoris PRIVATE -mtune=native)
            message(STATUS "x86/x64 baseline kept portable; using -mtune=native plus runtime SIMD dispatch")
        else()
            target_compile_options(meteoris PRIVATE -march=native)
        endif()
    endif()
elseif(MSVC)
    # spdlog's bundled fmt requires UTF-8 source/execution character sets on MSVC.
    target_compile_options(meteoris PRIVATE /W4 /utf-8)
    if(METEORIS_FAST_MATH)
        target_compile_options(meteoris PRIVATE /fp:fast)
    endif()
    if(METEORIS_NATIVE_OPTIMIZATION AND METEORIS_X86_TARGET)
        message(STATUS
            "MSVC x86/x64 baseline kept portable; AVX2 is selected at runtime when available.")
    endif()
endif()

if(METEORIS_BUILD_SIM)
    if(METEORIS_SOAPY_HAS_CONFIG_PACKAGE)
        add_subdirectory(sim)
    else()
        message(WARNING
            "METEORIS_BUILD_SIM=ON but SoapySDR CMake config package was not found. "
            "Skipping simulator plugin build. Install SoapySDRConfig.cmake and set SoapySDR_DIR to enable it.")
    endif()
endif()

include(GNUInstallDirs)

# Python helper commands are exposed in the build tree as well as installed.
# POSIX uses executable extensionless scripts; Windows keeps .py files and
# installs small .cmd launchers that invoke the Python launcher.
function(meteoris_prepare_python_tool tool_name source_name)
    if(WIN32)
        set(py_path "${CMAKE_BINARY_DIR}/${tool_name}.py")
        set(cmd_path "${CMAKE_BINARY_DIR}/${tool_name}.cmd")
        add_custom_command(
            OUTPUT "${py_path}" "${cmd_path}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_SOURCE_DIR}/tools/${source_name}" "${py_path}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_SOURCE_DIR}/tools/${tool_name}.cmd" "${cmd_path}"
            DEPENDS
                "${CMAKE_SOURCE_DIR}/tools/${source_name}"
                "${CMAKE_SOURCE_DIR}/tools/${tool_name}.cmd"
            COMMENT "Preparing ${tool_name} for Windows"
            VERBATIM
        )
        add_custom_target(${tool_name}_tool ALL DEPENDS "${py_path}" "${cmd_path}")
        install(FILES
            "tools/${source_name}"
            DESTINATION ${CMAKE_INSTALL_BINDIR}
            RENAME "${tool_name}.py"
            COMPONENT meteoris_runtime
        )
        install(PROGRAMS
            "tools/${tool_name}.cmd"
            DESTINATION ${CMAKE_INSTALL_BINDIR}
            COMPONENT meteoris_runtime
        )
    else()
        set(build_path "${CMAKE_BINARY_DIR}/${tool_name}")
        add_custom_command(
            OUTPUT "${build_path}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_SOURCE_DIR}/tools/${source_name}" "${build_path}"
            DEPENDS "${CMAKE_SOURCE_DIR}/tools/${source_name}"
            COMMENT "Preparing ${tool_name}"
            VERBATIM
        )
        add_custom_target(${tool_name}_tool ALL DEPENDS "${build_path}")
        install(PROGRAMS
            "tools/${source_name}"
            DESTINATION ${CMAKE_INSTALL_BINDIR}
            RENAME ${tool_name}
            COMPONENT meteoris_runtime
        )
    endif()
endfunction()

if(METEORIS_INSTALL_RECOVER_HDF5)
    meteoris_prepare_python_tool(meteoris_recover_hdf5 meteoris_recover_hdf5.py)
endif()

if(METEORIS_INSTALL_CONFIG_TOOL)
    meteoris_prepare_python_tool(meteoris_config meteoris_config.py)
endif()

if(METEORIS_INSTALL_PLOT)
    meteoris_prepare_python_tool(meteoris_plot meteoris_plot.py)
endif()

if(METEORIS_INSTALL_WEB)
    if(WIN32)
        install(PROGRAMS web/meteoris_web.py web/meteoris_web.cmd
                DESTINATION ${CMAKE_INSTALL_BINDIR}
                COMPONENT meteoris_runtime)
    else()
        install(PROGRAMS web/meteoris_web.py DESTINATION ${CMAKE_INSTALL_BINDIR}
                RENAME meteoris_web COMPONENT meteoris_runtime)
    endif()
    install(FILES web/index.html web/app.js web/style.css
            DESTINATION ${CMAKE_INSTALL_DATADIR}/meteoris/web
            COMPONENT meteoris_runtime)
endif()

install(TARGETS meteoris
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT meteoris_runtime
)

# These are examples/templates. User-edited runtime configuration should
# normally live beside the application or in a site-specific config dir.
install(FILES
    config/meteoris.toml
    config/meteoris_sim.toml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/meteoris
    COMPONENT meteoris_runtime
)

if(METEORIS_INSTALL_PLOT)
    install(FILES
        config/meteoris_plot.toml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/meteoris
        COMPONENT meteoris_runtime
    )
endif()

install(FILES README.md LICENSE VERSION
    DESTINATION ${CMAKE_INSTALL_DATADIR}/meteoris
    COMPONENT meteoris_runtime
)
