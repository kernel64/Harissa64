# Big-endian 64-bit PowerPC Linux cross build, run under qemu-ppc64 (WSL).
# Catches endianness bugs and, later, runs the MIPS->PowerPC dynarec without
# a console. Usage (from WSL):
#   cmake -S . -B build-ppc64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ppc64be.cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR ppc64)
set(CMAKE_C_COMPILER powerpc64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER powerpc64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /usr/powerpc64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# Static binaries: qemu-ppc64 then needs no sysroot (-L) to run them.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-ppc64)
