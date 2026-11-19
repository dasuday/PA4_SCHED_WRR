# SCHED_WRR: A Weighted Round-Robin Scheduler for Linux

## Introduction
In this assignment, you will add a new scheduling policy to the Linux kernel, rebuild the kernel, boot it in a virtual machine, and measure how your scheduler behaves. The policy is **weighted round-robin** (`SCHED_WRR`): every task gets a turn on the CPU in a fixed order, and a task's *weight* decides how long its turn is. A task of weight 3 runs three times as long per turn as a task of weight 1, so when both are busy it gets three times as much CPU.

You have seen round-robin in lectures as a textbook algorithm. Here you will write the real thing, inside the scheduler of a real operating system, where a single mistake freezes or crashes the machine. That is why you will do all of your testing in QEMU, a virtual machine that boots your kernel in a few seconds and can crash as often as it likes.

The assignment is adapted from scheduler assignments used at other universities (see [Sources](#sources)): Columbia's W4118 "Freezer" round-robin scheduler, Duke's group weighted round-robin (GWRR) project, and the University of Crete's HY-345 assignments, which add scheduling system calls to Linux and test them in QEMU.

### Weighted round-robin
Each CPU keeps a queue of the `SCHED_WRR` tasks that are ready to run. The task at the head of the queue runs. A task of weight *w* may run for a *time slice* of *w* × 10 ms. When its slice is used up, the task goes to the back of the queue and the next task gets its turn. A task that blocks (for example, to read from a pipe) leaves the queue, and joins the back of it again when it wakes up, keeping whatever was left of its slice.

When every task on a CPU is busy, one *round* takes the sum of all the slices, and in each round each task runs once. A task's share of the CPU is therefore

```
share(i) = w(i) / (w(1) + w(2) + ... + w(n))
```

For example, three busy tasks with weights 1, 2 and 3 on one CPU should get 1/6, 2/6 and 3/6 of it. A round takes 60 ms: 10 ms for the first task, 20 ms for the second and 30 ms for the third.

### How Linux chooses the next task to run
Linux supports several scheduling policies at once. Each policy is implemented by a *scheduling class*, and the classes have a fixed priority order. When a CPU needs a new task, the scheduler core asks each class in turn, highest first, "do you have a task that is ready to run?" and runs the first one it gets. In Linux 6.8 the classes are:

| Class | Policies | Used for |
|---|---|---|
| `stop_sched_class` | (none) | Internal: stopping a CPU, migrating tasks |
| `dl_sched_class` | `SCHED_DEADLINE` | Real-time tasks with deadlines |
| `rt_sched_class` | `SCHED_FIFO`, `SCHED_RR` | Fixed-priority real-time tasks |
| **`wrr_sched_class`** | **`SCHED_WRR`** | **Your scheduler** |
| `fair_sched_class` | `SCHED_NORMAL`, `SCHED_BATCH`, `SCHED_IDLE` | Almost everything (the "Completely Fair Scheduler"; since Linux 6.6 it chooses tasks with the EEVDF algorithm) |
| `idle_sched_class` | (none) | The idle task that runs when nothing else can |

Your class sits between the real-time class and the fair class. A `SCHED_WRR` task always runs before any normal (CFS) task on the same CPU, and a `SCHED_FIFO` or `SCHED_RR` task always runs before it.

A scheduling class is a `struct sched_class`: a table of function pointers that the scheduler core calls at the right moments. For example, `enqueue_task` is called when a task becomes ready to run, `pick_next_task` when the CPU needs a task, and `task_tick` on every timer interrupt while one of the class's tasks is running. This is the same interface-and-implementation idea as a C header and its `.c` file, only with function pointers. Each class keeps its own data in two places:
- per task: a field of `struct task_struct` (`p->se` for CFS, `p->rt` for real-time; yours is `p->wrr`);
- per CPU: a field of the CPU's run queue, `struct rq` (`rq->cfs`, `rq->rt`; yours is `rq->wrr`).

`kernel/sched/rt.c` implements `SCHED_RR`, which is plain round-robin with priorities. It is the closest relative of your scheduler and the best place to see how a class is written. Read `task_tick_rt()`, `yield_task_rt()` and `requeue_task_rt()` before you start.

## The SCHED_WRR specification
- **Policy.** `SCHED_WRR` is policy number 7. A task joins it with `sched_setscheduler(pid, SCHED_WRR, &param)`, where `param.sched_priority` must be 0. Joining needs the `CAP_SYS_NICE` capability (in practice, root), because a `SCHED_WRR` task runs ahead of every normal task.
- **Weights.** A weight is an integer from 1 to 20. Every task has one, 1 by default, but it only matters while the task's policy is `SCHED_WRR`. A task keeps its weight if it leaves `SCHED_WRR` and later returns.
- **Time slices.** A task of weight *w* runs for *w* × 10 ms per turn. The VM's kernel ticks 1000 times a second, so that is *w* × 10 ticks. `sched_rr_get_interval()` on a `SCHED_WRR` task reports this length.
- **Order.** Tasks on a CPU take turns in first-come, first-served order. A task that wakes up goes to the back of the queue, and does not preempt a `SCHED_WRR` task that is running. A task that calls `sched_yield()` gives up the rest of its turn and goes to the back of the queue.
- **Weight changes.** A new weight takes effect from the task's next turn. If the rest of the current turn is longer than a full slice at the new weight, the turn is cut short to that length.
- **fork.** A child gets its parent's policy and weight, so the child of a `SCHED_WRR` task is a `SCHED_WRR` task with the same weight. If the parent set `SCHED_RESET_ON_FORK`, the child is instead a `SCHED_NORMAL` task, and its weight goes back to 1.
- **Multiple CPUs.** Each CPU has its own queue. When a task is created, wakes up or calls `exec()`, it goes to the CPU with the smallest total `SCHED_WRR` weight among the CPUs it is allowed to use (its *affinity*); ties go to the CPU it ran on last. Tasks do not move between CPUs at any other time (there is no load balancing; see [Going further](#going-further)).
- **System calls.** Two new system calls read and change weights. They are described in [Part C](#part-c-the-weight-system-calls).

`SCHED_WRR` has no safety net: nothing stops a busy `SCHED_WRR` task from taking 100% of a CPU forever. Normal tasks on that CPU, including your shell if it is there, then never run. (Real-time tasks have such a limit: by default they may use at most 95% of each second.) Keep this in mind when you experiment.

## What you are given
This repository contains everything except the kernel source, which `make get-kernel` downloads.

| Path | What it is |
|---|---|
| `kernel/starter.patch` | The starter code, applied to Linux 6.8.12 by `make get-kernel` |
| `kernel/qemu-*.config` | The settings for the test VM's kernel (small, with scheduler debugging on) |
| `scripts/` | Download, build, boot, test and package scripts (used by the `Makefile`) |
| `user/include/wrr.h` | `SCHED_WRR`, the system call numbers and C wrappers for them |
| `user/tests/wrr-test.c` | The test suite |
| `user/tools/` | `wrr-run`, `wrr-weight` and `spin`, for trying things by hand |
| `user/experiments/` | `wrr-share` and `wrr-latency`, for Part D |
| `initramfs/init` | The VM's start-up script |
| `report/REPORT.md` | A template for your report |

The starter code already does all of the plumbing, so that you can concentrate on the scheduler itself:
- `SCHED_WRR` is defined in `include/uapi/linux/sched.h`, and the kernel accepts it in `sched_setscheduler()`.
- `struct sched_wrr_entity` and the weight and slice constants (`WRR_MAX_WEIGHT`, `WRR_TIMESLICE(w)`, ...) are in `include/linux/sched.h`. Every task has one, `p->wrr`, and the kernel sets it up for every new task.
- `struct wrr_rq` is in `kernel/sched/sched.h`. Every CPU's run queue has one, `rq->wrr`.
- The class is placed between RT and CFS in `include/asm-generic/vmlinux.lds.h`. (The order of the classes is the order of their structures in memory, which the linker script decides.)
- `kernel/sched/wrr.c` contains the class's method table and empty methods for you to fill in. It is compiled as part of `kernel/sched/build_policy.c`.
- The two system calls have numbers (462 and 463) and empty bodies.
- `/sys/kernel/debug/sched/debug` shows each CPU's `wrr.nr_running` and `wrr.total_weight`, which helps with debugging. The tests read it too.

You write code in two files. Every place is marked with a `TODO(WRR ...)` comment:

| File | Part | What you write |
|---|---|---|
| `kernel/sched/core.c` | A | Putting `SCHED_WRR` tasks in your class (3 small changes) |
| `kernel/sched/wrr.c` | B | The scheduling algorithm |
| `kernel/sched/wrr.c` | C | The two system calls |

You will need no more than about 200 lines of code. Most of the work is understanding what each method has to do.

## Setup
You need a Linux machine running Ubuntu 24.04 on x86_64 (Intel or AMD) or arm64. A virtual machine is fine, and so is WSL2 on Windows. On a Mac, use an Ubuntu 24.04 virtual machine (for example in UTM): the scripts detect arm64 and build an arm64 kernel. You need about 4 GB of free disk space.

Install the tools:
```
$ sudo apt update
$ sudo apt install build-essential flex bison bc libelf-dev libssl-dev cpio \
      busybox-static git curl xz-utils file qemu-system-x86
```
On arm64, install `qemu-system-arm` instead of `qemu-system-x86`.

QEMU runs much faster with KVM, the kernel's hardware virtualisation support. If `/dev/kvm` exists on your machine, give yourself access to it, then log out and in again:
```
$ sudo usermod -aG kvm $USER
```
If there is no `/dev/kvm` (common inside a virtual machine), QEMU emulates the CPU instead. Everything still works, just more slowly; all the times in this document were measured that way, on 2 CPU cores.

Now get the kernel. This downloads Linux 6.8.12 (about 140 MB) into `linux/`, applies the starter code, and makes `linux/` a git repository with two tags: `base` (the original kernel) and `starter` (with the starter code).
```
$ make get-kernel
```
Build the kernel and run the tests:
```
$ make test
```
The first build takes a few minutes. Later builds only recompile what you changed, so after editing `wrr.c` a rebuild takes about 10 seconds. With the starter code, every test except `sanity` fails, and the score is 0/75.

Commit your work in `linux/` as you go (`git -C linux commit -am "..."`), so you can always get back to a version that worked.

## Part A: Putting SCHED_WRR tasks in your class
The scheduler core decides which class a task belongs to by setting `p->sched_class`. It does this in three places in `kernel/sched/core.c`, each marked with a TODO:

- **A.1 `sched_fork()`, reset-on-fork.** If a `SCHED_WRR` parent has `SCHED_RESET_ON_FORK` set, its child must become `SCHED_NORMAL` with the default WRR weight. Look at what the code already does for real-time parents.
- **A.2 `sched_fork()`, the child's class.** A new task's class is chosen from its priority. A child whose policy is `SCHED_WRR` must go to `wrr_sched_class`.
- **A.3 `__setscheduler_prio()`.** This chooses the class after `sched_setscheduler()`. It is also used for *priority inheritance*: when a real-time task waits for a lock that a lower-priority task holds, the holder is temporarily raised to real-time priority so it can release the lock quickly. Your check must therefore come after the deadline and real-time cases: a `SCHED_WRR` task that is temporarily boosted belongs to the real-time class until the boost ends.

The helpers `task_has_wrr_policy(p)` and `wrr_policy(policy)` in `kernel/sched/sched.h` will be useful. Until you have done Part B, a task you put in your class will disappear: it is handed to your empty `enqueue_task_wrr()` and never runs again. The test suite copes with this, but don't run such a task by hand.

## Part B: The scheduling algorithm
All of Part B is in `kernel/sched/wrr.c`. The comment above each method says exactly what it must do; this section explains how the pieces fit together.

### The rules your data structures must follow
At every moment, for every CPU:
1. Every `SCHED_WRR` task on this CPU that is ready to run, *including the one that is running*, is on `rq->wrr.queue`, has `p->wrr.on_rq == 1`, and is counted in `rq->wrr.nr_running` and `rq->wrr.total_weight`.
2. The running task stays at the head of the queue until its turn ends. A turn ends when its slice runs out, when it yields, or when it blocks.
3. Every task you add to the queue is also counted in the CPU's total number of runnable tasks, with `add_nr_running(rq, 1)`, and removed from it with `sub_nr_running(rq, 1)`. The core uses this total for many things. For example, if all of a CPU's runnable tasks are CFS tasks, it skips the other classes and asks CFS directly; if you forget to count your tasks, it will skip yours.

### What to write
- **B.1 `update_curr_wrr()`** charges the running task for the CPU time it has used, by calling `update_curr_common(rq)`. This is what makes `top`, `getrusage()` and the tests see your tasks' CPU time.
- **B.2 `enqueue_task_wrr()` and `dequeue_task_wrr()`** add a task to the queue and remove it, keeping rules 1 and 3. The core calls them when a task wakes up or blocks, but also when it moves to another CPU or when its policy or weight changes, so don't assume anything else about why they are called.
- **B.3 `pick_task_wrr()`, `pick_next_task_wrr()`, `set_next_task_wrr()` and `put_prev_task_wrr()`** choose the task at the head of the queue, start its CPU-time clock when it starts running, and charge it when it stops. `pick_task_wrr()` must not change anything: the core may call it just to look.
- **B.4 `task_tick_wrr()`** is the round-robin itself. It is called on every timer tick (every 1 ms) while one of your tasks runs. Count down the running task's slice; when it runs out, refill it and, if another task is waiting, move the running task to the back of the queue and call `resched_curr(rq)`. That call doesn't switch tasks immediately: it marks the CPU so that the scheduler runs as soon as the interrupt returns.
- **B.5 `yield_task_wrr()`** moves the running task to the back of the queue.
- **B.6 `task_fork_wrr()`, `switched_to_wrr()` and `get_rr_interval_wrr()`** give new members of the class a full slice, preempt a CFS task when one of your tasks joins the class on its CPU, and report the slice length.
- **B.7 `select_task_rq_wrr()`** chooses a CPU for a task that is created, wakes up or calls `exec()`, as described in the specification.

`balance_wrr()` is given to you, and the method table at the bottom of `wrr.c` is complete.

### Rules for kernel code in the scheduler
- All the methods except `select_task_rq_wrr()` run with the CPU's run-queue lock held and interrupts off. Never sleep, allocate memory, or take a mutex in them.
- `select_task_rq_wrr()` holds only the task's own lock, `p->pi_lock`. It may read other CPUs' `total_weight` with `READ_ONCE()` but must not lock their run queues. A slightly out-of-date value is fine there.
- Don't call `printk()` from the scheduler: printing can wake up other tasks, which needs the lock you already hold. To debug, use `printk_deferred()`, or `printk_deferred_once()` for something that happens often.
- The list helpers you need are in `include/linux/list.h`: `list_add_tail()`, `list_del_init()`, `list_move_tail()` and `list_first_entry_or_null()`. To get from a `struct sched_wrr_entity` back to its task, use `wrr_task_of()` (defined in `wrr.c`).

## Part C: The weight system calls
```c
long sched_setweight(pid_t pid, int weight);   /* system call 462 */
long sched_getweight(pid_t pid);               /* system call 463 */
```
A `pid` of 0 means the calling thread. `user/include/wrr.h` has C wrappers for both calls. Like other system calls, they return a negative error code from the kernel, which the C library turns into a return value of -1 and `errno`.

**`sched_getweight(pid)`** returns the weight of task `pid`. Anybody may read any task's weight. It fails with:
- `EINVAL` if `pid` is negative, or the task's policy is not `SCHED_WRR`;
- `ESRCH` if there is no task `pid`.

**`sched_setweight(pid, weight)`** sets the weight of task `pid` and returns 0. The new weight applies as described in the specification, and the CPU's `total_weight` must change with it if the task is on a queue. It fails with:
- `EINVAL` if `pid` is negative, `weight` is not between 1 and 20, or the task's policy is not `SCHED_WRR`;
- `ESRCH` if there is no task `pid`;
- `EPERM` if the caller does not have `CAP_SYS_NICE` and either does not own the task (the caller's effective user ID matches neither the task's user ID nor its effective user ID), or is trying to raise the weight. Anybody may lower the weight of a task they own, as with `nice`.

The comment above the system calls in `wrr.c` lists the kernel functions you need, and the order to call them in. In particular, check the permissions *before* you lock the task's run queue, and check its policy *after*.

## Building and running
The `Makefile` wraps the scripts in `scripts/`:

| Command | What it does |
|---|---|
| `make kernel` | Builds the kernel into `build/` (only what changed) |
| `make run` | Builds, then boots the VM with a shell |
| `make test` | Builds, then boots the VM and runs every test |
| `make test TESTS="yield fork"` | Runs only the tests named |
| `make patch` | Writes your changes to `submission/wrr.patch` |

If the build fails, the script prints the first errors. It also prints compiler warnings from the files you changed: fix them, because in kernel code they are often real bugs. The full log is in `build/linux-*/build.log`.

`make run` boots the VM with 4 CPUs and a shell on CPU 0. Keep your `SCHED_WRR` experiments on CPUs 1 to 3, so the shell keeps working even if your scheduler doesn't. Here is a short session with the finished scheduler:
```
~ # wrr-run -w 3 -c 1 spin 5 &
~ # wrr-run -w 1 -c 1 spin 5 &
~ # spin pid 53 policy 7 weight 3 wall 5.00s cpu 3.90s share 0.781

[1]-  Done                       wrr-run -w 3 -c 1 spin 5
~ # spin pid 54 policy 7 weight 1 wall 5.00s cpu 1.64s share 0.328

[2]+  Done                       wrr-run -w 1 -c 1 spin 5
~ # wrr-run -c 2 spin 0 &
~ # wrr-weight $!
1
~ # wrr-weight $! 8
8
~ # grep -E "^cpu#|wrr" /sys/kernel/debug/sched/debug
cpu#0, 2100.013 MHz
  .wrr.nr_running                : 0
  .wrr.total_weight              : 0
cpu#1, 2100.013 MHz
  .wrr.nr_running                : 0
  .wrr.total_weight              : 0
cpu#2, 2100.013 MHz
  .wrr.nr_running                : 1
  .wrr.total_weight              : 8
cpu#3, 2100.013 MHz
  .wrr.nr_running                : 0
  .wrr.total_weight              : 0
~ # kill $!
~ # poweroff
```
`wrr-run -w W -c CPUS COMMAND` runs a command as a `SCHED_WRR` task of weight W on the CPUs listed. `spin SECONDS` burns CPU for that long (0 means forever), then reports the share of a CPU it got. `wrr-weight PID [WEIGHT]` reads or changes a weight. In the first two lines, the two spinners shared CPU 1 for most of their 5 seconds, but each also ran alone for a moment (the second started a little later), so their shares are close to, not exactly, 3/4 and 1/4. `poweroff` (or Ctrl-A then X) leaves the VM.

## Testing
`make test` boots the VM and runs `wrr-test`, which prints one line per test, then your score:
```
[ PASS ] runs              5/5     0.2s  A, B.1-B.4: a WRR task runs, execs, exits, is charged
[ FAIL ] yield             0/4     1.9s  B.5: sched_yield
         -> a task that calls sched_yield() in a loop got 0.500 of the CPU next to a spinner of the same weight; yielding should move it to the back of the queue
...
WRR-TEST SCORE 71/75
```
| Test | Points | Checks |
|---|---|---|
| `sanity` | 0 | The policy plumbing in the starter code (passes from the start) |
| `runs` | 5 | A task runs, preempts a CFS task, is charged for its CPU time, execs and exits |
| `class_order` | 6 | CFS tasks get nothing next to WRR tasks; real-time tasks run before WRR tasks |
| `fork` | 4 | Children inherit the policy (and really are in your class) and the weight |
| `reset_on_fork` | 3 | `SCHED_RESET_ON_FORK` |
| `rr_interval` | 4 | `sched_rr_get_interval()` reports *w* × 10 ms |
| `weight_syscalls` | 8 | Return values and error codes of both system calls |
| `weight_perm` | 4 | The permission rules |
| `share_equal` | 6 | Equal weights share a CPU equally (within 0.08) |
| `share_weighted` | 8 | Weights 1:2:3 and 1:4 get matching shares (within 25%) |
| `weight_change` | 5 | Changing the weight of a running task changes its share |
| `yield` | 4 | `sched_yield()` gives up the turn |
| `sleepers` | 4 | Tasks that sleep and wake up thousands of times keep working, and stay in your class |
| `smp_spread` | 8 | New and waking tasks spread over CPUs by total weight |
| `stress` | 6 | 24 tasks forking, sleeping, yielding, moving and changing weight for 3 seconds |

Each test runs its `SCHED_WRR` tasks on CPUs 1 to 3 and checks your queue counters through `/sys/kernel/debug/sched/debug` when they finish. A test also fails if the kernel prints a warning while it runs (for example, a corrupted list or a lock held while sleeping), even if everything else looked right.

You can run tests by hand inside `make run`: `wrr-test list` lists them, and `wrr-test yield fork` runs two. The stress test is random; `WRR_SEED=42 make test TESTS=stress` runs it with a different seed. The share tests measure time, so they allow some error, and emulation (no KVM) makes timing noisier. If a share test fails by a small margin only now and then, run it again before hunting for a bug. If it fails by a lot, the bug is real.

### When the kernel crashes
A bug in the scheduler usually crashes the whole kernel. The test script notices, prints the *first* problem the kernel reported (fix that one first: later messages are often consequences of it), and saves the whole console log in `build/test.log`. A kernel crash report (an *oops*) looks like this:
```
kernel BUG at kernel/sched/core.c:6756!
RIP: 0010:do_task_dead+0x4d/0x4f
Call Trace:
 ...
```
`RIP` is the function that was running, with the offset into it. The call trace below it lists the functions that called it, most recent first. The two most common scheduler bugs show up like this:
- **`list_add corruption`, `list_add double add` or `list_del corruption`**: a task was added to a queue it was already on, or removed from one it wasn't on. Check that `on_rq` and the list always agree.
- **`kernel BUG at kernel/sched/core.c` in `do_task_dead`**: a task that had exited was picked to run again, because it was never removed from your queue.

The kernel is built with extra checks for exactly these mistakes (`CONFIG_DEBUG_LIST`, `CONFIG_DEBUG_ATOMIC_SLEEP` and others in `kernel/qemu-*.config`), so it usually stops near the bug rather than long after it.

## Part D: Experiments and report
Run these in `make run`. Write your results and answers in your report; `report/REPORT.md` has tables to fill in.

1. **Shares.** Measure how close your scheduler gets to the weights. Run `wrr-share -t 10 1 2 3`, `wrr-share -t 10 1 4`, `wrr-share -t 10 1 19` and `wrr-share -t 0.5 1 19`, and record the measured and expected shares. Why is the last one further off than the third? (Hint: how long is one round?)
2. **Latency.** A program that waits for input, such as an editor, sleeps most of the time; what matters to its user is how quickly it gets the CPU after it wakes up. `wrr-latency` measures this: a task that sleeps for 1 ms in a loop shares a CPU with some busy tasks, and reports how late it woke up. Run
   ```
   wrr-latency -n 3 -w 1
   wrr-latency -n 3 -w 10
   wrr-latency -n 3 -p normal
   ```
   The first two use `SCHED_WRR`; the third uses the normal scheduler (CFS) for every task. Explain the worst-case latency of the first two from the weights alone. Why does CFS do so much better here, and what does it give up to do so?
3. **Starvation.** Run
   ```
   taskset -c 1 spin 6 & sleep 1; wrr-run -c 1 spin 3
   ```
   This starts a normal (CFS) task that spins for 6 seconds on CPU 1, and one second later a `SCHED_WRR` task that spins on the same CPU for 3 seconds. Compare the CPU time each one reports. What happened to the normal task while the `SCHED_WRR` task ran, and why? Why did your shell keep working? What would happen if a `SCHED_WRR` task spun forever on every CPU, including CPU 0? (You may try it with `wrr-run -c 0 spin 0`: the VM is disposable.) Real-time tasks have a limit for this reason; look at `/proc/sys/kernel/sched_rt_runtime_us` and `/proc/sys/kernel/sched_rt_period_us`.
4. **Design questions.** Answer briefly:
   - Why must `enqueue_task_wrr()` call `add_nr_running()`? Find the code in `kernel/sched/core.c` that would go wrong without it.
   - Why does the running task stay on the queue, rather than being taken off while it runs?
   - Why may `select_task_rq_wrr()` read other CPUs' `total_weight` without taking their locks, and what is the worst that can happen as a result?
   - Your scheduler never moves a task to another CPU once it is running. Describe a situation in which this leaves one CPU idle while another has several tasks waiting, and how you would fix it.

The report should be at most 3 pages. Besides the experiments and answers, include a short reflection (at most a page) on things you learned, difficulties you encountered and how you overcame them, comments about the assignment, etc.

## Submission requirements
- `submission/wrr.patch`, made with `make patch`. It contains all of your changes since the starter code, committed or not, and `make patch` checks that it applies to the starter code. Run `make test` once more just before you make it: graders apply the patch to the starter code and run the same tests.
- Your report, as a PDF.

### Grading
| Part | Points |
|---|---|
| Tests (`make test`), Parts A to C | 75 |
| Report: experiments and design questions (Part D) | 20 |
| Report: reflection | 5 |

To grade, we apply your patch to the starter code, build it with `kernel/qemu-*.config`, and run each test in its own freshly booted VM, so that a crash in one test does not cost you the others. A test that fails without crashing the kernel is run once more, and counts if it passes the second time. The stress test uses a different seed.

## Going further
These are not graded, but are good ways to learn more:
- **Load balancing.** When a CPU runs out of `SCHED_WRR` tasks, let it take a waiting task from the busiest other CPU. `balance_wrr()` is the place to start, and `pull_rt_task()` in `rt.c` shows how to lock two run queues safely.
- **Run it for real.** `scripts/vm-install.sh build` then `scripts/vm-install.sh install` build your kernel with your Ubuntu machine's own configuration and install it next to your current kernel. Only do this in a VM you can afford to break, after `make test` passes, and read the comment at the top of the script first.
- **Make it the default.** Columbia's version of this assignment ends by making its round-robin scheduler the default for every task, kernel threads included. What breaks, and why?

## Troubleshooting
- **`make get-kernel` cannot download.** It tries kernel.org first, then a GitHub mirror, then Ubuntu's kernel source if it is installed. To use Ubuntu's, run `sudo apt install linux-source-6.8.0`, then `SOURCE=ubuntu make get-kernel`; the starter code applies to it too.
- **`busybox ... is dynamically linked`.** Install `busybox-static`.
- **The VM is very slow.** Check whether `/dev/kvm` exists and whether you are in the `kvm` group (`groups`). Without KVM the VM still works, just more slowly. The test suite takes about 30 seconds either way, because the share tests measure for a fixed time.
- **The VM hangs.** Press Ctrl-A then X to quit QEMU. `make test` gives up after 15 minutes on its own (set `TIMEOUT` to change that).
- **I want to start over.** `git -C linux diff starter` shows your changes; `git -C linux checkout starter -- kernel/sched/wrr.c` restores one file.

## Sources
This assignment adapts ideas and structure from the following. The code in this repository was written for this course.
- Columbia University, COMS W4118 Operating Systems, "Freezer" scheduler assignment: https://cs4118.github.io/freezer, and its recitation notes on writing a scheduling class: https://columbia-os.github.io/recitations/E-Freezer/freezer.html
- Columbia University, COMS W4118 Fall 2024, Homework 4 ("Oven" scheduler): https://w4118.github.io/homework/hw4.html
- Duke University, CPS 210 Spring 2005, Project 2: CPU Scheduling (group weighted round-robin, `SCHED_GWRR`): https://courses.cs.duke.edu/spring05/cps210/Lab2.html
- University of Crete, HY-345 Operating Systems, Assignments 3 and 4 (scheduling system calls and policies, tested in QEMU): https://www.csd.uoc.gr/~hy345/assignments/2024/assignment_3_2024.pdf
- The Linux kernel documentation on the scheduler: https://docs.kernel.org/scheduler/index.html
