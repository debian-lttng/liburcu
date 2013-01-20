#ifndef _URCU_QSBR_STATIC_H
#define _URCU_QSBR_STATIC_H

/*
 * urcu-qsbr-static.h
 *
 * Userspace RCU QSBR header.
 *
 * TO BE INCLUDED ONLY IN CODE THAT IS TO BE RECOMPILED ON EACH LIBURCU
 * RELEASE. See urcu.h for linking dynamically with the userspace rcu library.
 *
 * Copyright (c) 2009 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 * Copyright (c) 2009 Paul E. McKenney, IBM Corporation.
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
 *
 * IBM's contributions to this file may be relicensed under LGPLv2 or later.
 */

#include <stdlib.h>
#include <pthread.h>
#include <assert.h>
#include <limits.h>
#include <unistd.h>
#include <stdint.h>

#include <urcu/compiler.h>
#include <urcu/arch.h>
#include <urcu/system.h>
#include <urcu/uatomic.h>
#include <urcu/list.h>
#include <urcu/futex.h>
#include <urcu/tls-compat.h>

#ifdef __cplusplus
extern "C" {
#endif 

/*
 * This code section can only be included in LGPL 2.1 compatible source code.
 * See below for the function call wrappers which can be used in code meant to
 * be only linked with the Userspace RCU library. This comes with a small
 * performance degradation on the read-side due to the added function calls.
 * This is required to permit relinking with newer versions of the library.
 */

#ifdef DEBUG_RCU
#define rcu_assert(args...)	assert(args)
#else
#define rcu_assert(args...)
#endif

#ifdef DEBUG_YIELD
#include <sched.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#define YIELD_READ 	(1 << 0)
#define YIELD_WRITE	(1 << 1)

/* maximum sleep delay, in us */
#define MAX_SLEEP 50

extern unsigned int yield_active;
extern DECLARE_URCU_TLS(unsigned int, rand_yield);

static inline void debug_yield_read(void)
{
	if (yield_active & YIELD_READ)
		if (rand_r(&URCU_TLS(rand_yield)) & 0x1)
			usleep(rand_r(&URCU_TLS(rand_yield)) % MAX_SLEEP);
}

static inline void debug_yield_write(void)
{
	if (yield_active & YIELD_WRITE)
		if (rand_r(&URCU_TLS(rand_yield)) & 0x1)
			usleep(rand_r(&URCU_TLS(rand_yield)) % MAX_SLEEP);
}

static inline void debug_yield_init(void)
{
	URCU_TLS(rand_yield) = time(NULL) ^ (unsigned long) pthread_self();
}
#else
static inline void debug_yield_read(void)
{
}

static inline void debug_yield_write(void)
{
}

static inline void debug_yield_init(void)
{

}
#endif

#define RCU_GP_ONLINE		(1UL << 0)
#define RCU_GP_CTR		(1UL << 1)

/*
 * Global quiescent period counter with low-order bits unused.
 * Using a int rather than a char to eliminate false register dependencies
 * causing stalls on some architectures.
 */
extern unsigned long rcu_gp_ctr;

struct rcu_reader {
	/* Data used by both reader and synchronize_rcu() */
	unsigned long ctr;
	/* Data used for registry */
	struct cds_list_head node __attribute__((aligned(CAA_CACHE_LINE_SIZE)));
	int waiting;
	pthread_t tid;
};

extern DECLARE_URCU_TLS(struct rcu_reader, rcu_reader);

extern int32_t gp_futex;

/*
 * Wake-up waiting synchronize_rcu(). Called from many concurrent threads.
 */
static inline void wake_up_gp(void)
{
	if (caa_unlikely(_CMM_LOAD_SHARED(URCU_TLS(rcu_reader).waiting))) {
		_CMM_STORE_SHARED(URCU_TLS(rcu_reader).waiting, 0);
		cmm_smp_mb();
		if (uatomic_read(&gp_futex) != -1)
			return;
		uatomic_set(&gp_futex, 0);
		futex_noasync(&gp_futex, FUTEX_WAKE, 1,
		      NULL, NULL, 0);
	}
}

static inline int rcu_gp_ongoing(unsigned long *ctr)
{
	unsigned long v;

	v = CMM_LOAD_SHARED(*ctr);
	return v && (v != rcu_gp_ctr);
}

/*
 * Enter an RCU read-side critical section.
 *
 * This function is less than 10 lines long.  The intent is that this
 * function meets the 10-line criterion for LGPL, allowing this function
 * to be invoked directly from non-LGPL code.
 */
static inline void _rcu_read_lock(void)
{
	rcu_assert(URCU_TLS(rcu_reader).ctr);
}

/*
 * Exit an RCU read-side critical section.
 *
 * This function is less than 10 lines long.  The intent is that this
 * function meets the 10-line criterion for LGPL, allowing this function
 * to be invoked directly from non-LGPL code.
 */
static inline void _rcu_read_unlock(void)
{
}

/*
 * Inform RCU of a quiescent state.
 *
 * This function is less than 10 lines long.  The intent is that this
 * function meets the 10-line criterion for LGPL, allowing this function
 * to be invoked directly from non-LGPL code.
 */
static inline void _rcu_quiescent_state(void)
{
	cmm_smp_mb();
	_CMM_STORE_SHARED(URCU_TLS(rcu_reader).ctr, _CMM_LOAD_SHARED(rcu_gp_ctr));
	cmm_smp_mb();	/* write URCU_TLS(rcu_reader).ctr before read futex */
	wake_up_gp();
	cmm_smp_mb();
}

/*
 * Take a thread offline, prohibiting it from entering further RCU
 * read-side critical sections.
 *
 * This function is less than 10 lines long.  The intent is that this
 * function meets the 10-line criterion for LGPL, allowing this function
 * to be invoked directly from non-LGPL code.
 */
static inline void _rcu_thread_offline(void)
{
	cmm_smp_mb();
	CMM_STORE_SHARED(URCU_TLS(rcu_reader).ctr, 0);
	cmm_smp_mb();	/* write URCU_TLS(rcu_reader).ctr before read futex */
	wake_up_gp();
	cmm_barrier();	/* Ensure the compiler does not reorder us with mutex */
}

/*
 * Bring a thread online, allowing it to once again enter RCU
 * read-side critical sections.
 *
 * This function is less than 10 lines long.  The intent is that this
 * function meets the 10-line criterion for LGPL, allowing this function
 * to be invoked directly from non-LGPL code.
 */
static inline void _rcu_thread_online(void)
{
	cmm_barrier();	/* Ensure the compiler does not reorder us with mutex */
	_CMM_STORE_SHARED(URCU_TLS(rcu_reader).ctr, CMM_LOAD_SHARED(rcu_gp_ctr));
	cmm_smp_mb();
}

#ifdef __cplusplus 
}
#endif

#endif /* _URCU_QSBR_STATIC_H */
