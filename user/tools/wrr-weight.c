/*
 * wrr-weight -- show or change the weight of SCHED_WRR tasks.
 *
 *   wrr-weight PID            print the weight of PID
 *   wrr-weight PID WEIGHT     set it (1..20)
 *
 * PID 0 means wrr-weight itself, which is only useful for testing errors.
 */
#include "wrr.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	pid_t pid;
	int w;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "usage: wrr-weight PID [WEIGHT]\n");
		return 2;
	}
	pid = (pid_t)atoi(argv[1]);
	if (argc == 3) {
		if (sched_setweight(pid, atoi(argv[2]))) {
			fprintf(stderr, "sched_setweight(%d, %s): %s\n", pid, argv[2],
				strerror(errno));
			return 1;
		}
	}
	w = sched_getweight(pid);
	if (w < 0) {
		fprintf(stderr, "sched_getweight(%d): %s\n", pid, strerror(errno));
		return 1;
	}
	printf("%d\n", w);
	return 0;
}
