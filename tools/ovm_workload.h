/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _OVM_WORKLOAD_H
#define _OVM_WORKLOAD_H

#include <stdint.h>
#include <stddef.h>

#define OVM_HUGEPAGE_2MB_SIZE	(2ULL * 1024ULL * 1024ULL)
#define OVM_TOTAL_WORDS		(OVM_HUGEPAGE_2MB_SIZE / sizeof(uint64_t)) /* 262,144 64-bit words */

/*
 * 128-bit magic value (first two 64-bit words of 2MB page):
 *   Word 0 (64-bit): 0x4f525048414e564d ('ORPHANVM')
 *   Word 1 (64-bit): 0x574f524b (upper 32-bit 'WORK') | (cpu_id in lower 32-bit)
 */
#define OVM_MAGIC0		0x4f525048414e564dULL /* 'ORPHANVM' */
#define OVM_MAGIC1_BASE		0x574f524b00000000ULL /* 'WORK' in upper 32 bits */
#define OVM_MAGIC1_PREFIX_MASK	0xffffffff00000000ULL
#define OVM_MAGIC1_CPU_MASK	0x00000000ffffffffULL

#define OVM_MAGIC1(cpu_id)	(OVM_MAGIC1_BASE | ((uint64_t)(uint32_t)(cpu_id)))
#define OVM_MAGIC1_GET_CPU(m1)	((int)((uint32_t)((m1) & OVM_MAGIC1_CPU_MASK)))
#define OVM_MAGIC1_MATCH(m1)	(((m1) & OVM_MAGIC1_PREFIX_MASK) == OVM_MAGIC1_BASE)

#define OVM_DATA_START_IDX	2
#define OVM_DATA_WORDS_COUNT	(OVM_TOTAL_WORDS - OVM_DATA_START_IDX) /* 262,142 data words */

#endif /* _OVM_WORKLOAD_H */
