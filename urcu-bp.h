#ifndef _URCU_BP_H
#define _URCU_BP_H

/*
 * urcu-bp.h
 *
 * Userspace RCU header, "bulletproof" version.
 *
 * Slower RCU read-side adapted for tracing library. Does not require thread
 * registration nor unregistration. Also signal-safe.
 *
 * Copyright (c) 2009 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 * Copyright (c) 2009 Paul E. McKenney, IBM Corporation.
 *
 * LGPL-compatible code should include this header with :
 *
 * #define _LGPL_SOURCE
 * #include <urcu.h>
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

/*
 * See urcu-pointer.h and urcu/static/urcu-pointer.h for pointer
 * publication headers.
 */
#include <urcu-pointer.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <urcu/map/urcu-bp.h>

/*
 * Important !
 *
 * Each thread containing read-side critical sections must be registered
 * with rcu_register_thread() before calling rcu_read_lock().
 * rcu_unregister_thread() should be called before the thread exits.
 */

#ifdef _LGPL_SOURCE

#include <urcu/static/urcu-bp.h>

/*
 * Mappings for static use of the userspace RCU library.
 * Should only be used in LGPL-compatible code.
 */

/*
 * rcu_read_lock()
 * rcu_read_unlock()
 *
 * Mark the beginning and end of a read-side critical section.
 */
#define rcu_read_lock_bp		_rcu_read_lock
#define rcu_read_unlock_bp		_rcu_read_unlock

#else /* !_LGPL_SOURCE */

/*
 * library wrappers to be used by non-LGPL compatible source code.
 * See LGPL-only urcu/static/urcu-pointer.h for documentation.
 */

extern void rcu_read_lock(void);
extern void rcu_read_unlock(void);

#endif /* !_LGPL_SOURCE */

extern void synchronize_rcu(void);

/*
 * rcu_bp_before_fork, rcu_bp_after_fork_parent and rcu_bp_after_fork_child
 * should be called around fork() system calls when the child process is not
 * expected to immediately perform an exec(). For pthread users, see
 * pthread_atfork(3).
 */
extern void rcu_bp_before_fork(void);
extern void rcu_bp_after_fork_parent(void);
extern void rcu_bp_after_fork_child(void);

/*
 * In the bulletproof version, the following functions are no-ops.
 */
static inline void rcu_register_thread(void)
{
}

static inline void rcu_unregister_thread(void)
{
}

static inline void rcu_init(void)
{
}

#ifdef __cplusplus 
}
#endif

#include <urcu-call-rcu.h>
#include <urcu-defer.h>

#endif /* _URCU_BP_H */
