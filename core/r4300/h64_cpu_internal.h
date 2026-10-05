// Harissa64 V2 - helpers shared by the interpreter's source files.
#ifndef H64_CPU_INTERNAL_H
#define H64_CPU_INTERNAL_H

#include "h64_cpu.h"
#include "../system/h64_system.h"

#define SEXT32(x) ((u64)(s64)(s32)(u32)(x))
#define RS(op) (((op) >> 21) & 31)
#define RT(op) (((op) >> 16) & 31)
#define RD(op) (((op) >> 11) & 31)
#define SA(op) (((op) >> 6) & 31)
#define FUNCT(op) ((op) & 63)
#define IMM16(op) ((u64)(s64)(s16)(u16)(op))
#define IMMU16(op) ((u64)(u16)(op))

// Raises an exception for the current instruction (EPC/BD from curPc).
// `vectorOffset` is 0x000 (TLB refill), 0x080 (XTLB refill) or 0x180.
void h64_cpu_exception(H64Cpu *cpu, int code, u32 vectorOffset);
void h64_cpu_exception_cop(H64Cpu *cpu, int copNumber);   // coprocessor unusable

// Virtual memory access with all checks. Return 0 on success; on failure the
// exception has been raised and the instruction must stop.
int h64_cpu_read8(H64System *sys, u64 vaddr, u8 *v);
int h64_cpu_read16(H64System *sys, u64 vaddr, u16 *v);
int h64_cpu_read32(H64System *sys, u64 vaddr, u32 *v);
int h64_cpu_read64(H64System *sys, u64 vaddr, u64 *v);
int h64_cpu_write8(H64System *sys, u64 vaddr, u8 v);
int h64_cpu_write16(H64System *sys, u64 vaddr, u16 v);
int h64_cpu_write32(H64System *sys, u64 vaddr, u32 v);
int h64_cpu_write64(H64System *sys, u64 vaddr, u64 v);

// COP1 (h64_fpu.cpp).
void h64_cop1_execute(H64System *sys, u32 op);
u32 h64_fpr_get32(H64Cpu *cpu, int n);
void h64_fpr_set32(H64Cpu *cpu, int n, u32 v);
u64 h64_fpr_get64(H64Cpu *cpu, int n);
void h64_fpr_set64(H64Cpu *cpu, int n, u64 v);

#endif
