# SCHED_WRR report

Name:

Machine (CPU, cores, native/VM/WSL2, KVM or not):

## 1. Shares

| Command | Weights | Measured shares | Expected shares |
|---|---|---|---|
| `wrr-share -t 10 1 2 3` | 1, 2, 3 | | |
| `wrr-share -t 10 1 4` | 1, 4 | | |
| `wrr-share -t 10 1 19` | 1, 19 | | |
| `wrr-share -t 0.5 1 19` | 1, 19 | | |

Why is the last one further off than the third?

## 2. Latency

| Command | mean (ms) | p50 | p90 | p99 | max |
|---|---|---|---|---|---|
| `wrr-latency -n 3 -w 1` | | | | | |
| `wrr-latency -n 3 -w 10` | | | | | |
| `wrr-latency -n 3 -p normal` | | | | | |

Explain the worst case of the two `SCHED_WRR` runs from the weights alone.

Why does CFS do so much better here, and what does it give up to do so?

## 3. Starvation

What CPU time did each spinner report? What happened to the normal task while the `SCHED_WRR` task ran, and why?

Why did your shell keep working?

What happens when a `SCHED_WRR` task spins forever on every CPU?

## 4. Design questions

Why must `enqueue_task_wrr()` call `add_nr_running()`? Which code in `kernel/sched/core.c` goes wrong without it?

Why does the running task stay on the queue while it runs?

Why may `select_task_rq_wrr()` read other CPUs' `total_weight` without their locks, and what is the worst that can happen?

When does the lack of load balancing leave a CPU idle while another has tasks waiting, and how would you fix it?

## 5. Reflection (at most a page)

Things you learned, difficulties you encountered and how you overcame them, comments about the assignment, etc.
