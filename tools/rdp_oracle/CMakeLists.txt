# rdp-oracle: replays an RDPDUMP2 file (ParaLLEl-RDP's dump format) with
# ParaLLEl-RDP (Vulkan compute, MIT) and Angrylion-Plus (MAME licence), and
# writes their RDRAM at chosen frames. A PC-only test tool, never part of the
# emulator: it is built against local clones of the two projects, kept outside
# this repository (docs-local/render_tests_study.md).
#
#   cmake -S tools/rdp_oracle -B build-oracle -G Ninja -DCMAKE_BUILD_TYPE=Release
#         -DPARALLEL_RDP_DIR=E:/IA-labs/Xenon64/parallel-rdp-standalone
#         -DANGRYLION_DIR=E:/IA-labs/Xenon64/parallel-rdp/angrylion-rdp-plus
cmake_minimum_required(VERSION 3.10)
project(rdp-oracle LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_C_STANDARD 99)

set(PARALLEL_RDP_DIR "" CACHE PATH "parallel-rdp-standalone checkout")
set(ANGRYLION_DIR "" CACHE PATH "angrylion-rdp-plus checkout (Themaister's fork, the one parallel-rdp pins)")
if (NOT EXISTS "${PARALLEL_RDP_DIR}/parallel-rdp/rdp_device.cpp")
    message(FATAL_ERROR "PARALLEL_RDP_DIR does not point to parallel-rdp-standalone")
endif()
if (NOT EXISTS "${ANGRYLION_DIR}/src/core/n64video.c")
    message(FATAL_ERROR "ANGRYLION_DIR does not point to angrylion-rdp-plus")
endif()

set(P ${PARALLEL_RDP_DIR})
file(GLOB PRDP_SRC ${P}/parallel-rdp/*.cpp)
set(PRDP_SRC ${PRDP_SRC}
    ${P}/vulkan/buffer.cpp ${P}/vulkan/buffer_pool.cpp ${P}/vulkan/command_buffer.cpp ${P}/vulkan/command_pool.cpp
    ${P}/vulkan/context.cpp ${P}/vulkan/cookie.cpp ${P}/vulkan/descriptor_set.cpp ${P}/vulkan/device.cpp
    ${P}/vulkan/event_manager.cpp ${P}/vulkan/fence.cpp ${P}/vulkan/fence_manager.cpp ${P}/vulkan/image.cpp
    ${P}/vulkan/indirect_layout.cpp ${P}/vulkan/memory_allocator.cpp ${P}/vulkan/pipeline_event.cpp
    ${P}/vulkan/query_pool.cpp ${P}/vulkan/render_pass.cpp ${P}/vulkan/sampler.cpp ${P}/vulkan/semaphore.cpp
    ${P}/vulkan/semaphore_manager.cpp ${P}/vulkan/shader.cpp ${P}/vulkan/texture/texture_format.cpp
    ${P}/util/arena_allocator.cpp ${P}/util/logging.cpp ${P}/util/thread_id.cpp ${P}/util/aligned_alloc.cpp
    ${P}/util/timer.cpp ${P}/util/timeline_trace_file.cpp ${P}/util/environment.cpp ${P}/util/thread_name.cpp
    ${P}/volk/volk.c)
add_library(prdp STATIC ${PRDP_SRC})
target_include_directories(prdp PUBLIC ${P}/parallel-rdp ${P}/volk ${P}/vulkan ${P}/vulkan-headers/include ${P}/util)
if (WIN32)
    target_compile_definitions(prdp PUBLIC VK_USE_PLATFORM_WIN32_KHR NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(prdp PUBLIC winmm)
endif()
if (MSVC)
    target_compile_options(prdp PRIVATE /wd4267 /wd4244 /wd4309 /wd4005)
endif()

set(A ${ANGRYLION_DIR}/src/core)
add_library(alp STATIC ${A}/n64video.c ${A}/parallel.cpp)
target_include_directories(alp PUBLIC ${A})
if (MSVC)
    target_compile_definitions(alp PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(alp PRIVATE /wd4267 /wd4244 /wd4996)
endif()

add_executable(rdp-oracle rdp_oracle.cpp)
target_link_libraries(rdp-oracle PRIVATE prdp alp)
if (MSVC)
    target_compile_definitions(rdp-oracle PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
