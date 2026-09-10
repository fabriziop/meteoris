# Raspberry Pi 3B (Raspberry Pi OS Trixie, aarch64) cross-compilation toolchain.
# Usage example:
#   export RPI_SYSROOT=/opt/sysroots/rpi-trixie-aarch64
#   cmake --preset rpi3-aarch64-cross-release

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(RPI_SYSROOT "" CACHE PATH "Path to Raspberry Pi sysroot")
# During compiler ABI checks, CMake runs internal try-configures that may not
# preserve this cache variable unless explicitly propagated.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES RPI_SYSROOT)

if(NOT RPI_SYSROOT AND DEFINED ENV{RPI_SYSROOT} AND NOT "$ENV{RPI_SYSROOT}" STREQUAL "")
    set(RPI_SYSROOT "$ENV{RPI_SYSROOT}" CACHE PATH "Path to Raspberry Pi sysroot" FORCE)
endif()

if(NOT RPI_SYSROOT)
    message(FATAL_ERROR "RPI_SYSROOT is not set. Point it to your Raspberry Pi sysroot.")
endif()

set(CMAKE_SYSROOT "${RPI_SYSROOT}")

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc CACHE FILEPATH "C compiler")
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++ CACHE FILEPATH "C++ compiler")

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Ensure pkg-config resolves target libraries from the sysroot.
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
set(ENV{PKG_CONFIG_DIR} "")
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_LIBDIR}
    "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${CMAKE_SYSROOT}/usr/lib/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
