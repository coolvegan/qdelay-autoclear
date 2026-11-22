# CMake toolchain for a Linux AArch64 build on a Debian/Ubuntu x86_64 host
# with the arm64 multiarch development packages installed.
#
# Used by scripts/build-juce-aarch64.sh inside docker/Dockerfile.juce-aarch64.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Find arm64 libraries in /usr/lib/aarch64-linux-gnu, headers in /usr/include.
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH /usr)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Multiarch pkg-config: only the arm64 .pc files, never the host's.
set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "/")

set(CMAKE_C_FLAGS_INIT "-march=armv8-a")
set(CMAKE_CXX_FLAGS_INIT "-march=armv8-a -fPIC")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--as-needed")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-Wl,--as-needed -static-libstdc++ -static-libgcc")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-Wl,--as-needed -static-libstdc++ -static-libgcc -Wl,-s")
