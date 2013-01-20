/*
 * urcu-call-rcu.c
 *
 * Userspace RCU library - batch memory reclamation with kernel API
 *
 * Copyright (c) 2010 Paul E. McKenney <paulmck@linux.vnet.ibm.com>
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

#define _GNU_SOURCE
#include <stdio.h>
#include <pthread.h>
#include <signal.h>
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <sys/time.h>
#include <unistd.h>
#include <sched.h>

#include "config.h"
#include "urcu/wfqueue.h"
#include "urcu-call-rcu.h"
#include "urcu-pointer.h"
#include "urcu/list.h"
#include "urcu/futex.h"
#include "urcu/tls-compat.h"
#include "urcu-die.h"

/* Data structure that identifies a call_rcu thread. */

struct call_rcu_data {
	struct cds_wfq_queue cbs;
	unsigned long flags;
	int32_t futex;
	unsigned long qlen; /* maintained for debugging. */
	pthread_t tid;
	int cpu_affinity;
	struct cds_list_head list;
} __attribute__((aligned(CAA_CACHE_LINE_SIZE)));

/*
 * List of all call_rcu_data structures to keep valgrind happy.
 * Protected by call_rcu_mutex.
 */

CDS_LIST_HEAD(call_rcu_data_list);

/* Link a thread using call_rcu() to its call_rcu thread. */

static DEFINE_URCU_TLS(struct call_rcu_data *, thread_call_rcu_data);

/*
 * Guard call_rcu thread creation and atfork handlers.
 */
static pthread_mutex_t call_rcu_mutex = PTHREAD_MUTEX_INITIALIZER;

/* If a given thread does not have its own call_rcu thread, this is default. */

static struct call_rcu_data *default_call_rcu_data;

/*
 * If the sched_getcpu() and sysconf(_SC_NPROCESSORS_CONF) calls are
 * available, then we can have call_rcu threads assigned to individual
 * CPUs rather than only to specific threads.
 */

#if defined(HAVE_SCHED_GETCPU) && defined(HAVE_SYSCONF)

/*
 * Pointer to array of pointers to per-CPU call_rcu_data structures
 * and # CPUs. per_cpu_call_rcu_data is a RCU-protected pointer to an
 * array of RCU-protected pointers to call_rcu_data. call_rcu acts as a
 * RCU read-side and reads per_cpu_call_rcu_data and the per-cpu pointer
 * without mutex. The call_rcu_mutex protects updates.
 */

static struct call_rcu_data **per_cpu_call_rcu_data;
static long maxcpus;

static void maxcpus_reset(void)
{
	maxcpus = 0;
}

/* Allocate the array if it has not already been allocated. */

static void alloc_cpu_call_rcu_data(void)
{
	struct call_rcu_data **p;
	static int warned = 0;

	if (maxcpus != 0)
		return;
	maxcpus = sysconf(_SC_NPROCESSORS_CONF);
	if (maxcpus <= 0) {
		return;
	}
	p = malloc(maxcpus * sizeof(*per_cpu_call_rcu_data));
	if (p != NULL) {
		memset(p, '\0', maxcpus * sizeof(*per_cpu_call_rcu_data));
		rcu_set_pointer(&per_cpu_call_rcu_data, p);
	} else {
		if (!warned) {
			fprintf(stderr, "[error] liburcu: unable to allocate per-CPU pointer array\n");
		}
		warned = 1;
	}
}

#else /* #if defined(HAVE_SCHED_GETCPU) && defined(HAVE_SYSCONF) */

/*
 * per_cpu_call_rcu_data should be constant, but some functions below, used both
 * for cases where cpu number is available and not available, assume it it not
 * constant.
 */
static struct call_rcu_data **per_cpu_call_rcu_data = NULL;
static const long maxcpus = -1;

static void maxcpus_reset(void)
{
}

static void alloc_cpu_call_rcu_data(void)
{
}

static int sched_getcpu(void)
{
	return -1;
}

#endif /* #else #if defined(HAVE_SCHED_GETCPU) && defined(HAVE_SYSCONF) */

/* Acquire the specified pthread mutex. */

static void call_rcu_lock(pthread_mutex_t *pmp)
{
	int ret;

	ret = pthread_mutex_lock(pmp);
	if (ret)
		urcu_die(ret);
}

/* Release the specified pthread mutex. */

static void call_rcu_unlock(pthread_mutex_t *pmp)
{
	int ret;

	ret = pthread_mutex_unlock(pmp);
	if (ret)
		urcu_die(ret);
}

#if HAVE_SCHED_SETAFFINITY
static
int set_thread_cpu_affinity(struct call_rcu_data *crdp)
{
	cpu_set_t mask;

	if (crdp->cpu_affinity < 0)
		return 0;

	CPU_ZERO(&mask);
	CPU_SET(crdp->cpu_affinity, &mask);
#if SCHED_SETAFFINITY_ARGS == 2
	return sched_setaffinity(0, &mask);
#else
	return sched_setaffinity(0, sizeof(mask), &mask);
#endif
}
#else
static
int set_thread_cpu_affinity(struct call_rcu_data *crdp)
{
	return 0;
}
#endif

static void call_rcu_wait(struct call_rcu_data *crdp)
{
	/* Read call_rcu list before read futex */
	cmm_smp_mb();
	if (uatomic_read(&crdp->futex) == -1)
		futex_async(&crdp->futex, FUTEX_WAIT, -1,
		      NULL, NULL, 0);
}

static void call_rcu_wake_up(struct call_rcu_data *crdp)
{
	/* Write to call_rcu list before reading/writing futex */
	cmm_smp_mb();
	if (caa_unlikely(uatomic_read(&crdp->futex) == -1)) {
		uatomic_set(&crdp->futex, 0);
		futex_async(&crdp->futex, FUTEX_WAKE, 1,
		      NULL, NULL, 0);
	}
}

/* This is the code run by each call_rcu thread. */

static void *call_rcu_thread(void *arg)
{
	unsigned long cbcount;
	struct cds_wfq_node *cbs;
	struct cds_wfq_node **cbs_tail;
	struct call_rcu_data *crdp = (struct call_rcu_data *)arg;
	struct rcu_head *rhp;
	int rt = !!(uatomic_read(&crdp->flags) & URCU_CALL_RCU_RT);
	int ret;

	ret = set_thread_cpu_affinity(crdp);
	if (ret)
		urcu_die(errno);

	/*
	 * If callbacks take a read-side lock, we need to be registered.
	 */
	rcu_register_thread();

	URCU_TLS(thread_call_rcu_data) = crdp;
	if (!rt) {
		uatomic_dec(&crdp->futex);
		/* Decrement futex before reading call_rcu list */
		cmm_smp_mb();
	}
	for (;;) {
		if (uatomic_read(&crdp->flags) & URCU_CALL_RCU_PAUSE) {
			/*
			 * Pause requested. Become quiescent: remove
			 * ourself from all global lists, and don't
			 * process any callback. The callback lists may
			 * still be non-empty though.
			 */
			rcu_unregister_thread();
			cmm_smp_mb__before_uatomic_or();
			uatomic_or(&crdp->flags, URCU_CALL_RCU_PAUSED);
			while ((uatomic_read(&crdp->flags) & URCU_CALL_RCU_PAUSE) != 0)
				poll(NULL, 0, 1);
			rcu_register_thread();
		}

		if (&crdp->cbs.head != _CMM_LOAD_SHARED(crdp->cbs.tail)) {
			while ((cbs = _CMM_LOAD_SHARED(crdp->cbs.head)) == NULL)
				poll(NULL, 0, 1);
			_CMM_STORE_SHARED(crdp->cbs.head, NULL);
			cbs_tail = (struct cds_wfq_node **)
				uatomic_xchg(&crdp->cbs.tail, &crdp->cbs.head);
			synchronize_rcu();
			cbcount = 0;
			do {
				while (cbs->next == NULL &&
				       &cbs->next != cbs_tail)
				       	poll(NULL, 0, 1);
				if (cbs == &crdp->cbs.dummy) {
					cbs = cbs->next;
					continue;
				}
				rhp = (struct rcu_head *)cbs;
				cbs = cbs->next;
				rhp->func(rhp);
				cbcount++;
			} while (cbs != NULL);
			uatomic_sub(&crdp->qlen, cbcount);
		}
		if (uatomic_read(&crdp->flags) & URCU_CALL_RCU_STOP)
			break;
		rcu_thread_offline();
		if (!rt) {
			if (&crdp->cbs.head
			    == _CMM_LOAD_SHARED(crdp->cbs.tail)) {
				call_rcu_wait(crdp);
				poll(NULL, 0, 10);
				uatomic_dec(&crdp->futex);
				/*
				 * Decrement futex before reading
				 * call_rcu list.
				 */
				cmm_smp_mb();
			} else {
				poll(NULL, 0, 10);
			}
		} else {
			poll(NULL, 0, 10);
		}
		rcu_thread_online();
	}
	if (!rt) {
		/*
		 * Read call_rcu list before write futex.
		 */
		cmm_smp_mb();
		uatomic_set(&crdp->futex, 0);
	}
	uatomic_or(&crdp->flags, URCU_CALL_RCU_STOPPED);
	rcu_unregister_thread();
	return NULL;
}

/*
 * Create both a call_rcu thread and the corresponding call_rcu_data
 * structure, linking the structure in as specified.  Caller must hold
 * call_rcu_mutex.
 */

static void call_rcu_data_init(struct call_rcu_data **crdpp,
			       unsigned long flags,
			       int cpu_affinity)
{
	struct call_rcu_data *crdp;
	int ret;

	crdp = malloc(sizeof(*crdp));
	if (crdp == NULL)
		urcu_die(errno);
	memset(crdp, '\0', sizeof(*crdp));
	cds_wfq_init(&crdp->cbs);
	crdp->qlen = 0;
	crdp->futex = 0;
	crdp->flags = flags;
	cds_list_add(&crdp->list, &call_rcu_data_list);
	crdp->cpu_affinity = cpu_affinity;
	cmm_smp_mb();  /* Structure initialized before pointer is planted. */
	*crdpp = crdp;
	ret = pthread_create(&crdp->tid, NULL, call_rcu_thread, crdp);
	if (ret)
		urcu_die(ret);
}

/*
 * Return a pointer to the call_rcu_data structure for the specified
 * CPU, returning NULL if there is none.  We cannot automatically
 * created it because the platform we are running on might not define
 * sched_getcpu().
 *
 * The call to this function and use of the returned call_rcu_data
 * should be protected by RCU read-side lock.
 */

struct call_rcu_data *get_cpu_call_rcu_data(int cpu)
{
	static int warned = 0;
	struct call_rcu_data **pcpu_crdp;

	pcpu_crdp = rcu_dereference(per_cpu_call_rcu_data);
	if (pcpu_crdp == NULL)
		return NULL;
	if (!warned && maxcpus > 0 && (cpu < 0 || maxcpus <= cpu)) {
		fprintf(stderr, "[error] liburcu: get CPU # out of range\n");
		warned = 1;
	}
	if (cpu < 0 || maxcpus <= cpu)
		return NULL;
	return rcu_dereference(pcpu_crdp[cpu]);
}

/*
 * Return the tid corresponding to the call_rcu thread whose
 * call_rcu_data structure is specified.
 */

pthread_t get_call_rcu_thread(struct call_rcu_data *crdp)
{
	return crdp->tid;
}

/*
 * Create a call_rcu_data structure (with thread) and return a pointer.
 */

static struct call_rcu_data *__create_call_rcu_data(unsigned long flags,
						    int cpu_affinity)
{
	struct call_rcu_data *crdp;

	call_rcu_data_init(&crdp, flags, cpu_affinity);
	return crdp;
}

struct call_rcu_data *create_call_rcu_data(unsigned long flags,
					   int cpu_affinity)
{
	struct call_rcu_data *crdp;

	call_rcu_lock(&call_rcu_mutex);
	crdp = __create_call_rcu_data(flags, cpu_affinity);
	call_rcu_unlock(&call_rcu_mutex);
	return crdp;
}

/*
 * Set the specified CPU to use the specified call_rcu_data structure.
 *
 * Use NULL to remove a CPU's call_rcu_data structure, but it is
 * the caller's responsibility to dispose of the removed structure.
 * Use get_cpu_call_rcu_data() to obtain a pointer to the old structure
 * (prior to NULLing it out, of course).
 *
 * The caller must wait for a grace-period to pass between return from
 * set_cpu_call_rcu_data() and call to call_rcu_data_free() passing the
 * previous call rcu data as argument.
 */

int set_cpu_call_rcu_data(int cpu, struct call_rcu_data *crdp)
{
	static int warned = 0;

	call_rcu_lock(&call_rcu_mutex);
	alloc_cpu_call_rcu_data();
	if (cpu < 0 || maxcpus <= cpu) {
		if (!warned) {
			fprintf(stderr, "[error] liburcu: set CPU # out of range\n");
			warned = 1;
		}
		call_rcu_unlock(&call_rcu_mutex);
		errno = EINVAL;
		return -EINVAL;
	}

	if (per_cpu_call_rcu_data == NULL) {
		call_rcu_unlock(&call_rcu_mutex);
		errno = ENOMEM;
		return -ENOMEM;
	}

	if (per_cpu_call_rcu_data[cpu] != NULL && crdp != NULL) {
		call_rcu_unlock(&call_rcu_mutex);
		errno = EEXIST;
		return -EEXIST;
	}

	rcu_set_pointer(&per_cpu_call_rcu_data[cpu], crdp);
	call_rcu_unlock(&call_rcu_mutex);
	return 0;
}

/*
 * Return a pointer to the default call_rcu_data structure, creating
 * one if need be.  Because we never free call_rcu_data structures,
 * we don't need to be in an RCU read-side critical section.
 */

struct call_rcu_data *get_default_call_rcu_data(void)
{
	if (default_call_rcu_data != NULL)
		return rcu_dereference(default_call_rcu_data);
	call_rcu_lock(&call_rcu_mutex);
	if (default_call_rcu_data != NULL) {
		call_rcu_unlock(&call_rcu_mutex);
		return default_call_rcu_data;
	}
	call_rcu_data_init(&default_call_rcu_data, 0, -1);
	call_rcu_unlock(&call_rcu_mutex);
	return default_call_rcu_data;
}

/*
 * Return the call_rcu_data structure that applies to the currently
 * running thread.  Any call_rcu_data structure assigned specifically
 * to this thread has first priority, followed by any call_rcu_data
 * structure assigned to the CPU on which the thread is running,
 * followed by the default call_rcu_data structure.  If there is not
 * yet a default call_rcu_data structure, one will be created.
 *
 * Calls to this function and use of the returned call_rcu_data should
 * be protected by RCU read-side lock.
 */
struct call_rcu_data *get_call_rcu_data(void)
{
	struct call_rcu_data *crd;

	if (URCU_TLS(thread_call_rcu_data) != NULL)
		return URCU_TLS(thread_call_rcu_data);

	if (maxcpus > 0) {
		crd = get_cpu_call_rcu_data(sched_getcpu());
		if (crd)
			return crd;
	}

	return get_default_call_rcu_data();
}

/*
 * Return a pointer to this task's call_rcu_data if there is one.
 */

struct call_rcu_data *get_thread_call_rcu_data(void)
{
	return URCU_TLS(thread_call_rcu_data);
}

/*
 * Set this task's call_rcu_data structure as specified, regardless
 * of whether or not this task already had one.  (This allows switching
 * to and from real-time call_rcu threads, for example.)
 *
 * Use NULL to remove a thread's call_rcu_data structure, but it is
 * the caller's responsibility to dispose of the removed structure.
 * Use get_thread_call_rcu_data() to obtain a pointer to the old structure
 * (prior to NULLing it out, of course).
 */

void set_thread_call_rcu_data(struct call_rcu_data *crdp)
{
	URCU_TLS(thread_call_rcu_data) = crdp;
}

/*
 * Create a separate call_rcu thread for each CPU.  This does not
 * replace a pre-existing call_rcu thread -- use the set_cpu_call_rcu_data()
 * function if you want that behavior. Should be paired with
 * free_all_cpu_call_rcu_data() to teardown these call_rcu worker
 * threads.
 */

int create_all_cpu_call_rcu_data(unsigned long flags)
{
	int i;
	struct call_rcu_data *crdp;
	int ret;

	call_rcu_lock(&call_rcu_mutex);
	alloc_cpu_call_rcu_data();
	call_rcu_unlock(&call_rcu_mutex);
	if (maxcpus <= 0) {
		errno = EINVAL;
		return -EINVAL;
	}
	if (per_cpu_call_rcu_data == NULL) {
		errno = ENOMEM;
		return -ENOMEM;
	}
	for (i = 0; i < maxcpus; i++) {
		call_rcu_lock(&call_rcu_mutex);
		if (get_cpu_call_rcu_data(i)) {
			call_rcu_unlock(&call_rcu_mutex);
			continue;
		}
		crdp = __create_call_rcu_data(flags, i);
		if (crdp == NULL) {
			call_rcu_unlock(&call_rcu_mutex);
			errno = ENOMEM;
			return -ENOMEM;
		}
		call_rcu_unlock(&call_rcu_mutex);
		if ((ret = set_cpu_call_rcu_data(i, crdp)) != 0) {
			call_rcu_data_free(crdp);

			/* it has been created by other thread */
			if (ret == -EEXIST)
				continue;

			return ret;
		}
	}
	return 0;
}

/*
 * Wake up the call_rcu thread corresponding to the specified
 * call_rcu_data structure.
 */
static void wake_call_rcu_thread(struct call_rcu_data *crdp)
{
	if (!(_CMM_LOAD_SHARED(crdp->flags) & URCU_CALL_RCU_RT))
		call_rcu_wake_up(crdp);
}

/*
 * Schedule a function to be invoked after a following grace period.
 * This is the only function that must be called -- the others are
 * only present to allow applications to tune their use of RCU for
 * maximum performance.
 *
 * Note that unless a call_rcu thread has not already been created,
 * the first invocation of call_rcu() will create one.  So, if you
 * need the first invocation of call_rcu() to be fast, make sure
 * to create a call_rcu thread first.  One way to accomplish this is
 * "get_call_rcu_data();", and another is create_all_cpu_call_rcu_data().
 *
 * call_rcu must be called by registered RCU read-side threads.
 */

void call_rcu(struct rcu_head *head,
	      void (*func)(struct rcu_head *head))
{
	struct call_rcu_data *crdp;

	cds_wfq_node_init(&head->next);
	head->func = func;
	/* Holding rcu read-side lock across use of per-cpu crdp */
	rcu_read_lock();
	crdp = get_call_rcu_data();
	cds_wfq_enqueue(&crdp->cbs, &head->next);
	uatomic_inc(&crdp->qlen);
	wake_call_rcu_thread(crdp);
	rcu_read_unlock();
}

/*
 * Free up the specified call_rcu_data structure, terminating the
 * associated call_rcu thread.  The caller must have previously
 * removed the call_rcu_data structure from per-thread or per-CPU
 * usage.  For example, set_cpu_call_rcu_data(cpu, NULL) for per-CPU
 * call_rcu_data structures or set_thread_call_rcu_data(NULL) for
 * per-thread call_rcu_data structures.
 *
 * We silently refuse to free up the default call_rcu_data structure
 * because that is where we put any leftover callbacks.  Note that
 * the possibility of self-spawning callbacks makes it impossible
 * to execute all the callbacks in finite time without putting any
 * newly spawned callbacks somewhere else.  The "somewhere else" of
 * last resort is the default call_rcu_data structure.
 *
 * We also silently refuse to free NULL pointers.  This simplifies
 * the calling code.
 *
 * The caller must wait for a grace-period to pass between return from
 * set_cpu_call_rcu_data() and call to call_rcu_data_free() passing the
 * previous call rcu data as argument.
 */
void call_rcu_data_free(struct call_rcu_data *crdp)
{
	struct cds_wfq_node *cbs;
	struct cds_wfq_node **cbs_tail;
	struct cds_wfq_node **cbs_endprev;

	if (crdp == NULL || crdp == default_call_rcu_data) {
		return;
	}
	if ((uatomic_read(&crdp->flags) & URCU_CALL_RCU_STOPPED) == 0) {
		uatomic_or(&crdp->flags, URCU_CALL_RCU_STOP);
		wake_call_rcu_thread(crdp);
		while ((uatomic_read(&crdp->flags) & URCU_CALL_RCU_STOPPED) == 0)
			poll(NULL, 0, 1);
	}
	if (&crdp->cbs.head != _CMM_LOAD_SHARED(crdp->cbs.tail)) {
		while ((cbs = _CMM_LOAD_SHARED(crdp->cbs.head)) == NULL)
			poll(NULL, 0, 1);
		_CMM_STORE_SHARED(crdp->cbs.head, NULL);
		cbs_tail = (struct cds_wfq_node **)
			uatomic_xchg(&crdp->cbs.tail, &crdp->cbs.head);
		/* Create default call rcu data if need be */
		(void) get_default_call_rcu_data();
		cbs_endprev = (struct cds_wfq_node **)
			uatomic_xchg(&default_call_rcu_data->cbs.tail,
					cbs_tail);
		_CMM_STORE_SHARED(*cbs_endprev, cbs);
		uatomic_add(&default_call_rcu_data->qlen,
			    uatomic_read(&crdp->qlen));
		wake_call_rcu_thread(default_call_rcu_data);
	}

	call_rcu_lock(&call_rcu_mutex);
	cds_list_del(&crdp->list);
	call_rcu_unlock(&call_rcu_mutex);

	free(crdp);
}

/*
 * Clean up all the per-CPU call_rcu threads.
 */
void free_all_cpu_call_rcu_data(void)
{
	int cpu;
	struct call_rcu_data **crdp;
	static int warned = 0;

	if (maxcpus <= 0)
		return;

	crdp = malloc(sizeof(*crdp) * maxcpus);
	if (!crdp) {
		if (!warned) {
			fprintf(stderr, "[error] liburcu: unable to allocate per-CPU pointer array\n");
		}
		warned = 1;
		return;
	}

	for (cpu = 0; cpu < maxcpus; cpu++) {
		crdp[cpu] = get_cpu_call_rcu_data(cpu);
		if (crdp[cpu] == NULL)
			continue;
		set_cpu_call_rcu_data(cpu, NULL);
	}
	/*
	 * Wait for call_rcu sites acting as RCU readers of the
	 * call_rcu_data to become quiescent.
	 */
	synchronize_rcu();
	for (cpu = 0; cpu < maxcpus; cpu++) {
		if (crdp[cpu] == NULL)
			continue;
		call_rcu_data_free(crdp[cpu]);
	}
	free(crdp);
}

/*
 * Acquire the call_rcu_mutex in order to ensure that the child sees
 * all of the call_rcu() data structures in a consistent state. Ensure
 * that all call_rcu threads are in a quiescent state across fork.
 * Suitable for pthread_atfork() and friends.
 */
void call_rcu_before_fork(void)
{
	struct call_rcu_data *crdp;

	call_rcu_lock(&call_rcu_mutex);

	cds_list_for_each_entry(crdp, &call_rcu_data_list, list) {
		uatomic_or(&crdp->flags, URCU_CALL_RCU_PAUSE);
		cmm_smp_mb__after_uatomic_or();
		wake_call_rcu_thread(crdp);
	}
	cds_list_for_each_entry(crdp, &call_rcu_data_list, list) {
		while ((uatomic_read(&crdp->flags) & URCU_CALL_RCU_PAUSED) == 0)
			poll(NULL, 0, 1);
	}
}

/*
 * Clean up call_rcu data structures in the parent of a successful fork()
 * that is not followed by exec() in the child.  Suitable for
 * pthread_atfork() and friends.
 */
void call_rcu_after_fork_parent(void)
{
	struct call_rcu_data *crdp;

	cds_list_for_each_entry(crdp, &call_rcu_data_list, list)
		uatomic_and(&crdp->flags, ~URCU_CALL_RCU_PAUSE);
	call_rcu_unlock(&call_rcu_mutex);
}

/*
 * Clean up call_rcu data structures in the child of a successful fork()
 * that is not followed by exec().  Suitable for pthread_atfork() and
 * friends.
 */
void call_rcu_after_fork_child(void)
{
	struct call_rcu_data *crdp, *next;

	/* Release the mutex. */
	call_rcu_unlock(&call_rcu_mutex);

	/* Do nothing when call_rcu() has not been used */
	if (cds_list_empty(&call_rcu_data_list))
		return;

	/*
	 * Allocate a new default call_rcu_data structure in order
	 * to get a working call_rcu thread to go with it.
	 */
	default_call_rcu_data = NULL;
	(void)get_default_call_rcu_data();

	/* Cleanup call_rcu_data pointers before use */
	maxcpus_reset();
	free(per_cpu_call_rcu_data);
	rcu_set_pointer(&per_cpu_call_rcu_data, NULL);
	URCU_TLS(thread_call_rcu_data) = NULL;

	/*
	 * Dispose of all of the rest of the call_rcu_data structures.
	 * Leftover call_rcu callbacks will be merged into the new
	 * default call_rcu thread queue.
	 */
	cds_list_for_each_entry_safe(crdp, next, &call_rcu_data_list, list) {
		if (crdp == default_call_rcu_data)
			continue;
		uatomic_set(&crdp->flags, URCU_CALL_RCU_STOPPED);
		call_rcu_data_free(crdp);
	}
}
