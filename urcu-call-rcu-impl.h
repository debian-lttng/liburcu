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

static __thread struct call_rcu_data *thread_call_rcu_data;

/* Guard call_rcu thread creation. */

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
 * and # CPUs.
 */

static struct call_rcu_data **per_cpu_call_rcu_data;
static long maxcpus;

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
		per_cpu_call_rcu_data = p;
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
	if (pthread_mutex_lock(pmp) != 0) {
		perror("pthread_mutex_lock");
		exit(-1);
	}
}

/* Release the specified pthread mutex. */

static void call_rcu_unlock(pthread_mutex_t *pmp)
{
	if (pthread_mutex_unlock(pmp) != 0) {
		perror("pthread_mutex_unlock");
		exit(-1);
	}
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
	if (unlikely(uatomic_read(&crdp->futex) == -1)) {
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

	if (set_thread_cpu_affinity(crdp) != 0) {
		perror("pthread_setaffinity_np");
		exit(-1);
	}

	thread_call_rcu_data = crdp;
	if (!rt) {
		uatomic_dec(&crdp->futex);
		/* Decrement futex before reading call_rcu list */
		cmm_smp_mb();
	}
	for (;;) {
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
	}
	if (!rt) {
		/*
		 * Read call_rcu list before write futex.
		 */
		cmm_smp_mb();
		uatomic_set(&crdp->futex, 0);
	}
	uatomic_or(&crdp->flags, URCU_CALL_RCU_STOPPED);
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

	crdp = malloc(sizeof(*crdp));
	if (crdp == NULL) {
		fprintf(stderr, "Out of memory.\n");
		exit(-1);
	}
	memset(crdp, '\0', sizeof(*crdp));
	cds_wfq_init(&crdp->cbs);
	crdp->qlen = 0;
	crdp->futex = 0;
	crdp->flags = flags;
	cds_list_add(&crdp->list, &call_rcu_data_list);
	crdp->cpu_affinity = cpu_affinity;
	cmm_smp_mb();  /* Structure initialized before pointer is planted. */
	*crdpp = crdp;
	if (pthread_create(&crdp->tid, NULL, call_rcu_thread, crdp) != 0) {
		perror("pthread_create");
		exit(-1);
	}
}

/*
 * Return a pointer to the call_rcu_data structure for the specified
 * CPU, returning NULL if there is none.  We cannot automatically
 * created it because the platform we are running on might not define
 * sched_getcpu().
 */

struct call_rcu_data *get_cpu_call_rcu_data(int cpu)
{
	static int warned = 0;

	if (per_cpu_call_rcu_data == NULL)
		return NULL;
	if (!warned && maxcpus > 0 && (cpu < 0 || maxcpus <= cpu)) {
		fprintf(stderr, "[error] liburcu: get CPU # out of range\n");
		warned = 1;
	}
	if (cpu < 0 || maxcpus <= cpu)
		return NULL;
	return per_cpu_call_rcu_data[cpu];
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
 */

int set_cpu_call_rcu_data(int cpu, struct call_rcu_data *crdp)
{
	int warned = 0;

	call_rcu_lock(&call_rcu_mutex);
	if (cpu < 0 || maxcpus <= cpu) {
		if (!warned) {
			fprintf(stderr, "[error] liburcu: set CPU # out of range\n");
			warned = 1;
		}
		call_rcu_unlock(&call_rcu_mutex);
		errno = EINVAL;
		return -EINVAL;
	}
	alloc_cpu_call_rcu_data();
	call_rcu_unlock(&call_rcu_mutex);
	if (per_cpu_call_rcu_data == NULL) {
		errno = ENOMEM;
		return -ENOMEM;
	}
	per_cpu_call_rcu_data[cpu] = crdp;
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
 */
struct call_rcu_data *get_call_rcu_data(void)
{
	int curcpu;
	static int warned = 0;

	if (thread_call_rcu_data != NULL)
		return thread_call_rcu_data;
	if (maxcpus <= 0)
		return get_default_call_rcu_data();
	curcpu = sched_getcpu();
	if (!warned && (curcpu < 0 || maxcpus <= curcpu)) {
		fprintf(stderr, "[error] liburcu: gcrd CPU # out of range\n");
		warned = 1;
	}
	if (curcpu >= 0 && maxcpus > curcpu &&
	    per_cpu_call_rcu_data != NULL &&
	    per_cpu_call_rcu_data[curcpu] != NULL)
	    	return per_cpu_call_rcu_data[curcpu];
	return get_default_call_rcu_data();
}

/*
 * Return a pointer to this task's call_rcu_data if there is one.
 */

struct call_rcu_data *get_thread_call_rcu_data(void)
{
	return thread_call_rcu_data;
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
	thread_call_rcu_data = crdp;
}

/*
 * Create a separate call_rcu thread for each CPU.  This does not
 * replace a pre-existing call_rcu thread -- use the set_cpu_call_rcu_data()
 * function if you want that behavior.
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
			/* FIXME: Leaks crdp for now. */
			return ret; /* Can happen on race. */
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
 */

void call_rcu(struct rcu_head *head,
	      void (*func)(struct rcu_head *head))
{
	struct call_rcu_data *crdp;

	cds_wfq_node_init(&head->next);
	head->func = func;
	crdp = get_call_rcu_data();
	cds_wfq_enqueue(&crdp->cbs, &head->next);
	uatomic_inc(&crdp->qlen);
	wake_call_rcu_thread(crdp);
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
		cbs_endprev = (struct cds_wfq_node **)
			uatomic_xchg(&default_call_rcu_data, cbs_tail);
		*cbs_endprev = cbs;
		uatomic_add(&default_call_rcu_data->qlen,
			    uatomic_read(&crdp->qlen));
		cds_list_del(&crdp->list);
		free(crdp);
	}
}

/*
 * Clean up all the per-CPU call_rcu threads.
 */
void free_all_cpu_call_rcu_data(void)
{
	int cpu;
	struct call_rcu_data *crdp;

	if (maxcpus <= 0)
		return;
	for (cpu = 0; cpu < maxcpus; cpu++) {
		crdp = get_cpu_call_rcu_data(cpu);
		if (crdp == NULL)
			continue;
		set_cpu_call_rcu_data(cpu, NULL);
		call_rcu_data_free(crdp);
	}
}

/*
 * Acquire the call_rcu_mutex in order to ensure that the child sees
 * all of the call_rcu() data structures in a consistent state.
 * Suitable for pthread_atfork() and friends.
 */
void call_rcu_before_fork(void)
{
	call_rcu_lock(&call_rcu_mutex);
}

/*
 * Clean up call_rcu data structures in the parent of a successful fork()
 * that is not followed by exec() in the child.  Suitable for
 * pthread_atfork() and friends.
 */
void call_rcu_after_fork_parent(void)
{
	call_rcu_unlock(&call_rcu_mutex);
}

/*
 * Clean up call_rcu data structures in the child of a successful fork()
 * that is not followed by exec().  Suitable for pthread_atfork() and
 * friends.
 */
void call_rcu_after_fork_child(void)
{
	struct call_rcu_data *crdp;

	/* Release the mutex. */
	call_rcu_unlock(&call_rcu_mutex);

	/*
	 * Allocate a new default call_rcu_data structure in order
	 * to get a working call_rcu thread to go with it.
	 */
	default_call_rcu_data = NULL;
	(void)get_default_call_rcu_data();

	/* Dispose of all of the rest of the call_rcu_data structures. */
	while (call_rcu_data_list.next != call_rcu_data_list.prev) {
		crdp = cds_list_entry(call_rcu_data_list.prev,
				      struct call_rcu_data, list);
		if (crdp == default_call_rcu_data)
			crdp = cds_list_entry(crdp->list.prev,
					      struct call_rcu_data, list);
		uatomic_set(&crdp->flags, URCU_CALL_RCU_STOPPED);
		call_rcu_data_free(crdp);
	}
}
