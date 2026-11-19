/*
 * wrr-run -- run a command as a SCHED_WRR task.
 *
 *   wrr-run [-w WEIGHT] [-c CPULIST] [-r] COMMAND [ARGS...]
 *
 *   -w WEIGHT   weight 1..20 (default 1)
 *   -c CPULIST  CPUs it may use, e.g. "1" or "1-3" or "1,3"
 *   -r          SCHED_RESET_ON_FORK: children start as SCHED_OTHER
 *
 * Example:  wrr-run -w 3 -c 1 spin 5
 * Needs root (CAP_SYS_NICE) to join SCHED_WRR.
 */
#define _GNU_SOURCE
#include "wrr.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int parse_cpulist(const char *s, cpu_set_t *set);

int parse_cpulist(const char *s, cpu_set_t *set)
{
	char *end;

	CPU_ZERO(set);
	while (*s) {
		long a = strtol(s, &end, 10), b = a;

		if (end == s || a < 0 || a >= CPU_SETSIZE)
			return -1;
		if (*end == '-') {
			s = end + 1;
			b = strtol(s, &end, 10);
			if (end == s || b < a || b >= CPU_SETSIZE)
				return -1;
		}
		for (long c = a; c <= b; c++)
			CPU_SET(c, set);
		if (*end == ',')
			end++;
		else if (*end)
			return -1;
		s = end;
	}
	return CPU_COUNT(set) ? 0 : -1;
}

static void usage(void)
{
	fprintf(stderr, "usage: wrr-run [-w WEIGHT] [-c CPULIST] [-r] COMMAND [ARGS...]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	int weight = WRR_DEFAULT_WEIGHT, policy = SCHED_WRR, opt;
	struct sched_param sp = { .sched_priority = 0 };
	cpu_set_t set;
	int have_cpus = 0;

	while ((opt = getopt(argc, argv, "+w:c:rh")) != -1) {
		switch (opt) {
		case 'w':
			weight = atoi(optarg);
			break;
		case 'c':
			if (parse_cpulist(optarg, &set))
				usage();
			have_cpus = 1;
			break;
		case 'r':
			policy |= SCHED_RESET_ON_FORK;
			break;
		default:
			usage();
		}
	}
	if (optind >= argc)
		usage();

	/*
	 * Join SCHED_WRR before moving to the requested CPUs: a SCHED_OTHER
	 * task moved onto a CPU where a WRR task spins would never run again
	 * to finish the job.
	 */
	if (sched_setscheduler(0, policy, &sp)) {
		perror("sched_setscheduler(SCHED_WRR)");
		return 1;
	}
	if (weight != WRR_DEFAULT_WEIGHT && sched_setweight(0, weight)) {
		perror("sched_setweight");
		return 1;
	}
	if (have_cpus && sched_setaffinity(0, sizeof(set), &set)) {
		perror("sched_setaffinity");
		return 1;
	}
	execvp(argv[optind], argv + optind);
	perror(argv[optind]);
	return 127;
}
