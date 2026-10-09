# Harissa64 V2 - host builds (Windows MSVC, Linux gcc, big-endian PowerPC
# cross build run under qemu-ppc64). The Xbox 360 build uses the Visual
# Studio 2010 project in platform/xbox360 instead.
cmake_minimum_required(VERSION 3.16)
project(Harissa64V2 C CXX)

file(READ "${CMAKE_SOURCE_DIR}/VERSION" H64_VERSION_RAW)
string(STRIP "${H64_VERSION_RAW}" H64_VERSION)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/wd4100;/wd4127>" /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  # -frounding-math: the reference FPU changes the rounding mode at run time.
  # -fwrapv: the software RDP relies on wrapping signed arithmetic (as GLSL).
  # -ffp-contract=off: no fused multiply-add, so the graphics HLE (float
  # geometry) gives the same result on x86 and PowerPC.
  add_compile_options(-Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing -frounding-math -fwrapv -ffp-contract=off)
endif()

# ---- zlib (inflate only, for zipped ROMs; Z_SOLO: no gz* file functions) ----
set(H64_ZLIB_SOURCES adler32.c crc32.c inffast.c inflate.c inftrees.c zutil.c)
list(TRANSFORM H64_ZLIB_SOURCES PREPEND "${CMAKE_SOURCE_DIR}/third_party/zlib/")
add_library(h64zlib STATIC ${H64_ZLIB_SOURCES})
target_compile_definitions(h64zlib PUBLIC Z_SOLO)
if(MSVC)
  target_compile_options(h64zlib PRIVATE /W0)
else()
  target_compile_options(h64zlib PRIVATE -w)
endif()

# ---- Portable core ----
file(GLOB_RECURSE H64_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/core/*.cpp")
add_library(h64core STATIC ${H64_CORE_SOURCES})
target_compile_definitions(h64core PUBLIC H64_VERSION=${H64_VERSION})
target_link_libraries(h64core PUBLIC h64zlib)

# ---- Unit tests (linked into every runner) ----
file(GLOB H64_UNIT_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/unit/*.cpp")

# ---- Headless runner ----
add_executable(h64test platform/host/h64test.cpp platform/host/png_write.cpp ${H64_UNIT_SOURCES})
target_link_libraries(h64test PRIVATE h64core)

enable_testing()
add_test(NAME unit COMMAND h64test --unit)
