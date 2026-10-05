# Harissa64 V2 - host builds (Windows MSVC, Linux gcc, big-endian PowerPC
# cross build run under qemu-ppc64). The Xbox 360 build uses the Visual
# Studio 2010 project in platform/xbox360 instead.
cmake_minimum_required(VERSION 3.16)
project(Harissa64V2 CXX)

file(READ "${CMAKE_SOURCE_DIR}/VERSION" H64_VERSION_RAW)
string(STRIP "${H64_VERSION_RAW}" H64_VERSION)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /wd4100 /wd4127)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  # -frounding-math: the reference FPU changes the rounding mode at run time.
  add_compile_options(-Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing -frounding-math)
endif()

# ---- Portable core ----
file(GLOB_RECURSE H64_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/core/*.cpp")
add_library(h64core STATIC ${H64_CORE_SOURCES})
target_compile_definitions(h64core PUBLIC H64_VERSION=${H64_VERSION})

# ---- Unit tests (linked into every runner) ----
file(GLOB H64_UNIT_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/unit/*.cpp")

# ---- Headless runner ----
add_executable(h64test platform/host/h64test.cpp ${H64_UNIT_SOURCES})
target_link_libraries(h64test PRIVATE h64core)

enable_testing()
add_test(NAME unit COMMAND h64test --unit)
