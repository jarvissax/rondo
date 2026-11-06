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
