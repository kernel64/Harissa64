// Harissa64 V2 - endian-aware memory access.
//
// The N64 is big-endian. Guest memory (RDRAM, ROM, DMEM/IMEM, PIF RAM...) is
// stored in the core as plain byte arrays in N64 byte order, so a guest byte
// address is a host byte offset on every target. All multi-byte guest
// accesses go through the helpers below; no raw casts of guest memory
// pointers anywhere else (rule 6 of the V2 plan).
//
// Two implementations:
//  - the reference one (h64_ref_*) assembles bytes one by one and is correct
//    on any host;
//  - the fast one (h64_load_be* / h64_store_be*) uses a native access on
//    big-endian hosts and a byte swap on little-endian hosts.
// Unit tests check that both agree.
#ifndef H64_ENDIAN_H
#define H64_ENDIAN_H

#include <string.h>
#include "h64_types.h"

// ---- Byte swaps ----
static H64_INLINE u16 h64_bswap16(u16 v) { return (u16)((v >> 8) | (v << 8)); }
static H64_INLINE u32 h64_bswap32(u32 v)
{
    return (v >> 24) | ((v >> 8) & 0x0000FF00u) | ((v << 8) & 0x00FF0000u) | (v << 24);
}
static H64_INLINE u64 h64_bswap64(u64 v)
{
    return ((u64)h64_bswap32((u32)v) << 32) | h64_bswap32((u32)(v >> 32));
}

// ---- Reference implementation (byte by byte) ----
static H64_INLINE u16 h64_ref_load_be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static H64_INLINE u32 h64_ref_load_be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}
static H64_INLINE u64 h64_ref_load_be64(const u8 *p)
{
    return ((u64)h64_ref_load_be32(p) << 32) | h64_ref_load_be32(p + 4);
}
static H64_INLINE void h64_ref_store_be16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static H64_INLINE void h64_ref_store_be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}
static H64_INLINE void h64_ref_store_be64(u8 *p, u64 v)
{
    h64_ref_store_be32(p, (u32)(v >> 32));
    h64_ref_store_be32(p + 4, (u32)v);
}

// ---- Fast implementation ----
// memcpy of a constant size compiles to a single load/store on every
// compiler we use, and is legal for unaligned addresses.
static H64_INLINE u16 h64_load_be16(const u8 *p)
{
    u16 v; memcpy(&v, p, 2);
#if H64_HOST_BIG_ENDIAN
    return v;
#else
    return h64_bswap16(v);
#endif
}
static H64_INLINE u32 h64_load_be32(const u8 *p)
{
    u32 v; memcpy(&v, p, 4);
#if H64_HOST_BIG_ENDIAN
    return v;
#else
    return h64_bswap32(v);
#endif
}
static H64_INLINE u64 h64_load_be64(const u8 *p)
{
    u64 v; memcpy(&v, p, 8);
#if H64_HOST_BIG_ENDIAN
    return v;
#else
    return h64_bswap64(v);
#endif
}
static H64_INLINE void h64_store_be16(u8 *p, u16 v)
{
#if !H64_HOST_BIG_ENDIAN
    v = h64_bswap16(v);
#endif
    memcpy(p, &v, 2);
}
static H64_INLINE void h64_store_be32(u8 *p, u32 v)
{
#if !H64_HOST_BIG_ENDIAN
    v = h64_bswap32(v);
#endif
    memcpy(p, &v, 4);
}
static H64_INLINE void h64_store_be64(u8 *p, u64 v)
{
#if !H64_HOST_BIG_ENDIAN
    v = h64_bswap64(v);
#endif
    memcpy(p, &v, 8);
}

#endif
