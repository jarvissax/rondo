// Include Android 4.2.2's private headers between aosp_begin.h and aosp_end.h.
//
// The Y1's libraries were built before C++11: their headers define char16_t and char32_t as typedefs
// of uint16_t and uint32_t (utils/Unicode.h), and every exported function that takes them is named
// with those integer types. Mapping the two names to the integer types while these headers are read
// keeps both the headers compiling and the names matching the firmware's libraries. Include every
// standard header you need before aosp_begin.h, since the mapping is in force until aosp_end.h.
// The standard headers the AOSP headers use, read before the mapping starts:
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#pragma push_macro("char16_t")
#pragma push_macro("char32_t")
#define char16_t uint16_t
#define char32_t uint32_t
