cmake_minimum_required(VERSION 3.16)

project(Meteoris
    VERSION 0.4.0
    DESCRIPTION "Real-time SDR meteor detector, recorder and simulator"
    LANGUAGES C CXX
)

option(METEORIS_BUILD_SIM "Build the SoapySDR meteoris_sim plugin" ON)
option(METEORIS_NATIVE_OPTIMIZATION "Optimize generated code for the build host CPU" ON)
option(METEORIS_FAST_MATH "Enable fast floating-point math for DSP kernels" ON)
option(METEORIS_USE_FFTW "Use FFTW3 for FFT instead of internal Radix-2 FFT" ON)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SoapySDR CONFIG REQUIRED)
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
    ${SoapySDR_INCLUDE_DIRS}
    ${FFTW3F_INCLUDE_DIRS}
)

if(TARGET SoapySDR::SoapySDR)
    set(METEORIS_SOAPY_TARGET SoapySDR::SoapySDR)
else()
    set(METEORIS_SOAPY_TARGET ${SoapySDR_LIBRARIES})
endif()

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

    if(METEORIS_NATIVE_OPTIMIZATION)
        target_compile_options(meteoris PRIVATE -march=native)
    endif()
endif()

if(METEORIS_BUILD_SIM)
    add_subdirectory(sim)
endif()

include(GNUInstallDirs)

install(TARGETS meteoris
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT meteoris_runtime
)

# Install the Python viewer as a normal command without the .py suffix.
install(PROGRAMS
    tools/meteoris_plot.py
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    RENAME meteoris_plot
    COMPONENT meteoris_runtime
)

# These are examples/templates. User-edited runtime configuration should
# normally live beside the application or in a site-specific config dir.
install(FILES
    config/meteoris.toml
    config/meteoris_sim.toml
    config/meteoris_plot.toml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/meteoris
    COMPONENT meteoris_runtime
)

install(FILES README.md LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/meteoris
    COMPONENT meteoris_runtime
)
