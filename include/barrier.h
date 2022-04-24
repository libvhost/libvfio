/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   Copyright (c) 2017, IBM Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** \file
 * Memory barriers
 */

#ifndef PROPIG_BARRIER_H
#define PROPIG_BARRIER_H

#ifdef __cplusplus
extern "C" {
#endif

/** Compiler memory barrier */
#define propig_compiler_barrier() __asm volatile("" ::: "memory")

/** Read memory barrier */
#define propig_rmb() _propig_rmb()

/** Write memory barrier */
#define propig_wmb() _propig_wmb()

/** Full read/write memory barrier */
#define propig_mb() _propig_mb()

/** SMP read memory barrier. */
#define propig_smp_rmb() _propig_smp_rmb()

/** SMP write memory barrier. */
#define propig_smp_wmb() _propig_smp_wmb()

/** SMP read/write memory barrier. */
#define propig_smp_mb() _propig_smp_mb()

#ifdef __PPC64__

#define _propig_rmb() __asm volatile("sync" ::: "memory")
#define _propig_wmb() __asm volatile("sync" ::: "memory")
#define _propig_mb() __asm volatile("sync" ::: "memory")
#define _propig_smp_rmb() __asm volatile("lwsync" ::: "memory")
#define _propig_smp_wmb() __asm volatile("lwsync" ::: "memory")
#define _propig_smp_mb() propig_mb()

#elif defined(__aarch64__)

#define _propig_rmb() __asm volatile("dsb ld" ::: "memory")
#define _propig_wmb() __asm volatile("dsb st" ::: "memory")
#define _propig_mb() __asm volatile("dsb sy" ::: "memory")
#define _propig_smp_rmb() __asm volatile("dmb ishld" ::: "memory")
#define _propig_smp_wmb() __asm volatile("dmb ishst" ::: "memory")
#define _propig_smp_mb() __asm volatile("dmb ish" ::: "memory")

#elif defined(__i386__) || defined(__x86_64__)

#define _propig_rmb() __asm volatile("lfence" ::: "memory")
#define _propig_wmb() __asm volatile("sfence" ::: "memory")
#define _propig_mb() __asm volatile("mfence" ::: "memory")
#define _propig_smp_rmb() propig_compiler_barrier()
#define _propig_smp_wmb() propig_compiler_barrier()
#if defined(__x86_64__)
#define _propig_smp_mb()                                                       \
  __asm volatile("lock addl $0, -128(%%rsp); " ::: "memory");
#elif defined(__i386__)
#define _propig_smp_mb()                                                       \
  __asm volatile("lock addl $0, -128(%%esp); " ::: "memory");
#endif

#else

#define _propig_rmb()
#define _propig_wmb()
#define _propig_mb()
#define _propig_smp_rmb()
#define _propig_smp_wmb()
#define _propig_smp_mb()
#error Unknown architecture

#endif

#ifdef __cplusplus
}
#endif

#endif
