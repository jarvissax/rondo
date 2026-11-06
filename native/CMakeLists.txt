# Rondo's native code, built for the Y1 (Android 4.2.2, armeabi-v7a) with NDK r23c: tools/native/build.sh
cmake_minimum_required(VERSION 3.18)
project(rondo_native C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-fno-exceptions -fno-rtti -Wall -Wextra -Wno-unused-parameter)

# The Y1's own libraries and the AOSP 4.2.2 headers that describe them (tools/native/sysroot.sh)
set(Y1_SYSROOT ${CMAKE_CURRENT_SOURCE_DIR}/sysroot)
set(Y1_LIB ${Y1_SYSROOT}/system-lib)
foreach(lib gui ui binder utils cutils log hardware media stagefright stagefright_foundation surfaceflinger hardware_legacy netutils fmjni mtkbtextadpa2dp)
  add_library(y1_${lib} SHARED IMPORTED)
  set_target_properties(y1_${lib} PROPERTIES IMPORTED_LOCATION ${Y1_LIB}/lib${lib}.so)
endforeach()

# A target that talks to Android's private native libraries: the headers, the configuration those
# libraries were built with, and the firmware's libraries to link and resolve against.
function(y1_private target)
  target_include_directories(${target} SYSTEM PRIVATE
    ${Y1_SYSROOT}/aosp/native ${Y1_SYSROOT}/aosp/core ${Y1_SYSROOT}/aosp/hardware ${Y1_SYSROOT}/aosp/av
    ${Y1_SYSROOT}/aosp/hardware_legacy)
  target_compile_options(${target} PRIVATE -include ${Y1_SYSROOT}/aosp/config/AndroidConfig.h
    -Wno-deprecated-register -Wno-unused-private-field -Wno-overloaded-virtual)
  target_link_options(${target} PRIVATE -Wl,-rpath-link,${Y1_LIB})
endfunction()

add_subdirectory(tests)

# Tools that run on the Y1 (tools/perf drives them)
add_executable(inject tools/inject.cpp)

# Third-party libraries, built for the Y1 by tools/native/deps.sh
set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party/y1)
foreach(lib freetype harfbuzz png16 turbojpeg sqlite3 yyjson)
  add_library(tp_${lib} STATIC IMPORTED)
  set_target_properties(tp_${lib} PROPERTIES IMPORTED_LOCATION ${TP}/lib/lib${lib}.a)
endforeach()

# The core (N2): settings, the card library, later the server client
add_library(rondo_core STATIC core/config.cpp core/fold.cpp core/tags.cpp core/library.cpp)
target_include_directories(rondo_core SYSTEM PUBLIC ${TP}/include)
target_link_libraries(rondo_core PUBLIC tp_sqlite3 tp_yyjson dl)

add_executable(libtest apps/libtest/main.cpp)
target_link_libraries(libtest PRIVATE rondo_core)

# The UI engine (N1): portable C++ over GLES2
add_library(rondo_ui STATIC ui/gfx.cpp ui/text.cpp ui/image.cpp ui/frame_log.cpp ui/theme.cpp ui/keyboard.cpp core/cover.cpp)
target_include_directories(rondo_ui SYSTEM PUBLIC ${TP}/include ${TP}/include/freetype2 ${TP}/include/harfbuzz)
target_link_libraries(rondo_ui PUBLIC rondo_core tp_harfbuzz tp_freetype tp_png16 tp_turbojpeg z GLESv2 m)

# The Y1's platform layer: framebuffer, vsync, input
add_library(rondo_platform_y1 STATIC platform/y1/display.cpp platform/y1/input.cpp)
y1_private(rondo_platform_y1)
target_link_libraries(rondo_platform_y1 PUBLIC y1_ui y1_utils y1_cutils EGL GLESv2)

add_executable(uitest apps/uitest/main.cpp)
target_link_libraries(uitest PRIVATE rondo_ui rondo_platform_y1)
target_link_options(uitest PRIVATE -Wl,-rpath-link,${Y1_LIB})
add_executable(churn tools/churn.cpp)
