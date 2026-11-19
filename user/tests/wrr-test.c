/*
 * wrr-test -- test suite for the SCHED_WRR scheduling class.
 *
 * CSPB 3753 scheduler assignment. Run as root inside the test VM:
 *
 *   wrr-test             run every test and print a score
 *   wrr-test list        list the tests
 *   wrr-test NAME...     run only the named tests
 *
 * Needs at least 4 CPUs. CPU 0 is left for the shell and the test driver
 * (which runs as SCHED_FIFO); SCHED_WRR test tasks only run on CPUs 1-3, so
 * a broken scheduler cannot freeze the shell.
 *
 * Each test runs in its own process group. Any kernel WARNING/BUG/lockdep
 * report printed while a test runs makes that test fail.
 */
#define _GNU_SOURCE
#include "wrr.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <dirent.h>

#define MAX_WORKERS 64
#define MAX_CPUS    64
#define NOBODY_UID  65534

/* ------------------------------------------------------------------ util */

static char *fail_msg;		/* shared with the runner */

#define FAIL(...)                                                       \
	do {                                                            \
		snprintf(fail_msg, 400, __VA_ARGS__);                   \
		return 0;                                               \
	} while (0)

#define CHECK(cond, ...)                                                \
	do {                                                            \
		if (!(cond))                                            \
			FAIL(__VA_ARGS__);                              \
	} while (0)

static void info(const char *fmt, ...)
{
	va_list ap;

	printf("         ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
}

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void sleep_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
}

static void *shared_alloc(size_t size)
{
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
		       MAP_SHARED | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED) {
		perror("mmap");
		exit(2);
	}
	memset(p, 0, size);
	return p;
}

static int set_cpus(pid_t pid, int first, int last)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	for (int c = first; c <= last; c++)
		CPU_SET(c, &set);
	return sched_setaffinity(pid, sizeof(set), &set);
}

static int set_policy(pid_t pid, int policy, int prio)
{
	struct sched_param sp = { .sched_priority = prio };

	return sched_setscheduler(pid, policy, &sp);
}

static const char *policy_name(int policy)
{
	switch (policy & ~SCHED_RESET_ON_FORK) {
	case SCHED_OTHER: return "SCHED_OTHER";
	case SCHED_FIFO:  return "SCHED_FIFO";
	case SCHED_RR:    return "SCHED_RR";
	case SCHED_WRR:   return "SCHED_WRR";
	default:          return "?";
	}
}

/* CPU time used so far by process @pid, in ns (-1 on error). */
static long long cpu_time_ns(pid_t pid)
{
	clockid_t cid;
	struct timespec ts;

	if (clock_getcpuclockid(pid, &cid) || clock_gettime(cid, &ts))
		return -1;
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Reap @pid, waiting at most @ms. Returns 1 if reaped. */
static int wait_timeout(pid_t pid, long ms, int *status)
{
	long long end = now_ns() + ms * 1000000LL;
	int st;

	do {
		pid_t r = waitpid(pid, &st, WNOHANG);

		if (r == pid) {
			if (status)
				*status = st;
			return 1;
		}
		if (r < 0)
			return 0;
		sleep_ms(2);
	} while (now_ns() < end);
	return 0;
}

static int exited_ok(int status)
{
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/*
 * /sys/kernel/debug/sched/debug shows rq->wrr.nr_running and
 * rq->wrr.total_weight for every CPU (the starter code adds those lines).
 * Returns the number of CPUs found, or -1 if the file is not available.
 */
static int read_wrr_debug(long nr[], long tw[])
{
	FILE *f = fopen("/sys/kernel/debug/sched/debug", "r");
	char line[256];
	int cpu = -1, ncpu = 0;

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		char *colon = strchr(line, ':');

		if (!strncmp(line, "cpu#", 4)) {
			cpu = atoi(line + 4);
			if (cpu >= 0 && cpu < MAX_CPUS && cpu + 1 > ncpu)
				ncpu = cpu + 1;
			continue;
		}
		if (cpu < 0 || cpu >= MAX_CPUS || !colon)
			continue;
		if (strstr(line, ".wrr.nr_running"))
			nr[cpu] = atol(colon + 1);
		else if (strstr(line, ".wrr.total_weight"))
			tw[cpu] = atol(colon + 1);
	}
	fclose(f);
	return ncpu;
}

/* Wait (up to 1 s) for every CPU's WRR queue to be empty. */
static int check_queues_empty(void)
{
	long nr[MAX_CPUS] = { 0 }, tw[MAX_CPUS] = { 0 };
	int n = 0;

	for (int tries = 0; tries < 50; tries++) {
		int bad = 0;

		memset(nr, 0, sizeof(nr));
		memset(tw, 0, sizeof(tw));
		n = read_wrr_debug(nr, tw);
		if (n < 0)
			return 1; /* debugfs not mounted: nothing to check */
		for (int c = 0; c < n; c++)
			bad |= nr[c] || tw[c];
		if (!bad)
			return 1;
		sleep_ms(20);
	}
	for (int c = 0; c < n; c++)
		if (nr[c] || tw[c])
			snprintf(fail_msg, 400,
				 "all WRR tasks are gone but CPU %d still has "
				 "wrr.nr_running=%ld wrr.total_weight=%ld",
				 c, nr[c], tw[c]);
	return 0;
}

/* --------------------------------------------------------------- workers */

/*
 * A worker is a child process that waits on a pipe until the test has set
 * its policy, weight and affinity, then runs a function.
 */
struct worker {
	pid_t pid;
	int go_fd;
};

static struct worker workers[MAX_WORKERS];
static int nworkers;

static struct worker *spawn(void (*fn)(void *), void *arg)
{
	int fds[2];
	struct worker *w;
	char c;

	if (nworkers == MAX_WORKERS || pipe(fds))
		return NULL;
	w = &workers[nworkers];
	w->pid = fork();
	if (w->pid < 0)
		return NULL;
	if (w->pid == 0) {
		close(fds[1]);
		if (read(fds[0], &c, 1) != 1)
			_exit(3);
		close(fds[0]);
		fn(arg);
		_exit(0);
	}
	close(fds[0]);
	w->go_fd = fds[1];
	nworkers++;
	return w;
}

static void release(struct worker *w)
{
	if (w->go_fd >= 0) {
		if (write(w->go_fd, "g", 1) != 1)
			perror("write");
		close(w->go_fd);
		w->go_fd = -1;
	}
}

/*
 * Kill every process left in this test's process group. Each one is first
 * moved back to SCHED_OTHER, so that even a task that a broken WRR class
 * "lost" gets to run again and can die.
 */
static void kill_group(void)
{
	pid_t me = getpid(), pgid = getpgrp();
	DIR *d = opendir("/proc");
	struct dirent *de;

	for (int i = 0; i < nworkers; i++) {
		set_policy(workers[i].pid, SCHED_OTHER, 0);
		kill(workers[i].pid, SIGKILL);
	}
	while (d && (de = readdir(d))) {
		char path[64], buf[512], *p;
		pid_t pid = atoi(de->d_name);
		FILE *f;
		int grp;

		if (pid <= 0 || pid == me)
			continue;
		snprintf(path, sizeof(path), "/proc/%d/stat", pid);
		f = fopen(path, "r");
		if (!f)
			continue;
		p = fgets(buf, sizeof(buf), f) ? strrchr(buf, ')') : NULL;
		fclose(f);
		/* fields after "comm)": state ppid pgrp */
		if (p && sscanf(p + 2, "%*c %*d %d", &grp) == 1 && grp == pgid) {
			set_policy(pid, SCHED_OTHER, 0);
			kill(pid, SIGKILL);
		}
	}
	if (d)
		closedir(d);
	for (int i = 0; i < nworkers; i++) {
		if (workers[i].go_fd >= 0)
			close(workers[i].go_fd);
		wait_timeout(workers[i].pid, 2000, NULL);
	}
	nworkers = 0;
}

static void spin_forever(void *arg)
{
	(void)arg;
	for (;;)
		__asm__ __volatile__("" ::: "memory");
}

static void yield_forever(void *arg)
{
	(void)arg;
	for (;;)
		sched_yield();
}

static long long thread_cpu_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/*
 * Move the calling task to @cpu, burn 60 ms of CPU time there, and return
 * CPU time / elapsed time. Next to a CFS spinner on that CPU a WRR task
 * should get ~1.0 (it preempts CFS); a task that is really in the CFS class
 * gets ~0.5. A task that is never charged for CPU time gets ~0.
 */
static double cpu_ratio_on(int cpu)
{
	long long w0, c0, w, c;

	set_cpus(0, cpu, cpu);
	w0 = now_ns();
	c0 = thread_cpu_ns();
	do {
		w = now_ns();
		c = thread_cpu_ns();
	} while (c - c0 < 60000000LL && w - w0 < 2000000000LL);
	return (double)(c - c0) / (double)(w - w0);
}

/* Make @pid a WRR task with @weight. Returns 0 or an errno value. */
static int make_wrr(pid_t pid, int weight)
{
	if (wrr_set_policy(pid))
		return errno;
	if (weight != WRR_DEFAULT_WEIGHT && sched_setweight(pid, weight))
		return errno;
	return 0;
}

/* Spawn a spinner pinned to @cpu with the given policy/weight (not released). */
static struct worker *spinner(int cpu, int policy, int weight, int *err)
{
	struct worker *w = spawn(spin_forever, NULL);

	*err = 0;
	if (!w) {
		*err = EAGAIN;
		return NULL;
	}
	set_cpus(w->pid, cpu, cpu);
	if (policy == SCHED_WRR)
		*err = make_wrr(w->pid, weight);
	else if (policy == SCHED_FIFO)
		*err = set_policy(w->pid, SCHED_FIFO, 10) ? errno : 0;
	return w;
}

/* A running SCHED_OTHER spinner on @cpu. */
static struct worker *cfs_hog(int cpu)
{
	int err;
	struct worker *w = spinner(cpu, SCHED_OTHER, 0, &err);

	if (w)
		release(w);
	return w;
}

/*
 * Measure how the CPU time of workers @w[0..n-1] grows during a window of
 * @window_ms, after @warm_ms of warm-up. Stores each one's share.
 */
static int measure(struct worker **w, int n, long warm_ms, long window_ms,
		   double share[])
{
	long long t0[MAX_WORKERS], t1[MAX_WORKERS], total = 0;

	sleep_ms(warm_ms);
	for (int i = 0; i < n; i++)
		t0[i] = cpu_time_ns(w[i]->pid);
	sleep_ms(window_ms);
	for (int i = 0; i < n; i++) {
		t1[i] = cpu_time_ns(w[i]->pid);
		if (t0[i] < 0 || t1[i] < 0)
			FAIL("could not read the CPU-time clock of pid %d", w[i]->pid);
		total += t1[i] - t0[i];
	}
	if (total <= 0)
		FAIL("the test tasks used no CPU time at all in %ld ms "
		     "(are WRR tasks ever picked? is update_curr_wrr() charging them?)",
		     window_ms);
	for (int i = 0; i < n; i++)
		share[i] = (double)(t1[i] - t0[i]) / (double)total;
	return 1;
}

/* Run spinners of @weights on CPU @cpu and check their shares. */
static int check_shares(int cpu, int n, const int weights[], long window_ms,
			double rel_tol, double abs_tol)
{
	struct worker *w[MAX_WORKERS];
	double share[MAX_WORKERS];
	long nr[MAX_CPUS] = { 0 }, tw[MAX_CPUS] = { 0 };
	int sum = 0, err;
	char buf[256] = "";

	for (int i = 0; i < n; i++) {
		w[i] = spinner(cpu, SCHED_WRR, weights[i], &err);
		CHECK(w[i] && !err, "could not make a weight-%d WRR task: %s",
		      weights[i], strerror(err));
		sum += weights[i];
	}
	for (int i = 0; i < n; i++)
		release(w[i]);
	if (!measure(w, n, 300, window_ms, share))
		return 0;

	for (int i = 0; i < n; i++)
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf),
			 " w=%d:%.3f(want %.3f)", weights[i], share[i],
			 (double)weights[i] / sum);
	info("CPU shares over %ld ms:%s", window_ms, buf);

	for (int i = 0; i < n; i++) {
		double want = (double)weights[i] / sum;
		double tol = want * rel_tol > abs_tol ? want * rel_tol : abs_tol;

		CHECK(share[i] > want - tol && share[i] < want + tol,
		      "weight-%d task got %.3f of the CPU, expected %.3f +/- %.3f",
		      weights[i], share[i], want, tol);
	}
	if (read_wrr_debug(nr, tw) > cpu)
		CHECK(nr[cpu] == n && tw[cpu] == sum,
		      "CPU %d runs %d WRR tasks with total weight %d, but "
		      "rq->wrr shows nr_running=%ld total_weight=%ld",
		      cpu, n, sum, nr[cpu], tw[cpu]);
	kill_group();
	return check_queues_empty();
}

/* ------------------------------------------------------------------ tests */

static void sleep_forever(void *arg)
{
	(void)arg;
	for (;;)
		pause();
}

/* Policy plumbing that the starter code already provides. */
static int t_sanity(void)
{
	struct worker *w = spawn(sleep_forever, NULL);
	pid_t pid;
	int st, pol;

	CHECK(w, "fork failed");
	CHECK(sched_get_priority_max(SCHED_WRR) == 0 &&
	      sched_get_priority_min(SCHED_WRR) == 0,
	      "sched_get_priority_max/min(SCHED_WRR) should both be 0");

	CHECK(wrr_set_policy(w->pid) == 0,
	      "sched_setscheduler(SCHED_WRR) failed: %s", strerror(errno));
	pol = sched_getscheduler(w->pid);
	CHECK(pol == SCHED_WRR, "sched_getscheduler returned %d (%s), expected %d",
	      pol, policy_name(pol), SCHED_WRR);
	CHECK(set_policy(w->pid, SCHED_OTHER, 0) == 0, "switching back to SCHED_OTHER failed");

	errno = 0;
	CHECK(set_policy(w->pid, SCHED_WRR, 1) == -1 && errno == EINVAL,
	      "SCHED_WRR with sched_priority 1 should fail with EINVAL");
	errno = 0;
	CHECK(set_policy(w->pid, 8, 0) == -1 && errno == EINVAL,
	      "policy 8 should fail with EINVAL");

	/* Joining SCHED_WRR needs CAP_SYS_NICE. */
	pid = fork();
	if (pid == 0) {
		if (setuid(NOBODY_UID))
			_exit(2);
		_exit(wrr_set_policy(0) == -1 && errno == EPERM ? 0 : 1);
	}
	CHECK(wait_timeout(pid, 5000, &st), "unprivileged child did not finish");
	CHECK(exited_ok(st), "an unprivileged process could switch itself to SCHED_WRR "
	      "(expected EPERM)");
	return 1;
}

struct ratio {
	volatile double ratio;
	volatile int policy;
};

/* Runs on CPU 1, next to a CFS spinner. */
static void ratio_probe(void *arg)
{
	struct ratio *r = arg;

	r->policy = sched_getscheduler(0);
	r->ratio = cpu_ratio_on(1);
}

static void exec_true(void *arg)
{
	(void)arg;
	execl("/bin/true", "true", (char *)NULL);
	_exit(4);
}

/*
 * A WRR task runs, preempts CFS, is charged for its CPU time, execs and
 * exits.
 */
static int t_runs(void)
{
	struct ratio *r = shared_alloc(sizeof(*r));
	struct worker *w;
	int st, err;

	CHECK(cfs_hog(1), "could not start a CFS spinner");
	r->ratio = -1;
	w = spawn(ratio_probe, r);
	CHECK(w, "fork failed");
	set_cpus(w->pid, 1, 1);
	err = make_wrr(w->pid, 1);
	CHECK(!err, "sched_setscheduler(SCHED_WRR) failed: %s", strerror(err));
	release(w);
	CHECK(wait_timeout(w->pid, 5000, &st),
	      "a WRR task that should run for 60 ms did not finish in 5 s "
	      "(is it ever enqueued and picked?)");
	CHECK(exited_ok(st), "WRR task exited with status 0x%x", st);
	nworkers = 1;	/* keep the hog in the list so kill_group() finds it */
	info("WRR task next to a CFS spinner ran %.0f%% of the time", 100 * r->ratio);
	CHECK(r->ratio > 0.80,
	      "a WRR task sharing CPU 1 with a CFS spinner ran only %.0f%% of the "
	      "time; it should preempt CFS and run ~100%%. Is it in wrr_sched_class "
	      "(Part A), picked (B.3) and charged by update_curr_wrr (B.1)?",
	      100 * r->ratio);
	kill_group();

	w = spawn(exec_true, NULL);
	CHECK(w, "fork failed");
	set_cpus(w->pid, 1, 1);
	CHECK(!make_wrr(w->pid, 1), "sched_setscheduler(SCHED_WRR) failed");
	release(w);
	CHECK(wait_timeout(w->pid, 5000, &st), "a WRR task running /bin/true did not finish in 5 s");
	CHECK(exited_ok(st), "WRR task running /bin/true exited with status 0x%x", st);
	nworkers = 0;
	return check_queues_empty();
}

/* WRR outranks CFS; RT outranks WRR. */
static int t_class_order(void)
{
	struct worker *w[3];
	double share[3];
	int err;

	w[0] = spinner(1, SCHED_WRR, 1, &err);
	CHECK(w[0] && !err, "could not create a WRR spinner");
	w[1] = spinner(1, SCHED_WRR, 1, &err);
	CHECK(w[1] && !err, "could not create a WRR spinner");
	w[2] = spinner(1, SCHED_OTHER, 0, &err);
	CHECK(w[2], "could not create a CFS spinner");
	for (int i = 0; i < 3; i++)
		release(w[i]);
	if (!measure(w, 3, 300, 1000, share))
		return 0;
	info("two WRR spinners and a CFS spinner on one CPU: %.3f %.3f | CFS %.3f",
	     share[0], share[1], share[2]);
	CHECK(share[2] < 0.05, "a CFS spinner got %.3f of a CPU shared with two WRR "
	      "spinners; WRR must run ahead of CFS", share[2]);
	CHECK(share[0] > 0.35 && share[1] > 0.35, "two weight-1 WRR spinners should "
	      "split the CPU evenly, got %.3f / %.3f", share[0], share[1]);
	kill_group();

	w[0] = spinner(1, SCHED_FIFO, 0, &err);
	CHECK(w[0] && !err, "could not create a SCHED_FIFO spinner");
	w[1] = spinner(1, SCHED_WRR, 1, &err);
	CHECK(w[1] && !err, "could not create a WRR spinner");
	release(w[1]);
	sleep_ms(50);
	release(w[0]);
	if (!measure(w, 2, 300, 1000, share))
		return 0;
	info("FIFO vs WRR on one CPU: FIFO %.3f, WRR %.3f", share[0], share[1]);
	CHECK(share[0] > 0.85, "a SCHED_FIFO spinner got only %.3f of a CPU shared "
	      "with a WRR spinner; RT must run ahead of WRR", share[0]);
	kill_group();
	return check_queues_empty();
}

struct fork_report {
	double ratio;
	int policy;
	int weight;
	int weight_errno;
	int after_join_weight;
	int after_join_errno;
};

static void fork_and_report(void *arg)
{
	struct fork_report *r = arg;
	pid_t pid = fork();

	if (pid == 0) {
		r->policy = sched_getscheduler(0);
		errno = 0;
		r->weight = sched_getweight(0);
		r->weight_errno = errno;
		r->ratio = cpu_ratio_on(2);	/* a CFS spinner runs there */
		if (wrr_set_policy(0) == 0) {
			errno = 0;
			r->after_join_weight = sched_getweight(0);
			r->after_join_errno = errno;
		}
		/* Go back to SCHED_OTHER so exiting cannot depend on WRR. */
		set_policy(0, SCHED_OTHER, 0);
		_exit(0);
	}
	if (pid > 0)
		waitpid(pid, NULL, 0);
}

/* A WRR task's children are WRR tasks with the same weight. */
static int t_fork(void)
{
	struct fork_report *r = shared_alloc(sizeof(*r));
	struct worker *w = spawn(fork_and_report, r);
	int st, err, have_weights;

	CHECK(w, "fork failed");
	r->policy = -1;
	set_cpus(w->pid, 1, 1);
	CHECK(cfs_hog(2), "could not start a CFS spinner");
	err = make_wrr(w->pid, 1);
	CHECK(!err, "sched_setscheduler(SCHED_WRR) failed: %s", strerror(err));
	have_weights = sched_setweight(w->pid, 3) == 0;
	if (!have_weights)
		info("sched_setweight() failed (%s): checking the policy only",
		     strerror(errno));
	release(w);
	CHECK(wait_timeout(w->pid, 5000, &st), "a WRR task that forks did not finish in 5 s");
	CHECK(r->policy == SCHED_WRR,
	      "the child of a SCHED_WRR task has policy %d (%s), expected SCHED_WRR",
	      r->policy, policy_name(r->policy));
	info("forked child next to a CFS spinner ran %.0f%% of the time", 100 * r->ratio);
	CHECK(r->ratio > 0.80, "the child of a WRR task has policy SCHED_WRR but ran "
	      "only %.0f%% of the time next to a CFS spinner, like a CFS task: "
	      "sched_fork() must put it in wrr_sched_class (Part A.2)", 100 * r->ratio);
	if (have_weights)
		CHECK(r->weight == 3, "the child of a weight-3 task has weight %d "
		      "(errno %d), expected 3", r->weight, r->weight_errno);
	return 1;
}

/* SCHED_RESET_ON_FORK: children go back to SCHED_OTHER and the default weight. */
static int t_reset_on_fork(void)
{
	struct fork_report *r = shared_alloc(sizeof(*r));
	struct worker *w = spawn(fork_and_report, r);
	struct sched_param sp = { 0 };
	int st, pol, have_weights;

	CHECK(w, "fork failed");
	r->policy = -1;
	set_cpus(w->pid, 1, 1);
	CHECK(sched_setscheduler(w->pid, SCHED_WRR | SCHED_RESET_ON_FORK, &sp) == 0,
	      "sched_setscheduler(SCHED_WRR | SCHED_RESET_ON_FORK) failed: %s",
	      strerror(errno));
	pol = sched_getscheduler(w->pid);
	CHECK(pol == (SCHED_WRR | SCHED_RESET_ON_FORK),
	      "sched_getscheduler returned 0x%x, expected SCHED_WRR | SCHED_RESET_ON_FORK", pol);
	have_weights = sched_setweight(w->pid, 4) == 0;
	release(w);
	CHECK(wait_timeout(w->pid, 5000, &st), "a WRR task that forks did not finish in 5 s");
	nworkers = 0;
	CHECK(r->policy == SCHED_OTHER,
	      "with SCHED_RESET_ON_FORK the child should be SCHED_OTHER, got %d (%s)",
	      r->policy, policy_name(r->policy));
	if (have_weights)
		CHECK(r->after_join_weight == WRR_DEFAULT_WEIGHT,
		      "a reset-on-fork child of a weight-4 task joined SCHED_WRR with "
		      "weight %d (errno %d), expected the default weight %d",
		      r->after_join_weight, r->after_join_errno, WRR_DEFAULT_WEIGHT);
	else
		FAIL("sched_setweight() does not work yet (%s)", strerror(errno));
	return 1;
}

static long rr_interval_ms(pid_t pid)
{
	struct timespec ts;

	if (sched_rr_get_interval(pid, &ts))
		return -1;
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* sched_rr_get_interval() reports weight * 10 ms. */
static int t_rr_interval(void)
{
	struct worker *w = spawn(sleep_forever, NULL);
	const int weights[] = { 1, 5, 20, 2 };

	CHECK(w, "fork failed");
	CHECK(wrr_set_policy(w->pid) == 0, "sched_setscheduler(SCHED_WRR) failed");
	for (unsigned i = 0; i < sizeof(weights) / sizeof(weights[0]); i++) {
		long want = weights[i] * WRR_BASE_SLICE_MS, got;

		if (weights[i] != 1)
			CHECK(sched_setweight(w->pid, weights[i]) == 0,
			      "sched_setweight(pid, %d) failed: %s", weights[i], strerror(errno));
		got = rr_interval_ms(w->pid);
		CHECK(got >= want - 4 && got <= want + 4,
		      "weight %d: sched_rr_get_interval says %ld ms, expected %ld ms",
		      weights[i], got, want);
	}
	set_policy(w->pid, SCHED_OTHER, 0);
	kill_group();
	return 1;
}

static pid_t unused_pid(void)
{
	pid_t pid = fork();

	if (pid == 0)
		_exit(0);
	waitpid(pid, NULL, 0);
	return pid;
}

struct self_report {
	int r1, e1, r2, e2, r3, e3;
};

static void self_weight(void *arg)
{
	struct self_report *r = arg;

	errno = 0;
	r->r1 = sched_getweight(0);
	r->e1 = errno;
	errno = 0;
	r->r2 = sched_setweight(0, 2);
	r->e2 = errno;
	errno = 0;
	r->r3 = sched_getweight(0);
	r->e3 = errno;
}

/* sched_setweight / sched_getweight: values and error codes. */
static int t_weight_syscalls(void)
{
	struct self_report *r = shared_alloc(sizeof(*r));
	struct worker *w = spawn(sleep_forever, NULL), *s;
	pid_t gone = unused_pid();
	int v, st;

	CHECK(w, "fork failed");

	errno = 0;
	v = sched_getweight(w->pid);
	CHECK(v == -1 && errno == EINVAL,
	      "sched_getweight() of a SCHED_OTHER task returned %d (%s), expected EINVAL",
	      v, strerror(errno));
	errno = 0;
	v = sched_setweight(w->pid, 3);
	CHECK(v == -1 && errno == EINVAL,
	      "sched_setweight() of a SCHED_OTHER task returned %d (%s), expected EINVAL",
	      v, strerror(errno));

	CHECK(wrr_set_policy(w->pid) == 0, "sched_setscheduler(SCHED_WRR) failed");
	v = sched_getweight(w->pid);
	CHECK(v == WRR_DEFAULT_WEIGHT, "a new WRR task has weight %d (%s), expected %d",
	      v, v < 0 ? strerror(errno) : "ok", WRR_DEFAULT_WEIGHT);
	CHECK(sched_setweight(w->pid, 7) == 0, "sched_setweight(pid, 7) failed: %s",
	      strerror(errno));
	v = sched_getweight(w->pid);
	CHECK(v == 7, "after sched_setweight(pid, 7), sched_getweight returned %d", v);
	CHECK(sched_setweight(w->pid, WRR_MAX_WEIGHT) == 0 &&
	      sched_getweight(w->pid) == WRR_MAX_WEIGHT,
	      "weight %d (the maximum) should be accepted", WRR_MAX_WEIGHT);
	CHECK(sched_setweight(w->pid, 7) == 0, "sched_setweight(pid, 7) failed");

	const int bad[] = { 0, WRR_MAX_WEIGHT + 1, -3, 1000 };
	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		errno = 0;
		v = sched_setweight(w->pid, bad[i]);
		CHECK(v == -1 && errno == EINVAL,
		      "sched_setweight(pid, %d) returned %d (%s), expected EINVAL",
		      bad[i], v, strerror(errno));
	}
	CHECK(sched_getweight(w->pid) == 7, "a rejected sched_setweight() changed the weight");

	errno = 0;
	CHECK(sched_setweight(-1, 3) == -1 && errno == EINVAL,
	      "sched_setweight(-1, 3) should fail with EINVAL (got %s)", strerror(errno));
	errno = 0;
	CHECK(sched_getweight(-1) == -1 && errno == EINVAL,
	      "sched_getweight(-1) should fail with EINVAL (got %s)", strerror(errno));
	errno = 0;
	CHECK(sched_getweight(gone) == -1 && errno == ESRCH,
	      "sched_getweight(no such pid) should fail with ESRCH (got %s)", strerror(errno));
	errno = 0;
	CHECK(sched_setweight(gone, 2) == -1 && errno == ESRCH,
	      "sched_setweight(no such pid, 2) should fail with ESRCH (got %s)", strerror(errno));

	set_policy(w->pid, SCHED_OTHER, 0);
	kill_group();

	/* pid 0 means the caller. The caller must be a running WRR task. */
	s = spawn(self_weight, r);
	CHECK(s, "fork failed");
	set_cpus(s->pid, 1, 1);
	CHECK(!make_wrr(s->pid, 5), "could not make a weight-5 WRR task");
	release(s);
	CHECK(wait_timeout(s->pid, 5000, &st), "a WRR task calling sched_getweight(0) "
	      "did not finish in 5 s");
	nworkers = 0;
	CHECK(r->r1 == 5, "sched_getweight(0) in a weight-5 task returned %d (%s)",
	      r->r1, strerror(r->e1));
	CHECK(r->r2 == 0 && r->r3 == 2, "sched_setweight(0, 2) then sched_getweight(0) "
	      "returned %d (%s), %d (%s)", r->r2, strerror(r->e2), r->r3, strerror(r->e3));
	return 1;
}

struct perm_ctl {
	int ready;
	pid_t target;
	int r_other, e_other;		/* nobody changes root's task */
	int r_lower, e_lower;		/* nobody lowers its own task */
	int r_raise, e_raise;		/* nobody raises its own task */
	int r_same, e_same;		/* nobody sets its own task's weight unchanged */
	int r_get;
};

/*
 * The owner of a task may lower its weight. Raising it, or changing
 * someone else's task, needs CAP_SYS_NICE.
 */
static int t_weight_perm(void)
{
	struct perm_ctl *c = shared_alloc(sizeof(*c));
	struct worker *root_task, *nobody_task;
	pid_t pid;
	int st;

	root_task = spawn(sleep_forever, NULL);
	CHECK(root_task, "fork failed");
	CHECK(!make_wrr(root_task->pid, 5), "could not make a weight-5 WRR task "
	      "(sched_setweight: %s)", strerror(errno));

	/* A task owned by nobody (uid 65534). */
	pid = fork();
	if (pid == 0) {
		if (setresuid(NOBODY_UID, NOBODY_UID, NOBODY_UID))
			_exit(2);
		sleep_forever(NULL);
	}
	CHECK(pid > 0, "fork failed");
	workers[nworkers].pid = pid;
	workers[nworkers].go_fd = -1;
	nobody_task = &workers[nworkers++];
	for (int i = 0; i < 100; i++) {
		char path[64];
		FILE *f;
		unsigned uid = 0;
		char line[128];

		snprintf(path, sizeof(path), "/proc/%d/status", pid);
		f = fopen(path, "r");
		while (f && fgets(line, sizeof(line), f))
			if (sscanf(line, "Uid: %*u %u", &uid) == 1)
				break;
		if (f)
			fclose(f);
		if (uid == NOBODY_UID)
			break;
		sleep_ms(10);
	}
	CHECK(!make_wrr(nobody_task->pid, 5), "could not make a weight-5 WRR task");

	/* An unprivileged process, also uid nobody, tries its luck. */
	pid = fork();
	if (pid == 0) {
		if (setresuid(NOBODY_UID, NOBODY_UID, NOBODY_UID))
			_exit(2);
		errno = 0; c->r_other = sched_setweight(root_task->pid, 3); c->e_other = errno;
		errno = 0; c->r_lower = sched_setweight(nobody_task->pid, 3); c->e_lower = errno;
		errno = 0; c->r_raise = sched_setweight(nobody_task->pid, 4); c->e_raise = errno;
		errno = 0; c->r_same = sched_setweight(nobody_task->pid, 3); c->e_same = errno;
		c->r_get = sched_getweight(root_task->pid);
		_exit(0);
	}
	CHECK(wait_timeout(pid, 5000, &st) && exited_ok(st), "unprivileged child failed");

	CHECK(c->r_other == -1 && c->e_other == EPERM,
	      "an unprivileged user changed the weight of another user's task "
	      "(got %d, %s; expected EPERM)", c->r_other, strerror(c->e_other));
	CHECK(c->r_lower == 0, "the owner could not lower its task's weight from 5 to 3 (%s)",
	      strerror(c->e_lower));
	CHECK(c->r_raise == -1 && c->e_raise == EPERM,
	      "an unprivileged owner raised its task's weight from 3 to 4 "
	      "(got %d, %s; expected EPERM)", c->r_raise, strerror(c->e_raise));
	CHECK(c->r_same == 0, "setting an unchanged weight should be allowed (%s)",
	      strerror(c->e_same));
	CHECK(c->r_get == 5, "anyone may read a weight: sched_getweight returned %d", c->r_get);
	CHECK(sched_getweight(nobody_task->pid) == 3, "weight should now be 3");
	CHECK(sched_setweight(nobody_task->pid, 9) == 0,
	      "root (CAP_SYS_NICE) could not raise another user's task's weight (%s)",
	      strerror(errno));
	set_policy(root_task->pid, SCHED_OTHER, 0);
	set_policy(nobody_task->pid, SCHED_OTHER, 0);
	kill_group();
	return 1;
}

static int t_share_equal(void)
{
	const int two[] = { 1, 1 }, three[] = { 1, 1, 1 };

	if (!check_shares(1, 2, two, 1500, 0, 0.08))
		return 0;
	return check_shares(2, 3, three, 1500, 0, 0.08);
}

static int t_share_weighted(void)
{
	const int a[] = { 1, 2, 3 }, b[] = { 1, 4 };

	if (!check_shares(1, 3, a, 3000, 0.25, 0.03))
		return 0;
	return check_shares(2, 2, b, 2000, 0.25, 0.03);
}

/* Changing the weight of a running task changes its share. */
static int t_weight_change(void)
{
	struct worker *w[2];
	double share[2];
	int err;

	for (int i = 0; i < 2; i++) {
		w[i] = spinner(1, SCHED_WRR, 1, &err);
		CHECK(w[i] && !err, "could not create a WRR spinner");
	}
	release(w[0]);
	release(w[1]);
	if (!measure(w, 2, 300, 1000, share))
		return 0;
	info("both weight 1: %.3f %.3f", share[0], share[1]);
	CHECK(share[0] > 0.40 && share[0] < 0.60, "two weight-1 tasks should split "
	      "the CPU evenly, got %.3f / %.3f", share[0], share[1]);

	CHECK(sched_setweight(w[0]->pid, 3) == 0, "sched_setweight(running task, 3) "
	      "failed: %s", strerror(errno));
	if (!measure(w, 2, 200, 2000, share))
		return 0;
	info("after changing one to weight 3: %.3f %.3f", share[0], share[1]);
	CHECK(share[0] > 0.67 && share[0] < 0.83, "after sched_setweight(3) the task "
	      "should get 0.75 of the CPU, got %.3f", share[0]);

	CHECK(sched_setweight(w[0]->pid, 1) == 0 && sched_setweight(w[1]->pid, 4) == 0,
	      "sched_setweight failed: %s", strerror(errno));
	if (!measure(w, 2, 200, 2000, share))
		return 0;
	info("after 1 vs 4: %.3f %.3f", share[0], share[1]);
	CHECK(share[1] > 0.72 && share[1] < 0.88, "a weight-4 task next to a weight-1 "
	      "task should get 0.80 of the CPU, got %.3f", share[1]);
	kill_group();
	return check_queues_empty();
}

/* sched_yield() gives up the rest of the turn. */
static int t_yield(void)
{
	struct worker *w[2];
	double share[2];
	int err;

	w[0] = spawn(yield_forever, NULL);
	CHECK(w[0], "fork failed");
	set_cpus(w[0]->pid, 1, 1);
	CHECK(!make_wrr(w[0]->pid, 1), "sched_setscheduler failed");
	w[1] = spinner(1, SCHED_WRR, 1, &err);
	CHECK(w[1] && !err, "could not create a WRR spinner");
	release(w[0]);
	release(w[1]);
	if (!measure(w, 2, 300, 1500, share))
		return 0;
	info("yielder %.3f, spinner %.3f", share[0], share[1]);
	CHECK(share[0] < 0.20, "a task that calls sched_yield() in a loop got %.3f of the "
	      "CPU next to a spinner of the same weight; yielding should move it to "
	      "the back of the queue", share[0]);
	kill_group();
	return check_queues_empty();
}

static void nap_200(void *arg)
{
	struct ratio *r = arg;

	for (int i = 0; i < 200; i++)
		sleep_ms(1);
	r->ratio = cpu_ratio_on(1);	/* still WRR after 200 wake-ups? */
}

struct pingpong {
	int to_b[2], to_a[2];
};

static void ping(void *arg)
{
	struct pingpong *pp = arg;
	char c = 'x';

	for (int i = 0; i < 2000; i++)
		if (write(pp->to_b[1], &c, 1) != 1 || read(pp->to_a[0], &c, 1) != 1)
			_exit(5);
}

static void pong(void *arg)
{
	struct pingpong *pp = arg;
	char c;

	for (int i = 0; i < 2000; i++)
		if (read(pp->to_b[0], &c, 1) != 1 || write(pp->to_a[1], &c, 1) != 1)
			_exit(5);
}

/* Tasks that sleep and wake up keep working. */
static int t_sleepers(void)
{
	struct ratio *r = shared_alloc(sizeof(*r));
	struct pingpong pp;
	struct worker *a, *b;
	long long t0;
	int st;

	r->ratio = -1;
	a = spawn(nap_200, r);
	CHECK(a, "fork failed");
	set_cpus(a->pid, 1, 1);
	CHECK(!make_wrr(a->pid, 1), "sched_setscheduler failed");
	t0 = now_ns();
	release(a);
	CHECK(wait_timeout(a->pid, 10000, &st), "a WRR task doing 200 x 1 ms sleeps "
	      "did not finish in 10 s (does a task that wakes up get enqueued again?)");
	CHECK(exited_ok(st), "sleeper exited with status 0x%x", st);
	info("200 x 1 ms sleeps took %lld ms", (now_ns() - t0) / 1000000);
	nworkers = 0;
	CHECK(cfs_hog(1), "could not start a CFS spinner");
	a = spawn(nap_200, r);
	CHECK(a, "fork failed");
	set_cpus(a->pid, 1, 1);
	CHECK(!make_wrr(a->pid, 1), "sched_setscheduler failed");
	release(a);
	CHECK(wait_timeout(a->pid, 10000, &st) && exited_ok(st),
	      "a WRR task sleeping next to a CFS spinner did not finish in 10 s");
	info("after 200 wake-ups next to a CFS spinner it ran %.0f%% of the time",
	     100 * r->ratio);
	CHECK(r->ratio > 0.80, "after sleeping and waking up 200 times, a WRR task "
	      "ran only %.0f%% of the time next to a CFS spinner; a waking task "
	      "must go back on the WRR queue", 100 * r->ratio);
	kill_group();

	CHECK(!pipe(pp.to_b) && !pipe(pp.to_a), "pipe failed");
	a = spawn(ping, &pp);
	b = spawn(pong, &pp);
	CHECK(a && b, "fork failed");
	for (int i = 0; i < 2; i++) {
		set_cpus(workers[i].pid, 1, 1);
		CHECK(!make_wrr(workers[i].pid, 1), "sched_setscheduler failed");
	}
	t0 = now_ns();
	release(a);
	release(b);
	CHECK(wait_timeout(a->pid, 15000, &st) && exited_ok(st),
	      "two WRR tasks passing a byte back and forth 2000 times did not finish "
	      "in 15 s");
	CHECK(wait_timeout(b->pid, 2000, &st) && exited_ok(st), "pong task did not finish");
	info("2000 pipe round trips took %lld ms", (now_ns() - t0) / 1000000);
	nworkers = 0;
	return check_queues_empty();
}

struct where {
	volatile int cpu[MAX_WORKERS];
};

static struct where *where;

static void report_cpu(void *arg)
{
	int idx = (int)(long)arg;

	for (;;)
		where->cpu[idx] = sched_getcpu();
}

/*
 * Create @n spinners with @weights on CPU 1, let them use CPUs 1-3, then
 * wake them one at a time. Fills @cpu_of[i] with where each one ended up.
 */
static int place(int n, const int weights[], int cpu_of[])
{
	struct worker *w[MAX_WORKERS];
	int err;

	for (int i = 0; i < n; i++) {
		where->cpu[i] = -1;
		w[i] = spawn(report_cpu, (void *)(long)i);
		CHECK(w[i], "fork failed");
		set_cpus(w[i]->pid, 1, 1);
		err = make_wrr(w[i]->pid, weights[i]);
		CHECK(!err, "could not make a weight-%d WRR task: %s", weights[i],
		      strerror(err));
		/* Allowed on 1-3 from now on, but it is asleep, so it stays put. */
		set_cpus(w[i]->pid, 1, 3);
	}
	for (int i = 0; i < n; i++) {
		release(w[i]);
		sleep_ms(40);
	}
	sleep_ms(300);
	for (int i = 0; i < n; i++)
		cpu_of[i] = where->cpu[i];
	return 1;
}

/* Does rq->wrr.nr_running on CPUs 1-3 agree with where the tasks run? */
static int queues_match(const int count[4])
{
	long nr[MAX_CPUS] = { 0 }, tw[MAX_CPUS] = { 0 };

	if (read_wrr_debug(nr, tw) < 4)
		return 1;
	for (int c = 1; c <= 3; c++)
		CHECK(nr[c] == count[c], "%d WRR task(s) run on CPU %d but its rq->wrr "
		      "says nr_running=%ld", count[c], c, nr[c]);
	return 1;
}

/* New and waking WRR tasks go to the CPU with the least WRR weight. */
static int t_smp_spread(void)
{
	const int same[] = { 1, 1, 1, 1, 1, 1 }, mixed[] = { 4, 1, 1, 1, 1 };
	int cpu_of[MAX_WORKERS], count[4], ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
	char buf[128] = "";

	CHECK(ncpu >= 4, "this test needs at least 4 CPUs (found %d)", ncpu);
	where = shared_alloc(sizeof(*where));

	if (!place(6, same, cpu_of))
		return 0;
	memset(count, 0, sizeof(count));
	for (int i = 0; i < 6; i++) {
		CHECK(cpu_of[i] >= 1 && cpu_of[i] <= 3, "task %d ran on CPU %d, outside its "
		      "allowed CPUs 1-3 (or never ran)", i, cpu_of[i]);
		count[cpu_of[i]]++;
	}
	info("six weight-1 tasks on CPUs 1/2/3: %d %d %d", count[1], count[2], count[3]);
	if (!queues_match(count))
		return 0;
	CHECK(count[1] == 2 && count[2] == 2 && count[3] == 2,
	      "six weight-1 tasks woken one at a time should end up 2-2-2 on CPUs 1-3, "
	      "got %d-%d-%d (select_task_rq_wrr)", count[1], count[2], count[3]);
	kill_group();
	if (!check_queues_empty())
		return 0;

	if (!place(5, mixed, cpu_of))
		return 0;
	memset(count, 0, sizeof(count));
	for (int i = 0; i < 5; i++) {
		CHECK(cpu_of[i] >= 1 && cpu_of[i] <= 3, "task %d ran on CPU %d", i, cpu_of[i]);
		count[cpu_of[i]]++;
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " w%d->cpu%d",
			 mixed[i], cpu_of[i]);
	}
	info("weights 4,1,1,1,1:%s", buf);
	if (!queues_match(count))
		return 0;
	CHECK(count[cpu_of[0]] == 1, "the weight-4 task shares CPU %d with %d other "
	      "task(s); placement should balance total weight (4 | 1+1 | 1+1)",
	      cpu_of[0], count[cpu_of[0]] - 1);
	kill_group();
	return check_queues_empty();
}

struct chaos {
	long long deadline;
	unsigned seed;
	volatile int done[MAX_WORKERS];
};

static struct chaos *chaos;

static void chaos_task(void *arg)
{
	int idx = (int)(long)arg;
	unsigned seed = chaos->seed * 7919u + idx;

	while (now_ns() < chaos->deadline) {
		int op = rand_r(&seed) % 9;

		switch (op) {
		case 0: case 1: { /* spin a little */
			long long end = now_ns() + (rand_r(&seed) % 15) * 1000000LL;

			while (now_ns() < end)
				;
			break;
		}
		case 2:
			sleep_ms(rand_r(&seed) % 5);
			break;
		case 3:
			sched_yield();
			break;
		case 4:
			sched_setweight(0, 1 + rand_r(&seed) % WRR_MAX_WEIGHT);
			break;
		case 5:
			if (sched_getscheduler(0) == SCHED_WRR)
				set_policy(0, SCHED_OTHER, 0);
			else
				wrr_set_policy(0);
			break;
		case 6: {
			int first = 1 + rand_r(&seed) % 3;
			int last = first + rand_r(&seed) % (4 - first);

			set_cpus(0, first, last);
			break;
		}
		case 7: { /* fork a short-lived child */
			pid_t pid = fork();

			if (pid == 0) {
				long long end = now_ns() + (rand_r(&seed) % 5) * 1000000LL;

				while (now_ns() < end)
					;
				_exit(0);
			}
			if (pid > 0)
				waitpid(pid, NULL, 0);
			break;
		}
		default:
			sched_setweight(getppid(), 1 + rand_r(&seed) % WRR_MAX_WEIGHT);
			break;
		}
	}
	/* Leave as a WRR task so that exit goes through the WRR class. */
	wrr_set_policy(0);
	chaos->done[idx] = 1;
}

/* Many tasks forking, sleeping, yielding, migrating and changing weight. */
static int t_stress(void)
{
	const int n = 24;
	const char *s = getenv("WRR_SEED");
	struct worker *w[MAX_WORKERS];
	int left = 0, st;

	chaos = shared_alloc(sizeof(*chaos));
	chaos->seed = s ? (unsigned)atoi(s) : 3753;
	for (int i = 0; i < n; i++) {
		w[i] = spawn(chaos_task, (void *)(long)i);
		CHECK(w[i], "fork failed");
		set_cpus(w[i]->pid, 1, 3);
		CHECK(!make_wrr(w[i]->pid, 1 + i % 5), "could not make a WRR task "
		      "with weight %d", 1 + i % 5);
	}
	chaos->deadline = now_ns() + 3000000000LL;
	info("24 tasks, 3 s, seed %u", chaos->seed);
	for (int i = 0; i < n; i++)
		release(w[i]);
	sleep_ms(3000);
	for (int i = 0; i < n; i++)
		if (!wait_timeout(w[i]->pid, i ? 2000 : 12000, &st) || !exited_ok(st))
			left++;
	CHECK(!left, "%d of %d tasks did not finish within 12 s of the end of "
	      "the run (lost, starved or crashed)", left, n);
	nworkers = 0;
	return check_queues_empty();
}

/* ----------------------------------------------------------------- runner */

struct test {
	const char *name;
	int points;
	int timeout_s;
	int (*fn)(void);
	const char *what;
};

static const struct test tests[] = {
	{ "sanity",          0, 20, t_sanity,          "policy plumbing (given in the starter)" },
	{ "runs",            5, 30, t_runs,            "A, B.1-B.4: a WRR task runs, execs, exits, is charged" },
	{ "class_order",     6, 30, t_class_order,     "A, B: RT > WRR > CFS" },
	{ "fork",            4, 20, t_fork,            "A.2, B.6: children inherit policy and weight" },
	{ "reset_on_fork",   3, 20, t_reset_on_fork,   "A.1, C: SCHED_RESET_ON_FORK" },
	{ "rr_interval",     4, 20, t_rr_interval,     "B.6, C: sched_rr_get_interval" },
	{ "weight_syscalls", 8, 30, t_weight_syscalls, "C: values and error codes" },
	{ "weight_perm",     4, 20, t_weight_perm,     "C: permission rules" },
	{ "share_equal",     6, 40, t_share_equal,     "B: equal weights share equally" },
	{ "share_weighted",  8, 40, t_share_weighted,  "B, C: shares follow weights" },
	{ "weight_change",   5, 40, t_weight_change,   "B, C: changing a running task's weight" },
	{ "yield",           4, 30, t_yield,           "B.5: sched_yield" },
	{ "sleepers",        4, 40, t_sleepers,        "B.2: sleep and wake-up" },
	{ "smp_spread",      8, 40, t_smp_spread,      "B.7, C: placement across CPUs" },
	{ "stress",          6, 60, t_stress,          "everything at once" },
};
#define NTESTS ((int)(sizeof(tests) / sizeof(tests[0])))

/* Make the calling process a SCHED_FIFO controller pinned to CPU 0. */
static void become_controller(void)
{
	struct sched_param sp = { .sched_priority = 50 };

	set_cpus(0, 0, 0);
	if (sched_setscheduler(0, SCHED_FIFO | SCHED_RESET_ON_FORK, &sp))
		perror("warning: cannot become SCHED_FIFO");
}

static int kmsg_open(void)
{
	int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);

	if (fd >= 0)
		lseek(fd, 0, SEEK_END);
	return fd;
}

/* Print kernel problems logged since kmsg_open(). Returns how many. */
static int kmsg_problems(int fd)
{
	static const char *bad[] = { "WARNING:", "BUG:", "Oops", "lockdep",
				     "INFO: task", "rcu: INFO", "stall", "general protection" };
	char rec[2048];
	int found = 0;
	ssize_t n;

	if (fd < 0)
		return 0;
	while ((n = read(fd, rec, sizeof(rec) - 1)) > 0 || (n < 0 && errno == EPIPE)) {
		char *msg;

		if (n <= 0)
			continue;
		rec[n] = 0;
		msg = strchr(rec, ';');
		msg = msg ? msg + 1 : rec;
		for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
			if (strstr(msg, bad[i])) {
				if (found < 8)
					printf("         kernel: %s", msg);
				found++;
				break;
			}
	}
	close(fd);
	return found;
}

static int run_one(const struct test *t, int *earned)
{
	int kfd = kmsg_open(), st = 0, ok, timed_out = 0, problems;
	long long t0 = now_ns();
	pid_t pid;

	fail_msg[0] = 0;
	fflush(stdout);
	pid = fork();
	if (pid == 0) {
		setpgid(0, 0);
		become_controller();
		ok = t->fn();
		kill_group();
		fflush(stdout);
		_exit(ok ? 0 : 1);
	}
	setpgid(pid, pid);
	if (!wait_timeout(pid, t->timeout_s * 1000L, &st)) {
		timed_out = 1;
		kill(-pid, SIGKILL);
		wait_timeout(pid, 3000, &st);
		snprintf(fail_msg, 400, "timed out after %d s", t->timeout_s);
	}
	problems = kmsg_problems(kfd);
	ok = !timed_out && exited_ok(st) && !problems;
	if (!timed_out && !exited_ok(st) && !fail_msg[0])
		snprintf(fail_msg, 400, "test process died (status 0x%x)", st);
	if (problems && ok == 0 && !fail_msg[0])
		snprintf(fail_msg, 400, "the kernel reported %d problem(s) (see above)", problems);
	else if (problems)
		snprintf(fail_msg + strlen(fail_msg), 400 - strlen(fail_msg),
			 "%sthe kernel reported %d problem(s)", fail_msg[0] ? "; " : "", problems);

	printf("[ %s ] %-16s %2d/%-2d  %5.1fs  %s\n", ok ? "PASS" : "FAIL", t->name,
	       ok ? t->points : 0, t->points, (now_ns() - t0) / 1e9, t->what);
	if (!ok)
		printf("         -> %s\n", fail_msg);
	fflush(stdout);
	*earned += ok ? t->points : 0;
	return ok;
}

int main(int argc, char **argv)
{
	int earned = 0, total = 0, passed = 0, ran = 0;
	struct utsname u;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc > 1 && !strcmp(argv[1], "list")) {
		for (int i = 0; i < NTESTS; i++)
			printf("%-16s %2d pts  %s\n", tests[i].name, tests[i].points, tests[i].what);
		return 0;
	}
	if (geteuid() != 0) {
		fprintf(stderr, "wrr-test must run as root\n");
		return 2;
	}
	fail_msg = shared_alloc(512);
	uname(&u);
	printf("WRR-TEST BEGIN kernel %s, %ld CPUs\n", u.release, sysconf(_SC_NPROCESSORS_ONLN));
	if (sysconf(_SC_NPROCESSORS_ONLN) < 4)
		printf("warning: fewer than 4 CPUs; several tests will fail\n");
	become_controller();

	for (int i = 0; i < NTESTS; i++) {
		int want = argc < 2 || (argc == 2 && !strcmp(argv[1], "all"));

		for (int a = 1; a < argc && !want; a++)
			want = !strcmp(argv[a], tests[i].name);
		if (!want)
			continue;
		ran++;
		total += tests[i].points;
		passed += run_one(&tests[i], &earned);
	}
	if (!ran) {
		fprintf(stderr, "no such test; try: wrr-test list\n");
		return 2;
	}
	printf("WRR-TEST PASSED %d/%d\n", passed, ran);
	printf("WRR-TEST SCORE %d/%d\n", earned, total);
	return passed == ran ? 0 : 1;
}
