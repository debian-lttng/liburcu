#ifndef _URCU_ARCH_X86_H
#define _URCU_ARCH_X86_H

/*
 * arch_x86.h: trivial definitions for the x86 architecture.
 *
 * Copyright (c) 2009 Paul E. McKenney, IBM Corporation.
 * Copyright (c) 2009 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <urcu/compiler.h>
#include <urcu/config.h>

#ifdef __cplusplus
extern "C" {
#endif 

#define CAA_CACHE_LINE_SIZE	128

#ifdef CONFIG_RCU_HAVE_FENCE
#define cmm_mb()    asm volatile("mfence":::"memory")
#define cmm_rmb()   asm volatile("lfence":::"memory")
#define cmm_wmb()   asm volatile("sfence"::: "memory")
#else
/*
 * Some non-Intel clones support out of order store. cmm_wmb() ceases to be a
 * nop for these.
 */
#define cmm_mb()    asm volatile("lock; addl $0,0(%%esp)":::"memory")
#define cmm_rmb()   asm volatile("lock; addl $0,0(%%esp)":::"memory")
#define cmm_wmb()   asm volatile("lock; addl $0,0(%%esp)"::: "memory")
#endif

#define caa_cpu_relax()	asm volatile("rep; nop" : : : "memory");

#define rdtscll(val)							  \
	do {						  		  \
	     unsigned int __a, __d;					  \
	     asm volatile("rdtsc" : "=a" (__a), "=d" (__d));		  \
	     (val) = ((unsigned long long)__a)				  \
			| (((unsigned long long)__d) << 32);		  \
	} while(0)

typedef unsigned long long cycles_t;

static inline cycles_t caa_get_cycles(void)
{
        cycles_t ret = 0;

        rdtscll(ret);
        return ret;
}

#ifdef __cplusplus 
}
#endif

#include <urcu/arch/generic.h>

#endif /* _URCU_ARCH_X86_H */
