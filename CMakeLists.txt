cmake_minimum_required(VERSION 3.24)
project(gtaiv_fpv CXX)

# Build with: cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-i686.cmake
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "GTA IV is 32-bit: configure with cmake/mingw-w64-i686.cmake")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(LIBFPV_DIR "$ENV{HOME}/Projects/libfpv" CACHE PATH "libfpv source checkout")
set(LIBFPV_BUILD "${LIBFPV_DIR}/build/win32" CACHE PATH "libfpv i686 build dir")
# Kamikaze builds of libfpv (dev only, see libfpv INTEGRATION.md "Kamikaze
# builds"). libfpv.h changes fpv_frame's layout with LIBFPV_KAMIKAZE (12 radio
# channels instead of 10), so the mod must be built for the libfpv.dll it
# loads: ON only with a -DLIBFPV_KAMIKAZE=ON libfpv. The core refuses a
# libfpv.dll of the other kind (src/fpvapi.cpp).
# On a kamikaze dev machine (LIBFPV_KAMIKAZE_DEV=1 or true, libfpv CLAUDE.md)
# local builds default to ON; CI never sets the variable.
set(_kamikaze_default OFF)
if("$ENV{LIBFPV_KAMIKAZE_DEV}" STREQUAL "1" OR "$ENV{LIBFPV_KAMIKAZE_DEV}" STREQUAL "true")
  set(_kamikaze_default ON)
endif()
option(GTAIV_FPV_KAMIKAZE "Build for a kamikaze libfpv.dll (dev only)" ${_kamikaze_default})

# The files are named after libfpv (libfpv docs/INTEGRATION.md "Label the mod
# libfpv inside the game"): GTAIV/libfpv.asi, and GTAIV/libfpv/libfpv_gtaiv.dll.

# libfpv.asi: loader that owns the ScriptHook thread and hot-reloads the core.
add_library(gtaiv_fpv_loader SHARED
  src/loader.cpp
  src/scripthook.cpp
  src/log.cpp
  src/paths.cpp
)
set_target_properties(gtaiv_fpv_loader PROPERTIES PREFIX "" OUTPUT_NAME "libfpv" SUFFIX ".asi")

# libfpv_gtaiv.dll: the mod itself.
add_library(gtaiv_fpv_core SHARED
  src/core.cpp
  src/audio.cpp
  src/autotest.cpp
  src/damage.cpp
  src/drone.cpp
  src/effects.cpp
  src/fpvapi.cpp
  src/kamikaze.cpp
  src/pilot.cpp
  src/recon.cpp
  src/render.cpp
  src/video.cpp
  src/wind.cpp
  src/world.cpp
  src/scripthook.cpp
  src/log.cpp
  src/paths.cpp
)
set_target_properties(gtaiv_fpv_core PROPERTIES PREFIX "" OUTPUT_NAME "libfpv_gtaiv")
# The mod's version and commit for libfpv's F8 > About tab (src/fpvapi.cpp).
set(GTAIV_FPV_BUILD_INFO "${CMAKE_BINARY_DIR}/generated/build_info.h")
add_custom_target(gtaiv_fpv_build_info
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${GTAIV_FPV_BUILD_INFO} -P ${CMAKE_SOURCE_DIR}/cmake/build_info.cmake
  BYPRODUCTS ${GTAIV_FPV_BUILD_INFO}
  COMMENT "Build info for the About tab")
add_dependencies(gtaiv_fpv_core gtaiv_fpv_build_info)
target_include_directories(gtaiv_fpv_core PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(gtaiv_fpv_core PRIVATE winmm d3d9)
# The core appends to the log the loader started.
target_compile_definitions(gtaiv_fpv_core PRIVATE FPVLOG_APPEND)

foreach(target gtaiv_fpv_loader gtaiv_fpv_core)
  # docs/renderers: fpv_video_d3d9.h, the video feed's renderer (src/video.cpp)
  target_include_directories(${target} PRIVATE src "${LIBFPV_DIR}/include" "${LIBFPV_DIR}/docs/renderers")
  # libfpv.dll is loaded at runtime; the header is only used for declarations.
  target_compile_definitions(${target} PRIVATE LIBFPV_STATIC)
  if(GTAIV_FPV_KAMIKAZE)
    target_compile_definitions(${target} PRIVATE LIBFPV_KAMIKAZE)
  endif()
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_options(${target} PRIVATE -static)
endforeach()
