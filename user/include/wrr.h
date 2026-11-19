/*
 * wrr.h -- user-space interface to the SCHED_WRR scheduling class.
 *
 * CSPB 3753 scheduler assignment. These numbers must match the kernel:
 *   SCHED_WRR             include/uapi/linux/sched.h
 *   __NR_sched_setweight  arch/x86/entry/syscalls/syscall_64.tbl,
 *   __NR_sched_getweight  include/uapi/asm-generic/unistd.h (arm64)
 */
#ifndef WRR_H
#define WRR_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef SCHED_WRR
#define SCHED_WRR 7
#endif
#ifndef SCHED_RESET_ON_FORK
#define SCHED_RESET_ON_FORK 0x40000000
#endif

#define WRR_MIN_WEIGHT     1
#define WRR_DEFAULT_WEIGHT 1
#define WRR_MAX_WEIGHT     20
#define WRR_BASE_SLICE_MS  10 /* a weight-w task runs w * 10 ms per turn */

#define NR_sched_setweight 462
#define NR_sched_getweight 463

/* Returns 0, or -1 with errno set. */
static inline int sched_setweight(pid_t pid, int weight)
{
	return (int)syscall(NR_sched_setweight, pid, weight);
}

/* Returns the weight (1..20), or -1 with errno set. */
static inline int sched_getweight(pid_t pid)
{
	return (int)syscall(NR_sched_getweight, pid);
}

/* Make @pid (0 = caller) a SCHED_WRR task. Returns 0 or -1 with errno. */
static inline int wrr_set_policy(pid_t pid)
{
	struct sched_param sp = { .sched_priority = 0 };

	return sched_setscheduler(pid, SCHED_WRR, &sp);
}

#endif /* WRR_H */
