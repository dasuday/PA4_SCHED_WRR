/*
 * spin -- burn CPU for a while, then report how much CPU time it got.
 *
 *   spin [SECONDS]      default 10; 0 means forever
 *
 * Prints: pid, policy, weight (if WRR), wall time, CPU time and the
 * fraction of one CPU it received.
 */
#define _GNU_SOURCE
#include "wrr.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(clockid_t clk)
{
	struct timespec ts;

	clock_gettime(clk, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	double secs = argc > 1 ? atof(argv[1]) : 10;
	double start = now(CLOCK_MONOTONIC), end = start + secs, wall, cpu;
	int policy = sched_getscheduler(0);

	while (secs == 0 || now(CLOCK_MONOTONIC) < end)
		;
	wall = now(CLOCK_MONOTONIC) - start;
	cpu = now(CLOCK_PROCESS_CPUTIME_ID);
	printf("spin pid %d policy %d", getpid(), policy);
	if (policy == SCHED_WRR)
		printf(" weight %d", sched_getweight(0));
	printf(" wall %.2fs cpu %.2fs share %.3f\n", wall, cpu, cpu / wall);
	return 0;
}
