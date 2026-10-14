cmake_minimum_required(VERSION 3.20)
project(gow3_uwp LANGUAGES C CXX)
add_executable(uwp-overlay-chord-test EXCLUDE_FROM_ALL ../tests/test_uwp_overlay_chord.cpp)
option(GOW3_BUILD_GRAPHICS "Build the existing renderer for UWP" OFF)
option(GOW3_LINK_RENDERER "Connect the existing renderer to the UWP loader" ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD_LIBRARIES "")
set(CMAKE_CXX_STANDARD_LIBRARIES "")
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "Use clang-cl: guest code uses the System V x64 ABI")
endif()
set(GOW3_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(GOW3_PTHREAD "${CMAKE_CURRENT_LIST_DIR}/.local/deps/winpthread" CACHE PATH "UCRT winpthread files")
set(GOW3_PREPARED "${CMAKE_CURRENT_LIST_DIR}/.local/prepared" CACHE PATH "Prepared guest image")
set(GOW3_UWP_SOURCES
  runtime runtime_content runtime_file runtime_kernel runtime_memory runtime_mutex
  runtime_rtc runtime_rwlock runtime_savedata runtime_sema runtime_services runtime_thread runtime_time
  compat_win cpu_check isa_emulate isa_rewrite probe)
list(TRANSFORM GOW3_UWP_SOURCES PREPEND "${GOW3_ROOT}/src/")
list(TRANSFORM GOW3_UWP_SOURCES APPEND ".c")
add_executable(gow3_uwp WIN32 platform/main.cpp platform/runtime_compat.c platform/milestone_services.c ${GOW3_UWP_SOURCES})
target_include_directories(gow3_uwp PRIVATE "${GOW3_ROOT}" "${GOW3_ROOT}/src" platform platform/compat "${GOW3_PTHREAD}/include")
target_compile_definitions(gow3_uwp PRIVATE GOW3_UWP NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _TIMESPEC_DEFINED)
target_compile_options(gow3_uwp PRIVATE /W3 $<$<COMPILE_LANGUAGE:C>:/clang:-Wno-deprecated-declarations> $<$<COMPILE_LANGUAGE:C>:/clang:-masm=att> $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
set_source_files_properties("${GOW3_ROOT}/src/isa_emulate.c" PROPERTIES COMPILE_OPTIONS "/clang:-mgeneral-regs-only")
target_link_options(gow3_uwp PRIVATE /APPCONTAINER /HIGHENTROPYVA:NO /STACK:16777216,16777216 /OPT:REF)
target_link_directories(gow3_uwp BEFORE PRIVATE "$ENV{VCToolsInstallDir}/lib/x64/store")
target_link_libraries(gow3_uwp PRIVATE WindowsApp "${GOW3_PTHREAD}/lib/libwinpthread.dll.a")
set(GOW3_LAYOUT "${CMAKE_BINARY_DIR}/layout")
if(GOW3_BUILD_GRAPHICS)
  add_compile_options(/clang:-Wno-unused-command-line-argument)
  set(GOW3_UWP ON)
  set(GOW3_LTO OFF CACHE BOOL "" FORCE)
  add_subdirectory("${GOW3_ROOT}/gpu" "${CMAKE_BINARY_DIR}/gpu")
  target_sources(gow3gpu PRIVATE graphics/overlay.cpp)
  target_sources(gow3_uwp PRIVATE graphics/probe.cpp graphics/present_bridge.cpp graphics/descriptor_fallback.cpp)
  target_include_directories(gow3_uwp PRIVATE "${GOW3_ROOT}/gpu/shadps4")
  target_compile_definitions(gow3_uwp PRIVATE GOW3_BUILD_GRAPHICS VK_USE_PLATFORM_WIN32_KHR)
  target_link_libraries(gow3_uwp PRIVATE Vulkan::Headers d3d12 dxgi)
  if(GOW3_LINK_RENDERER)
    target_compile_definitions(gow3_uwp PRIVATE GOW3_LINK_RENDERER)
    target_sources(gow3_uwp PRIVATE platform/input.cpp platform/audio.cpp "${GOW3_ROOT}/src/runtime_pad.c"
      "${GOW3_ROOT}/src/runtime_audio.c" "${GOW3_ROOT}/src/runtime_ajm.c"
      "${GOW3_ROOT}/src/runtime_mp3.c" "${GOW3_ROOT}/src/runtime_videodec.c"
      "${GOW3_REFERENCE_ROOT}/.local/kyty-on-jetsku/src/uwp/src/desktopShims.cpp")
    file(GLOB GOW3_ATRAC9 "${GOW3_ROOT}/third_party/LibAtrac9/C/src/*.c")
    add_library(gow3_atrac9 STATIC ${GOW3_ATRAC9})
    set_source_files_properties("${GOW3_ROOT}/src/runtime_ajm.c" PROPERTIES COMPILE_DEFINITIONS COMPILING_DLL)
    target_link_libraries(gow3_uwp PRIVATE gow3gpu gow3_atrac9 PkgConfig::SDL3 PkgConfig::FFMPEG xaudio2)
    target_compile_definitions(gow3gpu PRIVATE VK_USE_PLATFORM_WIN32_KHR)
  endif()
  set(GOW3_DOZEN_ROOT "${GOW3_MESA_ROOT}/build/dozen-uwp")
  add_custom_command(TARGET gow3_uwp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GOW3_LAYOUT}/ThirdParty"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GOW3_DOZEN_ROOT}/src/microsoft/vulkan/vulkan_dzn.dll" "${GOW3_LAYOUT}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GOW3_MESA_ROOT}/tools/dxc/bin/x64/dxil.dll" "${GOW3_LAYOUT}"
    COMMAND ${CMAKE_COMMAND} -E copy "${GOW3_MESA_ROOT}/mesa/licenses/MIT" "${GOW3_LAYOUT}/ThirdParty/Mesa-MIT.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${GOW3_MESA_ROOT}/tools/dxc/LICENCE-MIT.txt" "${GOW3_MESA_ROOT}/tools/dxc/LICENSE-LLVM.txt" "${GOW3_MESA_ROOT}/tools/dxc/LICENSE-MS.txt" "${GOW3_LAYOUT}/ThirdParty")
endif()
add_custom_command(TARGET gow3_uwp POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${GOW3_LAYOUT}/Assets" "${GOW3_LAYOUT}/Prepared" "${GOW3_LAYOUT}/ThirdParty"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:gow3_uwp>" "${GOW3_LAYOUT}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GOW3_PTHREAD}/libwinpthread-1.dll" "${GOW3_LAYOUT}"
  COMMAND ${CMAKE_COMMAND} -E copy "${GOW3_PTHREAD}/COPYING" "${GOW3_LAYOUT}/ThirdParty/winpthread-COPYING.txt"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_LIST_DIR}/AppxManifest.xml" "${GOW3_LAYOUT}"
  COMMAND ${CMAKE_COMMAND} -E copy "${GOW3_PREPARED}/boot-linked.bin" "${GOW3_PREPARED}/content.bin" "${GOW3_LAYOUT}/Prepared")
