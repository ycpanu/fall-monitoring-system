# Orange Pi 3B / RK3566 - Ubuntu Jammy (aarch64) cross toolchain.
# Copy this file to the ROOT of fall-monitoring-system alongside CMakeLists.txt.
# Copy the device's ARM64 sysroot to <project>/sysroot before configuring.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

get_filename_component(ORANGEPI_SYSROOT "${CMAKE_CURRENT_LIST_DIR}/sysroot" ABSOLUTE)
if(NOT EXISTS "${ORANGEPI_SYSROOT}/usr/include")
    message(FATAL_ERROR
        "ARM64 sysroot is missing: ${ORANGEPI_SYSROOT}. "
        "Copy /usr/include, /usr/lib and /lib from your Orange Pi first.")
endif()

set(CMAKE_SYSROOT "${ORANGEPI_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "${ORANGEPI_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Host pkg-config must only see target ARM64 .pc files.
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${ORANGEPI_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR}
    "${ORANGEPI_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${ORANGEPI_SYSROOT}/usr/lib/pkgconfig:${ORANGEPI_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
