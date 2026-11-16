# Pintos Project 1: Multi-Level Feedback Queue Scheduler (MLFQS) Comprehensive Report

## 1. Executive Summary & Design Overview

In Pintos Project 1 (Threads), the third and final milestone is the implementation of the **Multi-Level Feedback Queue Scheduler (MLFQS)**, modeled after the 4.4BSD Unix scheduler.

### 1.1 The Limitations of Strict Priority Scheduling & Priority Donation
In the preceding phase of Project 1, we implemented Strict Priority Scheduling with Priority Donation. While that resolved priority inversion, it suffered from inherent real-world limitations:
1. **Manual Configuration Required:** Thread priorities had to be explicitly declared by programmers or hardcoded at thread creation. Applications could game the system by artificially setting high priorities.
2. **Lack of Dynamic Adaptation:** The scheduler had no automated metric to distinguish between CPU-intensive batch jobs (which should yield to keep the system responsive) and interactive, I/O-intensive jobs (which sleep frequently and need immediate execution when an event occurs).
3. **Synchronization Complexity:** Priority donation required traversing lock ownership graphs, maintaining lists of held locks, and recalculating base versus donated priorities on every lock acquisition and release.

### 1.2 The MLFQS Paradigm
The 4.4BSD MLFQS replaces manual priority assignment and priority donation with automated, mathematical scheduling:
* **Dynamic Priority Recalculation:** The scheduler calculates priority every 4 ticks based on a thread's niceness and its recent CPU consumption:
  $$\text{priority} = \text{PRI\_MAX} - \left(\frac{\text{recent\_cpu}}{4}\right) - (\text{nice} \times 2)$$
* **Exponential CPU Decay:** A thread's recent CPU consumption decays exponentially over time using the system-wide load average, ensuring past CPU spikes do not permanently penalize threads.
* **System Load Average Tracking:** Tracks the exponential moving average of ready and running threads over time.
* **Mutual Scheduler Isolation:** The `-mlfqs` command-line flag completely disables priority donation and manual priority assignments, transferring full scheduling authority to the mathematical formulas.

---

## 2. 17.14 Fixed-Point Real Arithmetic Engine

### 2.1 Why Fixed-Point Math is Essential in the Pintos Kernel
The x86 architecture requires floating-point operations (FPU/SSE) to manage special floating-point registers. In the Pintos kernel:
* Floating-point instructions are disabled via compiler flags (`-mno-sse`).
* Context switches do not save or restore FPU registers (doing so would cause severe performance overhead on each thread switch).
* Attempting floating-point arithmetic inside kernel space would trigger undefined behavior or kernel crashes.

To compute ratios, fractions, and decay factors, we implemented 17.14 fixed-point arithmetic:
* **Bit Format:** 32-bit signed integer (`int32_t`): 1 sign bit, 17 integer bits, and 14 fractional bits.
* **Scaling Factor:** $f = 2^{14} = 16384$. An integer $n$ is represented as $n \times 16384$.

### 2.2 Implementation: [`src/threads/fixed-point.h`](file:///home/yasamin/pintos/src/threads/fixed-point.h)

We created a dedicated header file defining fixed-point arithmetic macros:

```c
#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

#include <stdint.h>

#define FP_SHIFT 14
#define FP_F (1 << FP_SHIFT)       /* 16384 */
#define FP_HALF (FP_F / 2)         /* 8192 (for rounding to nearest) */

/* Convert integer n to fixed point */
#define INT_TO_FP(n) ((int) ((n) * FP_F))

/* Convert fixed point x to integer (rounding toward zero) */
#define FP_TO_INT_ZERO(x) ((int) ((x) / FP_F))
#define FP_TO_INT(x) FP_TO_INT_ZERO(x)

/* Convert fixed point x to integer (rounding to nearest) */
#define FP_TO_INT_ROUND(x) ((int) ((x) >= 0 ? (((x) + FP_HALF) / FP_F) : (((x) - FP_HALF) / FP_F)))

/* Add two fixed-point numbers */
#define FP_ADD(x, y) ((x) + (y))

/* Subtract fixed-point y from fixed-point x */
#define FP_SUB(x, y) ((x) - (y))

/* Add fixed-point x and integer n */
#define FP_ADD_INT(x, n) ((x) + ((n) * FP_F))

/* Subtract integer n from fixed-point x */
#define FP_SUB_INT(x, n) ((x) - ((n) * FP_F))

/* Multiply two fixed-point numbers */
#define FP_MUL(x, y) ((int) (((int64_t) (x)) * (y) / FP_F))

/* Multiply fixed-point x by integer n */
#define FP_MUL_INT(x, n) ((x) * (n))

/* Divide fixed-point x by fixed-point y */
#define FP_DIV(x, y) ((int) (((int64_t) (x)) * FP_F / (y)))

/* Divide fixed-point x by integer n */
#define FP_DIV_INT(x, n) ((x) / (n))

#endif /* threads/fixed-point.h */
```

### 2.3 Mathematical Considerations
1. **64-Bit Casting in Multiplication and Division:**
   Multiplying two 17.14 numbers produces a number scaled by $f^2$ ($2^{28}$), which easily exceeds the 32-bit signed integer boundary ($\approx 2 \times 10^9$). We cast operands to `(int64_t)` before multiplying or scaling, preventing overflow before dividing by $f$.
2. **Rounding to Nearest:**
   Pintos specification mandates that priority and getters return values rounded to the nearest integer. `FP_TO_INT_ROUND(x)` adds $f/2$ ($8192$) for positive numbers and subtracts $f/2$ for negative numbers before integer division by $f$.

---

## 3. Data Structure Modifications & System Initialization

### 3.1 Modifications to `struct thread`
File: [`src/threads/thread.h`](file:///home/yasamin/pintos/src/threads/thread.h#L100-L108)

```c
struct thread
  {
    /* ... existing members ... */
    int base_priority;                  /**< Base priority. */
    struct list locks_held;             /**< Locks held by this thread. */
    struct lock *lock_waiting_on;       /**< Lock thread is waiting on. */

    int nice;                           /**< Niceness value (-20 to 20). */
    int recent_cpu;                     /**< Recent CPU usage (17.14 fixed-point). */
#ifdef USERPROG
    uint32_t *pagedir;
#endif
    unsigned magic;
  };
```

* `nice`: Measures how "nice" a thread is to other threads. Ranges from -20 (least nice, gets highest priority boost) to 20 (most nice, yields CPU time to others). Default is 0.
* `recent_cpu`: 17.14 fixed-point estimator of CPU time consumed recently by this specific thread.

### 3.2 Global System Load Average
Files: [`src/threads/thread.h`](file:///home/yasamin/pintos/src/threads/thread.h#L118-L121), [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L65-L68)

```c
/* Global system load average in 17.14 fixed-point */
int load_avg;
```

### 3.3 System Boot & Thread Initialization
File: [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L92-L115)

In `thread_init()`:
```c
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);
  list_init (&sleep_list);

  load_avg = 0;  /* System load initialized to 0 at boot */

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
  initial_thread->nice = 0;
  initial_thread->recent_cpu = 0;
  if (thread_mlfqs)
    thread_calculate_priority (initial_thread);
}
```

### 3.4 Inheritance on Thread Creation
File: [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L195-L215)

When a new thread is created in `thread_create()`, it must inherit the parent thread's `nice` and `recent_cpu` values:
```c
  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  if (thread_mlfqs)
    {
      if (strcmp (name, "idle") == 0)
        {
          t->nice = 0;
          t->recent_cpu = 0;
          t->priority = PRI_MIN;
        }
      else
        {
          t->nice = thread_current ()->nice;
          t->recent_cpu = thread_current ()->recent_cpu;
          thread_calculate_priority (t);
        }
    }
```
**Special Consideration for `idle` Thread:**
The `idle` thread must never participate in MLFQS priority calculations. It must remain strictly at `PRI_MIN` (0) and never accumulate `recent_cpu` or count towards `ready_threads`.

---

## 4. The MLFQS Calculations & Algorithms

### 4.1 Thread Dynamic Priority Calculation
File: [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L455-L470)

The priority formula specified by 4.4BSD is:
$$\text{priority} = \text{PRI\_MAX} - \left(\frac{\text{recent\_cpu}}{4}\right) - (\text{nice} \times 2)$$

```c
void
thread_calculate_priority (struct thread *t)
{
  if (t == idle_thread)
    return;

  int p = PRI_MAX - FP_TO_INT_ROUND (t->recent_cpu / 4) - (t->nice * 2);
  if (p > PRI_MAX)
    p = PRI_MAX;
  if (p < PRI_MIN)
    p = PRI_MIN;

  t->priority = p;
}
```
**Key Highlights:**
* Dividing `t->recent_cpu / 4` preserves fixed-point scaling since $16384 / 4 = 4096$.
* The term is converted to an integer rounded to the nearest integer using `FP_TO_INT_ROUND`.
* Clamped strictly between `PRI_MIN` (0) and `PRI_MAX` (63).

---

### 4.2 System Load Average (`load_avg`)
File: [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L480-L510)

The system load average estimates the average number of threads ready to run over the past 60 seconds:
$$\text{load\_avg} = \left(\frac{59}{60}\right) \times \text{load\_avg} + \left(\frac{1}{60}\right) \times \text{ready\_threads}$$

Where `ready_threads` is the number of threads that are running or ready to run at the update point (excluding the idle thread):
$$\text{ready\_threads} = \text{list\_size}(\&\text{ready\_list}) + (\text{thread\_current}() \ne \text{idle\_thread} \; ? \; 1 : 0)$$

In fixed-point arithmetic:
```c
int ready_threads = list_size (&ready_list);
if (thread_current () != idle_thread)
  ready_threads++;

load_avg = (int) (((int64_t) 59 * load_avg + (int64_t) ready_threads * FP_F) / 60);
```
Computing $((59 \times \text{load\_avg}) + (\text{ready\_threads} \times 16384)) / 60$ in a single 64-bit division minimizes cumulative rounding errors.

---

### 4.3 Recent CPU Usage (`recent_cpu`)
Each thread's `recent_cpu` measures the CPU time it has received recently.
1. **Per-Tick Increment:** Each timer tick, if the running thread is not the idle thread, its `recent_cpu` increments by 1 (in fixed point, $+ 16384$).
2. **Once-Per-Second Decay:** Every 100 ticks (1 second), `recent_cpu` for **every thread** in the system (running, ready, and blocked) decays according to:
   $$\text{recent\_cpu} = \left(\frac{2 \times \text{load\_avg}}{2 \times \text{load\_avg} + 1}\right) \times \text{recent\_cpu} + \text{nice}$$

```c
int twice_load = 2 * load_avg;
int coef = FP_DIV (twice_load, twice_load + FP_F);

struct list_elem *e;
for (e = list_begin (&all_list); e != list_end (&all_list); e = list_next (e))
  {
    struct thread *t = list_entry (e, struct thread, allelem);
    if (t != idle_thread)
      t->recent_cpu = FP_ADD_INT (FP_MUL (coef, t->recent_cpu), t->nice);
  }
```
**Performance Optimization:** The coefficient $\frac{2 \times \text{load\_avg}}{2 \times \text{load\_avg} + 1}$ is identical for all threads; precomputing `coef` once per second avoids redundant 64-bit divisions inside the loop over `all_list`.

---

## 5. Timer Interrupt Updates & Scheduling Rules

File: [`src/devices/timer.c`](file:///home/yasamin/pintos/src/devices/timer.c#L175-L225)

The heart of the MLFQS timer updates resides in `timer_interrupt()`:

```c
static void
timer_interrupt (struct intr_frame *args UNUSED)
{
  ticks++;
  thread_tick ();

  bool yield_needed = false;
  struct list_elem *e = list_begin (&sleep_list);
  while (e != list_end (&sleep_list))
    {
      struct thread *t = list_entry (e, struct thread, elem);
      if (t->wake_up_tick <= ticks)
        {
          e = list_remove (e);
          thread_unblock (t);
          if (thread_current () == thread_idle () || t->priority > thread_current ()->priority)
            yield_needed = true;
        }
      else
        {
          e = list_next (e);
        }
    }

  if (thread_mlfqs)
    {
      /* 1. Every tick: increment running thread's recent_cpu */
      thread_mlfqs_tick ();

      /* 2. Every 100 ticks (1 second): update load_avg and decay recent_cpu */
      if (ticks % TIMER_FREQ == 0)
        thread_mlfqs_update_load_avg_and_recent_cpu ();

      /* 3. Every 4 ticks: recalculate priority of all threads */
      if (ticks % 4 == 0)
        {
          thread_mlfqs_update_priorities ();
          if (thread_mlfqs_should_yield ())
            yield_needed = true;
        }
    }

  if (yield_needed)
    intr_yield_on_return ();
}
```

### Critical Sequencing Considerations:
1. **Sleep List Woken Before MLFQS Updates:**
   Sleeping threads whose sleep expires at tick $T$ must be unblocked into `ready_list` **before** `thread_mlfqs_update_load_avg_and_recent_cpu()` executes. Otherwise, threads waking at tick $T$ would be excluded from `ready_threads`, artificially depressing `load_avg`.
2. **Order of Decay and Priority Recalculation at Tick 100:**
   Because 100 is a multiple of 4 ($100 \% 4 == 0$), both the 1-second decay and 4-tick priority update trigger at tick 100. Updating `load_avg` and `recent_cpu` first guarantees that priority recalculation utilizes the newly decayed `recent_cpu` values.
3. **Re-sorting `ready_list` on Priority Updates:**
   Because priority updates change the priority of threads currently waiting in `ready_list`, `ready_list` can become disordered. Calling `list_sort (&ready_list, thread_cmp_priority, NULL)` restores descending priority order.
4. **Immediate Preemption Check (`thread_mlfqs_should_yield`):**
   If priority recalculation causes the running thread's priority to drop below any thread in `ready_list` (or if the idle thread is running while ready threads exist), `yield_needed` is set to trigger `intr_yield_on_return()`.

---

## 6. Disabling Priority Donation & Isolating Schedulers

When `-mlfqs` is enabled on the kernel command line, manual priorities and priority donations must be completely disabled.

### 6.1 `thread_set_priority` Ignored
File: [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L368-L377)
```c
void
thread_set_priority (int new_priority) 
{
  if (thread_mlfqs)
    return;  /* Manual priority setting is ignored under MLFQS */

  struct thread *cur = thread_current ();
  cur->base_priority = new_priority;
  thread_recompute_priority (cur);
  thread_yield_if_needed ();
}
```

### 6.2 Bypassing Priority Donation Chains in `synch.c`
File: [`src/threads/synch.c`](file:///home/yasamin/pintos/src/threads/synch.c#L225-L255)
In `lock_acquire()`:
```c
  struct thread *cur = thread_current ();

  /* Disable priority donation chain under MLFQS */
  if (!thread_mlfqs && lock->holder != NULL)
    {
      cur->lock_waiting_on = lock;
      struct lock *curr_lock = lock;
      int depth = 0;
      while (curr_lock != NULL && depth < 8)
        {
          struct thread *holder = curr_lock->holder;
          if (holder == NULL)
            break;
          if (holder->priority < cur->priority)
            thread_donate_priority (holder, cur->priority);

          if (curr_lock->max_priority < cur->priority)
            curr_lock->max_priority = cur->priority;

          curr_lock = holder->lock_waiting_on;
          depth++;
        }
    }

  sema_down (&lock->semaphore);

  lock->holder = cur;
  if (!thread_mlfqs)
    {
      cur->lock_waiting_on = NULL;
      list_push_back (&cur->locks_held, &lock->elem);
      lock->max_priority = lock_get_max_priority (lock);
    }
```

In `lock_release()`:
```c
void
lock_release (struct lock *lock) 
{
  ASSERT (lock != NULL);
  ASSERT (lock_held_by_current_thread (lock));

  struct thread *cur = thread_current ();

  /* Recomputing priority based on held locks is only done under standard scheduler */
  if (!thread_mlfqs)
    {
      list_remove (&lock->elem);
      thread_recompute_priority (cur);
    }

  lock->holder = NULL;
  sema_up (&lock->semaphore);
}
```

### 6.3 Interface Functions: Niceness and Stats Getters
File: [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L530-L565)
```c
void
thread_set_nice (int nice) 
{
  struct thread *cur = thread_current ();
  cur->nice = nice;
  thread_calculate_priority (cur);
  thread_yield_if_needed ();
}

int
thread_get_nice (void) 
{
  return thread_current ()->nice;
}

int
thread_get_load_avg (void) 
{
  int64_t val = (int64_t) load_avg * 100;
  return (int) (val >= 0 ? (val + FP_HALF) / FP_F : (val - FP_HALF) / FP_F);
}

int
thread_get_recent_cpu (void) 
{
  int64_t val = (int64_t) thread_current ()->recent_cpu * 100;
  return (int) (val >= 0 ? (val + FP_HALF) / FP_F : (val - FP_HALF) / FP_F);
}
```

---

## 7. Deep-Dive: MLFQS Test Analysis & What Makes Each Pass

| Test Name | Core Objective | Key Mechanism That Makes It Pass |
| :--- | :--- | :--- |
| **`mlfqs-load-1`** | Verifies a single compute-bound thread raises system `load_avg` to 0.5 in 38–45s (theoretical: 42s), then 10s of inactivity drops it below 0.5. | `ready_threads` equals 1 during CPU spin and 0 when sleeping. The recurrence $\text{load\_avg} = \frac{59}{60}\text{load\_avg} + \frac{1}{60}(1)$ reaches $0.5$ at $t = 42$. |
| **`mlfqs-load-60`** | 60 threads sleep for 10s, spin for 60s, then sleep. Tests large batch load average curve. | Unblocking sleeping threads in `timer_interrupt` *before* `ticks % 100 == 0` ensures all 60 threads are counted at $t = 10$, raising load average to exactly $1.00$ at second 0 of measurement. |
| **`mlfqs-load-avg`** | 60 staggered threads (thread $i$ sleeps $10+i$ seconds, spins 60s). Stresses interrupt overhead and load response. | Fast, minimal timer interrupt overhead prevents timer tick slip. Accurate increment and decay of load average over a multi-minute test trajectory. |
| **`mlfqs-recent-1`** | Tests `recent_cpu` calculation for a single thread over 180 seconds. | Incrementing `recent_cpu` by 1 per tick and decaying via $\frac{2 \times \text{load\_avg}}{2 \times \text{load\_avg} + 1}$ exactly on tick boundaries. Fixed-point rounding in `thread_get_recent_cpu()` matches expected values within margin of error. |
| **`mlfqs-fair-2`** | 2 threads with `nice = 0` spin for 30s; both must receive nearly identical tick counts (~1500 each). | Every 4 ticks, the running thread accumulates `recent_cpu`, lowering its priority. The waiting thread has higher priority and preempts. Both threads execute in alternating round-robin slices. |
| **`mlfqs-fair-20`** | 20 threads with `nice = 0` spin for 30s; all 20 must receive approximately equal CPU time (~150 ticks each). | Stable `list_sort()` preserves FIFO ordering among threads of equal priority, preventing starvation or bias across all 20 threads. |
| **`mlfqs-nice-2`** | 2 threads with `nice = 0` and `nice = 5` spin for 30s. Thread 0 must get ~1904 ticks; Thread 1 must get ~1096 ticks. | The term $-(\text{nice} \times 2)$ penalizes Thread 1 by 10 priority levels, so Thread 0 runs until its `recent_cpu` rises high enough for Thread 1 to compete. |
| **`mlfqs-nice-10`** | 10 threads with `nice` from 0 to 9 spin for 30s; verified against simulated tick distributions. | The priority separation proportional to niceness allows threads with lower nice values to acquire predictably larger shares of CPU cycles. |
| **`mlfqs-block`** | A thread spins, blocks on a lock held by a sleeping thread for 10s. Verifies `recent_cpu` decays while blocked. | Because `thread_mlfqs_update_load_avg_and_recent_cpu()` iterates over `all_list` (which includes blocked threads), the blocked thread's `recent_cpu` decays significantly. When the lock is released, its priority is much higher than the spinner, causing immediate preemption. |

---

## 8. Test Verification & Results

### MLFQS Suite (`-mlfqs`)
```text
mlfqs-load-1: PASS
mlfqs-load-60: PASS
mlfqs-load-avg: PASS
mlfqs-recent-1: PASS
mlfqs-fair-2: PASS
mlfqs-fair-20: PASS
mlfqs-nice-2: PASS
mlfqs-nice-10: PASS
mlfqs-block: PASS
```

### Regression Priority & Alarm Suite (Standard Scheduler)
```text
alarm-single: PASS
alarm-multiple: PASS
alarm-simultaneous: PASS
alarm-priority: PASS
alarm-zero: PASS
alarm-negative: PASS
priority-change: PASS
priority-donate-one: PASS
priority-donate-multiple: PASS
priority-donate-multiple2: PASS
priority-donate-nest: PASS
priority-donate-sema: PASS
priority-donate-lower: PASS
priority-fifo: PASS
priority-preempt: PASS
priority-sema: PASS
priority-condvar: PASS
priority-donate-chain: PASS
```

**Total Pass Rate:** 27 / 27 tests passing.
All version constraints and zero git commands adhered to.
