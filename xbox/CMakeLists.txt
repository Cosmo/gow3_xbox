cmake_minimum_required(VERSION 3.20)
project(gow3_uwp LANGUAGES C CXX)
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
set(GOW3_DEPS_ROOT "${CMAKE_CURRENT_LIST_DIR}/.local/deps" CACHE PATH "Pinned third-party dependencies (scripts/build-deps.ps1)")
set(GOW3_PTHREAD "${GOW3_DEPS_ROOT}/winpthreads" CACHE PATH "UCRT winpthread headers and import library")
# platform/compat/unistd.h owns mode_t; winpthreads 14 would declare its own, narrower one.
add_compile_definitions(_MODE_T_)
# The app prepares the game files from the player's dump at run time; only their format is built in.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/prepared-format.txt" GOW3_PREPARED_FORMAT LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/prepared-format.txt")
set(GOW3_UWP_SOURCES
  runtime runtime_content runtime_file runtime_kernel runtime_memory runtime_mutex
  runtime_rtc runtime_rwlock runtime_savedata runtime_sema runtime_services runtime_thread runtime_time
  compat_win cpu_check isa_emulate isa_rewrite probe)
list(TRANSFORM GOW3_UWP_SOURCES PREPEND "${GOW3_ROOT}/src/")
list(TRANSFORM GOW3_UWP_SOURCES APPEND ".c")
add_executable(gow3_uwp WIN32 platform/main.cpp platform/prepare_game.cpp platform/runtime_compat.c platform/milestone_services.c ${GOW3_UWP_SOURCES})
target_include_directories(gow3_uwp PRIVATE "${GOW3_ROOT}" "${GOW3_ROOT}/src" platform platform/compat "${GOW3_PTHREAD}/include")
target_compile_definitions(gow3_uwp PRIVATE GOW3_UWP NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _TIMESPEC_DEFINED
  GOW3_PREPARED_FORMAT="${GOW3_PREPARED_FORMAT}")
target_compile_options(gow3_uwp PRIVATE /W3 $<$<COMPILE_LANGUAGE:C>:/clang:-Wno-deprecated-declarations> $<$<COMPILE_LANGUAGE:C>:/clang:-masm=att> $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
set_source_files_properties("${GOW3_ROOT}/src/isa_emulate.c" PROPERTIES COMPILE_OPTIONS "/clang:-mgeneral-regs-only")
target_link_options(gow3_uwp PRIVATE /APPCONTAINER /HIGHENTROPYVA:NO /STACK:16777216,16777216 /OPT:REF)
target_link_directories(gow3_uwp BEFORE PRIVATE "$ENV{VCToolsInstallDir}/lib/x64/store")
target_link_libraries(gow3_uwp PRIVATE WindowsApp "${GOW3_PTHREAD}/lib/libwinpthread.dll.a")
set(GOW3_LAYOUT "${CMAKE_BINARY_DIR}/layout")
if(GOW3_BUILD_GRAPHICS)
  add_compile_options(/clang:-Wno-unused-command-line-argument)
  set(GOW3_UWP ON)
  set(SPARTAN_LTO OFF CACHE BOOL "" FORCE)
  add_subdirectory("${GOW3_ROOT}/gpu" "${CMAKE_BINARY_DIR}/gpu")
  add_executable(uwp-dialog-input-test EXCLUDE_FROM_ALL ../tests/test_uwp_dialog_input.cpp graphics/window.cpp)
  target_include_directories(uwp-dialog-input-test PRIVATE "${GOW3_ROOT}/gpu/shim" "${GOW3_ROOT}/gpu/shadps4")
  target_compile_definitions(uwp-dialog-input-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(uwp-dialog-input-test PRIVATE Vulkan::Headers PkgConfig::SDL3)
  target_sources(spartanportgpu PRIVATE graphics/overlay.cpp graphics/texture_probe.cpp)
  target_sources(gow3_uwp PRIVATE graphics/probe.cpp graphics/present_bridge.cpp graphics/descriptor_fallback.cpp)
  target_include_directories(gow3_uwp PRIVATE "${GOW3_ROOT}/gpu/shadps4")
  target_compile_definitions(gow3_uwp PRIVATE GOW3_BUILD_GRAPHICS VK_USE_PLATFORM_WIN32_KHR)
  target_link_libraries(gow3_uwp PRIVATE Vulkan::Headers d3d12 dxgi)
  if(GOW3_LINK_RENDERER)
    target_compile_definitions(gow3_uwp PRIVATE GOW3_LINK_RENDERER)
    target_sources(gow3_uwp PRIVATE platform/input.cpp platform/audio.cpp "${GOW3_ROOT}/src/runtime_pad.c"
      "${GOW3_ROOT}/src/runtime_audio.c" "${GOW3_ROOT}/src/runtime_ajm.c"
      "${GOW3_ROOT}/src/runtime_mp3.c" "${GOW3_ROOT}/src/runtime_videodec.c"
      platform/desktop_shims.cpp)
    file(GLOB SPARTAN_ATRAC9 "${GOW3_ROOT}/third_party/LibAtrac9/C/src/*.c")
    add_library(gow3_atrac9 STATIC ${SPARTAN_ATRAC9})
    set_source_files_properties("${GOW3_ROOT}/src/runtime_ajm.c" PROPERTIES COMPILE_DEFINITIONS COMPILING_DLL)
    target_link_libraries(gow3_uwp PRIVATE spartanportgpu gow3_atrac9 PkgConfig::SDL3 PkgConfig::FFMPEG xaudio2)
    target_compile_definitions(spartanportgpu PRIVATE VK_USE_PLATFORM_WIN32_KHR)
  endif()
  # Dozen comes from the Mesa fork's UWP build (a local checkout, or its release on GitHub), dxil.dll from DXC.
  set(GOW3_DOZEN_DLL "${GOW3_MESA_ROOT}/build/dozen-uwp/src/microsoft/vulkan/vulkan_dzn.dll" CACHE FILEPATH "Dozen built for UWP")
  set(GOW3_MESA_LICENSE "${GOW3_MESA_ROOT}/mesa/licenses/MIT" CACHE FILEPATH "Mesa's MIT license")
  set(GOW3_DXC_ROOT "${GOW3_MESA_ROOT}/tools/dxc" CACHE PATH "DirectX Shader Compiler release (bin/x64/dxil.dll and licenses)")
  # The license of every library in the package (CREDITS.md lists them).
  set(GOW3_LICENSE_COMMANDS)
  foreach(entry IN ITEMS
      "${GOW3_MESA_LICENSE}|Mesa-MIT.txt"
      "${GOW3_DXC_ROOT}/LICENCE-MIT.txt|DXC-LICENCE-MIT.txt" "${GOW3_DXC_ROOT}/LICENSE-LLVM.txt|DXC-LICENSE-LLVM.txt"
      "${GOW3_DXC_ROOT}/LICENSE-MS.txt|DXC-LICENSE-MS.txt"
      "${GOW3_ROOT}/gpu/shadps4/LICENSE|shadPS4-GPL-2.0.txt"
      "${GOW3_DEPS_ROOT}/licenses/ffmpeg.txt|FFmpeg-LGPL-2.1.txt" "${GOW3_DEPS_ROOT}/licenses/sdl.txt|SDL3-zlib.txt"
      "${GOW3_DEPS_ROOT}/licenses/fmt.txt|fmt-MIT.txt" "${GOW3_DEPS_ROOT}/licenses/zydis.txt|Zydis-MIT.txt"
      "${GOW3_DEPS_ROOT}/licenses/zycore.txt|Zycore-MIT.txt" "${GOW3_DEPS_ROOT}/licenses/xxhash.txt|xxHash-BSD-2.txt"
      "${GOW3_DEPS_ROOT}/licenses/vma.txt|VulkanMemoryAllocator-MIT.txt"
      "${GOW3_DEPS_ROOT}/licenses/vulkan-headers.txt|Vulkan-Headers.txt"
      "${GOW3_DEPS_ROOT}/licenses/robin-map.txt|robin-map-MIT.txt" "${GOW3_DEPS_ROOT}/boost/licenses/config.txt|Boost-BSL-1.0.txt"
      "${GOW3_ROOT}/third_party/LibAtrac9/LICENSE|LibAtrac9-MIT.txt" "${GOW3_ROOT}/gpu/third_party/imgui/LICENSE.txt|imgui-MIT.txt"
      "${GOW3_ROOT}/gpu/third_party/miniz/LICENSE|miniz-MIT.txt" "${GOW3_ROOT}/gpu/third_party/magic_enum/LICENSE|magic_enum-MIT.txt"
      "${GOW3_ROOT}/gpu/third_party/half/LICENSE.txt|half-MIT.txt" "${GOW3_ROOT}/gpu/third_party/sirit/LICENSE.txt|sirit-BSD-3.txt"
      "${GOW3_ROOT}/gpu/third_party/xbyak/COPYRIGHT|xbyak-BSD-3.txt"
      "${GOW3_ROOT}/gpu/third_party/fsr-vulkan/LICENSE.txt|fsr-vulkan-MIT.txt"
      "${GOW3_ROOT}/gpu/third_party/fonts/LICENSE-DejaVu.txt|DejaVu-fonts.txt")
    string(REPLACE "|" ";" pair "${entry}")
    list(GET pair 0 source)
    list(GET pair 1 name)
    list(APPEND GOW3_LICENSE_COMMANDS COMMAND ${CMAKE_COMMAND} -E copy_if_different "${source}" "${GOW3_LAYOUT}/ThirdParty/${name}")
  endforeach()
  add_custom_command(TARGET gow3_uwp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GOW3_LAYOUT}/ThirdParty"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GOW3_DOZEN_DLL}" "${GOW3_LAYOUT}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GOW3_DXC_ROOT}/bin/x64/dxil.dll" "${GOW3_LAYOUT}"
    ${GOW3_LICENSE_COMMANDS})
endif()
add_custom_command(TARGET gow3_uwp POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${GOW3_LAYOUT}/Assets" "${GOW3_LAYOUT}/ThirdParty" "${GOW3_LAYOUT}/Patches"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GOW3_ROOT}/patches/CUSA01623.xml" "${GOW3_LAYOUT}/Patches"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:gow3_uwp>" "${GOW3_LAYOUT}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GOW3_DEPS_ROOT}/libwinpthread/bin/libwinpthread-1.dll" "${GOW3_LAYOUT}"
  COMMAND ${CMAKE_COMMAND} -E copy "${GOW3_PTHREAD}/share/licenses/winpthreads/COPYING" "${GOW3_LAYOUT}/ThirdParty/winpthread-COPYING.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GOW3_ROOT}/LICENSE" "${GOW3_LAYOUT}/LICENSE.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_LIST_DIR}/CREDITS.md" "${GOW3_LAYOUT}"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_LIST_DIR}/AppxManifest.xml" "${GOW3_LAYOUT}")
