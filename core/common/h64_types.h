// Harissa64 V2 - basic types and platform detection.
//
// The core is portable C++ restricted to what the Xbox 360 SDK compiler
// (VS2010, cl 16.00) accepts: see "Core language subset" in docs/DEVELOPMENT.md.
#ifndef H64_TYPES_H
#define H64_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

// ---- Host byte order ----
// H64_HOST_BIG_ENDIAN is 1 on the Xbox 360 (Xenon) and on the big-endian
// PowerPC test build, 0 on x86/x64 hosts.
#if defined(_XBOX) || defined(__BIG_ENDIAN__) || \
    (defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define H64_HOST_BIG_ENDIAN 1
#else
#define H64_HOST_BIG_ENDIAN 0
#endif

// ---- Compiler helpers ----
#if defined(_MSC_VER)
#define H64_INLINE __forceinline
#define H64_NOINLINE __declspec(noinline)
#define H64_ALIGN(n) __declspec(align(n))
#else
#define H64_INLINE inline __attribute__((always_inline))
#define H64_NOINLINE __attribute__((noinline))
#define H64_ALIGN(n) __attribute__((aligned(n)))
#endif

#define H64_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

#endif
