cmake_minimum_required(VERSION 3.16)

project(Meteoris
    VERSION 0.5.0
    DESCRIPTION "Real-time SDR meteor detector, recorder and simulator"
    LANGUAGES C CXX
)

option(METEORIS_BUILD_SIM "Build the SoapySDR meteoris_sim plugin" ON)
option(METEORIS_NATIVE_OPTIMIZATION "Optimize generated code for the build host CPU" ON)
option(METEORIS_FAST_MATH "Enable fast floating-point math for DSP kernels" ON)
option(METEORIS_USE_FFTW "Use FFTW3 for FFT instead of internal Radix-2 FFT" ON)
option(METEORIS_INSTALL_PLOT "Install the meteoris_plot viewer tool" ON)
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
find_package(spdlog CONFIG REQUIRED)

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

add_executable(meteoris
    src/meteoris.cpp
    src/detector/detector_registry.cpp
    src/detector/echoes_automatic_detector.cpp
    src/detector/peak_tracker_detector.cpp
    src/fft/fft_backend.cpp
)

target_include_directories(meteoris PRIVATE
    src
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
    spdlog::spdlog
)

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
        target_compile_options(meteoris PRIVATE -march=native)
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

install(TARGETS meteoris
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT meteoris_runtime
)

# Install the Python viewer as a normal command without the .py suffix.
if(METEORIS_INSTALL_PLOT)
    install(PROGRAMS
        tools/meteoris_plot.py
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        RENAME meteoris_plot
        COMPONENT meteoris_runtime
    )
endif()

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

install(FILES README.md LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/meteoris
    COMPONENT meteoris_runtime
)
