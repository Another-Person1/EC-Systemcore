# Reproducible Linux AArch64 cross-compilation contract for Raspberry Pi CM5.
# The CI image supplies Debian Bookworm's aarch64-linux-gnu toolchain and sysroot.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++ CACHE FILEPATH "")
set(CMAKE_AR aarch64-linux-gnu-ar CACHE FILEPATH "")
set(CMAKE_RANLIB aarch64-linux-gnu-ranlib CACHE FILEPATH "")
set(CMAKE_STRIP aarch64-linux-gnu-strip CACHE FILEPATH "")

set(EC_SYSTEMCORE_TARGET_ROOT "/usr/aarch64-linux-gnu" CACHE PATH
    "Pinned target sysroot search prefix")
set(CMAKE_FIND_ROOT_PATH "${EC_SYSTEMCORE_TARGET_ROOT}" CACHE STRING "")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# CMake packages which contribute target libraries or headers must resolve
# exclusively inside the ARM64 sysroot. Host build tools are still found by
# find_program because PROGRAM mode is NEVER.
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_CROSSCOMPILING_EMULATOR
    qemu-aarch64;-L;${EC_SYSTEMCORE_TARGET_ROOT}
    CACHE STRING "AArch64 smoke-test emulator")

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
