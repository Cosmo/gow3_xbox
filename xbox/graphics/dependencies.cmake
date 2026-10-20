# Third-party code of the UWP renderer, at the versions xbox/scripts/build-deps.ps1 fetches and builds
# into GOW3_DEPS_ROOT (fmt, Zydis, SDL and xxHash compile here; glslang and FFmpeg are prebuilt there).
set(gow3_deps "${GOW3_DEPS_ROOT}")
foreach(required ffmpeg-install/lib/libavcodec.a glslang-install/bin/glslang.exe fmt/CMakeLists.txt sdl/CMakeLists.txt boost/include)
  if(NOT EXISTS "${gow3_deps}/${required}")
    message(FATAL_ERROR "${gow3_deps}/${required} is missing: run xbox/scripts/build-deps.ps1 first")
  endif()
endforeach()

# The UWP targets link without the desktop default libraries; the dependencies' own configure
# checks still need them to link their test programs.
set(gow3_standard_c "${CMAKE_C_STANDARD_LIBRARIES}")
set(gow3_standard_cxx "${CMAKE_CXX_STANDARD_LIBRARIES}")
set(CMAKE_C_STANDARD_LIBRARIES "${CMAKE_C_STANDARD_LIBRARIES_INIT}")
set(CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_CXX_STANDARD_LIBRARIES_INIT}")

set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
set(FMT_DOC OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
# fmt builds a C++20 module with Ninja 1.11+, which clang-cl cannot scan; the launcher uses the headers.
set(FMT_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory("${gow3_deps}/fmt" "${CMAKE_BINARY_DIR}/deps/fmt" EXCLUDE_FROM_ALL)

set(ZYAN_ZYCORE_PATH "${gow3_deps}/zycore" CACHE PATH "" FORCE)
foreach(option ZYDIS_BUILD_SHARED_LIB ZYDIS_BUILD_EXAMPLES ZYDIS_BUILD_TOOLS ZYDIS_BUILD_TESTS ZYDIS_BUILD_MAN
               ZYDIS_BUILD_DOXYGEN ZYCORE_BUILD_SHARED_LIB ZYCORE_BUILD_EXAMPLES ZYCORE_BUILD_TESTS)
  set(${option} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("${gow3_deps}/zydis" "${CMAKE_BINARY_DIR}/deps/zydis" EXCLUDE_FROM_ALL)

# SDL only supplies events and gamepad mapping helpers here; the app has its own window, audio and input.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
foreach(option SDL_TEST_LIBRARY SDL_TESTS SDL_EXAMPLES SDL_INSTALL SDL_UNINSTALL SDL_AUDIO SDL_VIDEO SDL_JOYSTICK
               SDL_HAPTIC SDL_HIDAPI SDL_SENSOR SDL_POWER SDL_DIALOG SDL_FILESYSTEM SDL_LOCALE SDL_MISC SDL_TRAY
               SDL_PTHREADS SDL_CLOCK_GETTIME SDL_LIBICONV SDL_SYSTEM_ICONV SDL_OPENVR SDL_PIPEWIRE)
  set(${option} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("${gow3_deps}/sdl" "${CMAKE_BINARY_DIR}/deps/sdl" EXCLUDE_FROM_ALL)
add_library(PkgConfig::SDL3 ALIAS SDL3-static)

set(CMAKE_C_STANDARD_LIBRARIES "${gow3_standard_c}")
set(CMAKE_CXX_STANDARD_LIBRARIES "${gow3_standard_cxx}")

add_library(Boost::headers INTERFACE IMPORTED GLOBAL)
set_target_properties(Boost::headers PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${gow3_deps}/boost/include")
add_library(tsl::robin_map INTERFACE IMPORTED GLOBAL)
set_target_properties(tsl::robin_map PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${gow3_deps}/robin-map/include")
add_library(GPUOpen::VulkanMemoryAllocator INTERFACE IMPORTED GLOBAL)
set_target_properties(GPUOpen::VulkanMemoryAllocator PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${gow3_deps}/vma/include")
add_library(Vulkan::Headers INTERFACE IMPORTED GLOBAL)
set_target_properties(Vulkan::Headers PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${gow3_deps}/vulkan-headers/include")
set(GLSLANG_COMPILER "${gow3_deps}/glslang-install/bin/glslang.exe")
set(gow3_ffmpeg "${gow3_deps}/ffmpeg-install")
add_library(PkgConfig::FFMPEG INTERFACE IMPORTED GLOBAL)
set_target_properties(PkgConfig::FFMPEG PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${gow3_ffmpeg}/include" INTERFACE_LINK_LIBRARIES "${gow3_ffmpeg}/lib/libavformat.a;${gow3_ffmpeg}/lib/libavcodec.a;${gow3_ffmpeg}/lib/libavutil.a;${gow3_ffmpeg}/lib/libswscale.a;${gow3_ffmpeg}/lib/libswresample.a")
add_library(gow3_xxhash STATIC "${gow3_deps}/xxhash/xxhash.c")
target_include_directories(gow3_xxhash PUBLIC "${gow3_deps}/xxhash")
add_library(PkgConfig::XXHASH ALIAS gow3_xxhash)
set(GOW3_UWP_DEPENDENCIES_READY TRUE)
