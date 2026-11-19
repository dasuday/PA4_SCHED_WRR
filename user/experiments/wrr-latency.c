/*
 * wrr-latency -- how long does a task that sleeps and wakes up wait for
 * the CPU when CPU-bound tasks share that CPU?
 *
 *   wrr-latency [-c CPU] [-t SECONDS] [-n HOGS] [-w HOG_WEIGHT]
 *               [-W PROBE_WEIGHT] [-s SLEEP_US] [-p wrr|normal]
 *
 * Starts HOGS CPU-bound tasks (default 3) and one "probe" task on CPU
 * (default 1), all with the same policy. The probe loops: sleep SLEEP_US
 * (default 1000), then measure how late it woke up. That lateness is the
 * scheduling latency an interactive program would feel.
 *
 * Prints the number of wake-ups and the mean, median, 90th and 99th
 * percentile and maximum latency in milliseconds. Run as root.
 * Example:  wrr-latency -n 3 -w 10        then   wrr-latency -n 3 -p normal
 */
#define _GNU_SOURCE
#include "wrr.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>

#define MAX_SAMPLES 200000
#define MAX_HOGS 32

struct result {
	int count;
	double lat_ms[MAX_SAMPLES];
};

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int cmp(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

static void usage(void)
{
	fprintf(stderr, "usage: wrr-latency [-c CPU] [-t SECONDS] [-n HOGS] [-w HOG_WEIGHT] "
		"[-W PROBE_WEIGHT] [-s SLEEP_US] [-p wrr|normal]\n");
	exit(2);
}

static int setup(pid_t pid, int cpu, int wrr, int weight)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(pid, sizeof(set), &set)) {
		perror("sched_setaffinity");
		return -1;
	}
	if (!wrr)
		return 0;
	if (wrr_set_policy(pid) ||
	    (weight != WRR_DEFAULT_WEIGHT && sched_setweight(pid, weight))) {
		perror("making a SCHED_WRR task");
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	int cpu = 1, hogs = 3, hog_w = 1, probe_w = 1, wrr = 1, opt, go[2], n;
	long sleep_us = 1000;
	double secs = 5, sum = 0;
	pid_t hog[MAX_HOGS] = { 0 }, probe;
	struct sched_param fifo = { .sched_priority = 50 };
	struct result *r;
	char c;

	while ((opt = getopt(argc, argv, "c:t:n:w:W:s:p:h")) != -1) {
		switch (opt) {
		case 'c': cpu = atoi(optarg); break;
		case 't': secs = atof(optarg); break;
		case 'n': hogs = atoi(optarg); break;
		case 'w': hog_w = atoi(optarg); break;
		case 'W': probe_w = atoi(optarg); break;
		case 's': sleep_us = atol(optarg); break;
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
	if (hogs < 0 || hogs > MAX_HOGS || sleep_us < 1)
		usage();

	r = mmap(NULL, sizeof(*r), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (r == MAP_FAILED || pipe(go))
		return 1;
	if (sched_setscheduler(0, SCHED_FIFO | SCHED_RESET_ON_FORK, &fifo))
		perror("warning: sched_setscheduler(SCHED_FIFO)");

	for (int i = 0; i < hogs; i++) {
		hog[i] = fork();
		if (hog[i] == 0) {
			close(go[1]);
			if (read(go[0], &c, 1) < 0)
				_exit(1);
			for (;;)
				;
		}
		if (setup(hog[i], cpu, wrr, hog_w))
			goto out;
	}

	probe = fork();
	if (probe == 0) {
		struct timespec ts = { sleep_us / 1000000, (sleep_us % 1000000) * 1000 };
		long long end;

		close(go[1]);
		if (read(go[0], &c, 1) < 0)
			_exit(1);
		end = now_ns() + (long long)(secs * 1e9);
		while (now_ns() < end && r->count < MAX_SAMPLES) {
			long long t0 = now_ns();

			nanosleep(&ts, NULL);
			r->lat_ms[r->count++] = (now_ns() - t0 - sleep_us * 1000LL) / 1e6;
		}
		_exit(0);
	}
	if (setup(probe, cpu, wrr, probe_w))
		goto out;
	close(go[1]);
	waitpid(probe, NULL, 0);

	n = r->count;
	if (n == 0) {
		printf("the probe never woke up\n");
		goto out;
	}
	for (int i = 0; i < n; i++)
		sum += r->lat_ms[i];
	qsort(r->lat_ms, n, sizeof(double), cmp);
	printf("policy %s  hogs %d (weight %d)  probe weight %d  sleep %ld us  CPU %d\n",
	       wrr ? "SCHED_WRR" : "SCHED_OTHER", hogs, hog_w, probe_w, sleep_us, cpu);
	printf("wakeups %d  latency ms: mean %.2f  p50 %.2f  p90 %.2f  p99 %.2f  max %.2f\n",
	       n, sum / n, r->lat_ms[n / 2], r->lat_ms[(int)(n * 0.9)],
	       r->lat_ms[(int)(n * 0.99)], r->lat_ms[n - 1]);
out:
	for (int i = 0; i < hogs; i++)
		if (hog[i] > 0)
			kill(hog[i], SIGKILL);
	while (wait(NULL) > 0)
		;
	return 0;
}
