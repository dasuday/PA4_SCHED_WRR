/*
 * wrr-share -- measure how CPU-bound tasks on one CPU share it.
 *
 *   wrr-share [-c CPU] [-t SECONDS] [-p wrr|normal] N1 N2 ...
 *
 * Starts one CPU-bound task per number, all pinned to CPU (default 1), and
 * after a short warm-up measures each one's CPU time for SECONDS
 * (default 5).
 *   -p wrr     (default) the numbers are SCHED_WRR weights (1..20)
 *   -p normal  the numbers are nice values (-20..19) under SCHED_OTHER (CFS)
 *
 * Prints each task's measured share next to the share its weight entitles
 * it to. Run as root. Example:  wrr-share -t 10 1 2 3
 */
#define _GNU_SOURCE
#include "wrr.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>

#define MAX 32

/* CFS load weight of each nice level (kernel/sched/core.c sched_prio_to_weight). */
static const int nice_weight[40] = {
	88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916,
	9548, 7620, 6100, 4904, 3906, 3121, 2501, 1991, 1586, 1277,
	1024, 820, 655, 526, 423, 335, 272, 215, 172, 137,
	110, 87, 70, 56, 45, 36, 29, 23, 18, 15,
};

static long long cpu_ns(pid_t pid)
{
	clockid_t cid;
	struct timespec ts;

	if (clock_getcpuclockid(pid, &cid) || clock_gettime(cid, &ts))
		return -1;
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void usage(void)
{
	fprintf(stderr, "usage: wrr-share [-c CPU] [-t SECONDS] [-p wrr|normal] N1 N2 ...\n");
	exit(2);
}

int main(int argc, char **argv)
{
	int cpu = 1, wrr = 1, n, opt, go[2];
	double secs = 5, want_sum = 0, want[MAX];
	long long t0[MAX], t1[MAX], total = 0;
	int val[MAX];
	pid_t pid[MAX] = { 0 };
	struct sched_param fifo = { .sched_priority = 50 };
	struct timespec ts;
	cpu_set_t set;

	while ((opt = getopt(argc, argv, "c:t:p:h")) != -1) {
		switch (opt) {
		case 'c': cpu = atoi(optarg); break;
		case 't': secs = atof(optarg); break;
		case 'p':
			if (!strcmp(optarg, "wrr"))
				wrr = 1;
			else if (!strcmp(optarg, "normal"))
				wrr = 0;
			else
				usage();
			break;
		default: usage();
		}
	}
	n = argc - optind;
	if (n < 1 || n > MAX)
		usage();
	if (cpu == 0)
		fprintf(stderr, "warning: CPU 0 runs your shell; it will freeze for the run\n");

	for (int i = 0; i < n; i++) {
		val[i] = atoi(argv[optind + i]);
		if (wrr && (val[i] < WRR_MIN_WEIGHT || val[i] > WRR_MAX_WEIGHT))
			usage();
		if (!wrr && (val[i] < -20 || val[i] > 19))
			usage();
		want[i] = wrr ? val[i] : nice_weight[val[i] + 20];
		want_sum += want[i];
	}

	/* Run this controller as SCHED_FIFO so it gets to measure on time. */
	if (sched_setscheduler(0, SCHED_FIFO | SCHED_RESET_ON_FORK, &fifo))
		perror("warning: sched_setscheduler(SCHED_FIFO)");
	if (pipe(go))
		return 1;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	for (int i = 0; i < n; i++) {
		char c;

		pid[i] = fork();
		if (pid[i] == 0) {
			close(go[1]);
			if (read(go[0], &c, 1) < 0)
				_exit(1);
			for (;;)
				;
		}
		if (sched_setaffinity(pid[i], sizeof(set), &set)) {
			perror("sched_setaffinity");
			goto out;
		}
		if (wrr) {
			if (wrr_set_policy(pid[i]) ||
			    (val[i] != WRR_DEFAULT_WEIGHT && sched_setweight(pid[i], val[i]))) {
				perror("making a SCHED_WRR task");
				goto out;
			}
		} else if (setpriority(PRIO_PROCESS, pid[i], val[i])) {
			perror("setpriority");
			goto out;
		}
	}
	close(go[1]); /* every child's read() returns: go */

	ts.tv_sec = 0;
	ts.tv_nsec = 300000000;
	nanosleep(&ts, NULL);
	for (int i = 0; i < n; i++)
		t0[i] = cpu_ns(pid[i]);
	ts.tv_sec = (time_t)secs;
	ts.tv_nsec = (long)((secs - (double)ts.tv_sec) * 1e9);
	nanosleep(&ts, NULL);
	for (int i = 0; i < n; i++) {
		t1[i] = cpu_ns(pid[i]);
		total += t1[i] - t0[i];
	}

	printf("%-6s %-8s %10s %10s %10s\n", "task", wrr ? "weight" : "nice",
	       "cpu(ms)", "share", "expected");
	for (int i = 0; i < n; i++)
		printf("%-6d %-8d %10.1f %10.3f %10.3f\n", i, val[i], (t1[i] - t0[i]) / 1e6,
		       total > 0 ? (double)(t1[i] - t0[i]) / total : 0.0, want[i] / want_sum);
	printf("total CPU time %.1f ms over %.1f s on CPU %d (%s)\n", total / 1e6, secs, cpu,
	       wrr ? "SCHED_WRR" : "SCHED_OTHER");
out:
	for (int i = 0; i < n; i++)
		if (pid[i] > 0)
			kill(pid[i], SIGKILL);
	while (wait(NULL) > 0)
		;
	return 0;
}
