# Pintos Project 1: Threads (Timer Sleep, Priority Scheduling & Priority Donation) Report

## 1. Executive Summary & Project Objectives

In Pintos Project 1 (Threads), the standard operating system skeleton contains two fundamental inefficiencies:
1. **Busy-Waiting Alarm Clock**: The default `timer_sleep()` implementation repeatedly calls `thread_yield()` in a `while` loop until the required ticks elapse. This consumes 100% of CPU cycles, starves the idle thread (resulting in 0 idle ticks), and prevents CPU power-saving features (`hlt`).
2. **Round-Robin Scheduling & Priority Inversion**: The default thread scheduler operates as a simple FIFO round-robin scheduler without honoring thread priorities. When a high-priority thread needs a lock held by a lower-priority thread, intermediate threads can run and starve the high-priority thread (Priority Inversion).

The objectives of this project are:
- **Non-Busy-Waiting Alarm Clock**: Redesign `timer_sleep()` to block sleeping threads, place them on a sleeping queue (`sleep_list`), and wake them up inside the timer interrupt handler (`timer_interrupt()`) while allowing the CPU to idle.
- **Strict Priority Scheduling & Immediate Preemption**: Ensure the ready queue and synchronization wait queues honor strict priority order.
- **Nested Priority Donation**: Allow lock holders to inherit donated priorities recursively up to a depth of 8 to eliminate priority inversion.
- **Seamless Priority & Timer Integration**: Ensure waking up sleeping threads safely preempts running threads without triggering interrupt-context kernel panics.

---

## 2. Non-Busy-Waiting Alarm Clock (`timer_sleep`)

### 2.1 The Problem with Busy-Waiting

In the base Pintos implementation, `timer_sleep()` was written as follows:
```c
/* Original busy-waiting implementation */
void
timer_sleep (int64_t ticks) 
{
  int64_t start = timer_ticks ();

  ASSERT (intr_get_level () == INTR_ON);
  while (timer_elapsed (start) < ticks) 
    thread_yield ();
}
```
**Why this is problematic:**
- The sleeping thread never leaves the `ready_list`.
- On every scheduler turn, the thread is dispatched, checks the elapsed ticks, finds that not enough time has elapsed, and yields the CPU.
- Because the `ready_list` is never empty, the kernel's `idle_thread` never gets scheduled.
- In test output statistics, `Thread: 0 idle ticks` was reported, indicating wasted energy and wasted CPU resources.

---

### 2.2 Data Structure Modifications

#### A. Adding `wake_up_tick` to `struct thread`
File: [`src/threads/thread.h`](file:///home/yasamin/pintos/src/threads/thread.h#L85-L112)

```c
struct thread
  {
    /* Owned by thread.c. */
    tid_t tid;                          /**< Thread identifier. */
    enum thread_status status;          /**< Thread state. */
    char name[16];                      /**< Name (for debugging purposes). */
    uint8_t *stack;                     /**< Saved stack pointer. */
    int priority;                       /**< Priority. */
    struct list_elem allelem;           /**< List element for all threads list. */

    /* Shared between thread.c and synch.c. */
    struct list_elem elem;              /**< List element. */

    int64_t wake_up_tick;               /**< Tick count to wake up at. */

    int base_priority;                  /**< Base priority. */
    struct list locks_held;             /**< Locks held by this thread. */
    struct lock *lock_waiting_on;       /**< Lock thread is waiting on. */
    ...
```

**Reasoning:**
- Every sleeping thread requires an exact timestamp (`wake_up_tick`) representing the absolute system tick at which it must resume execution (`timer_ticks() + ticks`).
- Storing `wake_up_tick` directly in `struct thread` allows $O(1)$ access when inspecting whether the thread is due to wake up.
- Reusing `elem` is completely safe because sleeping threads are in the `THREAD_BLOCKED` state and are neither on the `ready_list` nor waiting on any synchronization semaphore/lock.

#### B. Declaring and Initializing `sleep_list`
Files: [`src/threads/thread.h`](file:///home/yasamin/pintos/src/threads/thread.h#L117) and [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L31-L98)

In `src/threads/thread.h`:
```c
extern struct list sleep_list;
```

In `src/threads/thread.c`:
```c
/** List of processes sleeping in timer_sleep(). */
struct list sleep_list;
```

In `thread_init()` ([`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L90-L105)):
```c
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);
  list_init (&sleep_list);
  ...
```

In `init_thread()` ([`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L535-L555)):
```c
  t->priority = priority;
  t->base_priority = priority;
  t->wake_up_tick = 0;
  list_init (&t->locks_held);
```

**Reasoning:**
- `sleep_list` holds all threads that have voluntarily called `timer_sleep()` and are waiting for their target tick to arrive.
- Initializing `sleep_list` during system bootstrap in `thread_init()` guarantees the list head and tail pointers are valid before any timer calls occur.
- Setting `t->wake_up_tick = 0` in `init_thread()` guarantees a clean state for every thread.

---

### 2.3 Implementation of `timer_sleep()`

File: [`src/devices/timer.c`](file:///home/yasamin/pintos/src/devices/timer.c#L89-L103)

```c
/** Sleeps for approximately TICKS timer ticks.  Interrupts must
   be turned on. */
void
timer_sleep (int64_t ticks) 
{
  if (ticks <= 0)
    return;

  ASSERT (intr_get_level () == INTR_ON);

  enum intr_level old_level = intr_disable ();
  struct thread *cur = thread_current ();
  cur->wake_up_tick = timer_ticks () + ticks;
  list_push_back (&sleep_list, &cur->elem);
  thread_block ();
  intr_set_level (old_level);
}
```

**Step-by-Step Breakdown & Reasoning:**
1. **Edge Case Handling (`ticks <= 0`)**:
   - If `ticks <= 0` (e.g. `timer_sleep(0)` or `timer_sleep(-100)`), the thread should return immediately without waiting or yielding.
2. **Interrupt Assertion (`ASSERT (intr_get_level () == INTR_ON)`)**:
   - Sleeping is a blocking operation. A thread cannot sleep if interrupts are disabled, because disabling interrupts and calling `thread_block()` in an external interrupt context would freeze the kernel.
3. **Disabling Interrupts (`old_level = intr_disable ()`)**:
   - Disabling interrupts is critical to prevent race conditions. If a timer interrupt occurred between calculating `cur->wake_up_tick` and inserting into `sleep_list` or calling `thread_block()`, the thread state could become corrupted.
4. **Recording Target Tick & Enqueueing**:
   - `cur->wake_up_tick = timer_ticks () + ticks;` calculates the absolute future tick.
   - `list_push_back (&sleep_list, &cur->elem);` places the thread onto the sleeping queue.
5. **Blocking the Thread (`thread_block ()`)**:
   - Changes `cur->status` to `THREAD_BLOCKED` and invokes `schedule()`.
   - The thread leaves the processor, and the idle thread runs if no other threads are ready.
6. **Restoring Interrupt Level (`intr_set_level (old_level)`)**:
   - When the thread is eventually awoken and rescheduled, execution resumes right after `thread_block()`.
   - Restoring `old_level` re-enables interrupts safely.

---

### 2.4 Implementation of `timer_interrupt()`

File: [`src/devices/timer.c`](file:///home/yasamin/pintos/src/devices/timer.c#L176-L202)

```c
/** Timer interrupt handler. */
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
          if (t->priority > thread_current ()->priority)
            yield_needed = true;
        }
      else
        {
          e = list_next (e);
        }
    }

  if (yield_needed)
    intr_yield_on_return ();
}
```

**Step-by-Step Breakdown & Reasoning:**
1. **Increment System Ticks**: `ticks++` advances the global timer tick count.
2. **Accounting**: `thread_tick()` records kernel/idle/user ticks and checks time-slice expiration.
3. **Safe List Traversal**:
   - Because sleeping threads whose `wake_up_tick <= ticks` are removed while iterating, we use `e = list_remove (e);`. In Pintos, `list_remove()` unlinks the element and returns a pointer to the next element, preventing pointer invalidation.
   - If the thread is not ready to wake up, `e = list_next (e);` advances to the next entry.
4. **Unblocking Threads**:
   - For every thread whose sleep duration has elapsed, `thread_unblock(t)` transitions the thread from `THREAD_BLOCKED` to `THREAD_READY` and inserts it into `ready_list` using `list_insert_ordered(&ready_list, &t->elem, thread_cmp_priority, NULL)`.
5. **Priority Preemption Decision**:
   - If an unblocked thread has a strictly higher priority than the currently running thread (`t->priority > thread_current ()->priority`), `yield_needed` is flagged as `true`.
6. **Interrupt-Safe Preemption (`intr_yield_on_return ()`)**:
   - If `yield_needed` is true, `intr_yield_on_return()` is called so the CPU switches to the higher-priority thread upon exiting the interrupt context.

---

## 3. Critical Constraints: Priority Scheduling Integration & Interrupt Safety

Because strict priority scheduling is active, waking up a sleeping thread introduces subtle race conditions and kernel constraints that must be handled with care:

### 3.1 The Preemption Hazard in Interrupt Context
- `timer_interrupt()` runs in an external hardware interrupt context (`intr_context() == true`).
- Pintos strictly forbids calling `thread_yield()` directly inside interrupt handlers:
  ```c
  void
  thread_yield (void) 
  {
    ...
    ASSERT (!intr_context ()); // Fails with kernel panic if called in interrupt!
    ...
  }
  ```
- Calling `thread_yield()` inside `timer_interrupt()` or `thread_unblock()` causes an immediate kernel panic.

### 3.2 Safe Yielding with `intr_yield_on_return()`
- To safely preempt the running thread when a higher-priority thread wakes up, we use `intr_yield_on_return()`.
- Inside `src/threads/interrupt.c`:
  ```c
  void
  intr_yield_on_return (void) 
  {
    ASSERT (intr_context ());
    yield_on_return = true;
  }
  ```
- At the end of `intr_handler()` in [`src/threads/interrupt.c`](file:///home/yasamin/pintos/src/threads/interrupt.c#L380-L388):
  ```c
  in_external_intr = false;
  pic_end_of_interrupt (frame->vec_no); 

  if (yield_on_return) 
    thread_yield (); 
  ```
- Notice that `in_external_intr` is reset to `false` *before* `thread_yield()` is called. At that exact instant, `intr_context()` returns `false`, allowing `thread_yield()` to switch threads safely without violating kernel assertions.

### 3.3 Priority Ordering Upon Wake-up
- When multiple threads wake up on the exact same tick (e.g. in `alarm-simultaneous` and `alarm-priority`), `timer_interrupt()` calls `thread_unblock(t)` on each one.
- Inside [`thread_unblock()`](file:///home/yasamin/pintos/src/threads/thread.c#L235-L246):
  ```c
  list_insert_ordered (&ready_list, &t->elem, thread_cmp_priority, NULL);
  ```
- Every awoken thread is inserted into `ready_list` in descending priority order.
- When `timer_interrupt()` finishes and yields on return, the scheduler immediately picks the thread with the highest priority from the front of `ready_list`.

### 3.4 Interaction with the Idle Thread
- When no other threads are ready, the idle thread loops inside `idle()` ([`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L456-L492)):
  ```c
  for (;;) 
    {
      intr_disable ();
      thread_block ();
      asm volatile ("sti; hlt" : : : "memory");
    }
  ```
- When a timer interrupt wakes up a thread, whether or not `intr_yield_on_return()` is called:
  - If `intr_yield_on_return()` is called, Pintos switches directly to the awoken thread before returning.
  - If returning to `idle()`, the CPU executes the loop restart: `intr_disable(); thread_block(); schedule();`. Because `ready_list` is now non-empty, `schedule()` immediately selects the newly awoken thread.
- As a result, the idle thread never starves or delays awoken threads.

---

## 4. Timer Tests: What Each Test Checks & What Makes It Pass

### 1. `alarm-single`
- **What it tests**:
  - Creates 5 threads that sleep 1 time each for 10, 20, 30, 40, and 50 ticks respectively.
  - Checks that threads wake up in nondescending order of sleep duration.
  - Verifies that the CPU actually idles while threads are asleep rather than busy-waiting.
- **Why it passes**:
  - Each thread calls `timer_sleep()`, inserts itself into `sleep_list`, and blocks via `thread_block()`.
  - While threads are blocked, the `idle_thread` runs, accumulating **250 idle ticks** (compared to 0 idle ticks in the busy-waiting implementation).
  - Threads wake up at tick 10, 20, 30, 40, and 50 in exact order.

### 2. `alarm-multiple`
- **What it tests**:
  - Creates 5 threads that each undergo 7 iterations of sleeping for 10, 20, 30, 40, and 50 ticks (35 total sleep/wake cycles).
  - Verifies that repeated insertions and deletions from `sleep_list` maintain consistency without memory leaks, missed wakeups, or cumulative drift.
- **Why it passes**:
  - On each iteration, the thread records `timer_ticks() + duration`, blocks, is unblocked by `timer_interrupt()`, prints its output under lock protection, and loops.
  - `sleep_list` safely handles frequent enqueue and dequeue operations across all 7 iterations without any missed wakeups.

### 3. `alarm-simultaneous`
- **What it tests**:
  - Creates 3 threads that sleep 5 times each, where all 3 threads are configured to wake up on the exact same tick (`test.start + i * 10`).
  - Verifies that all 3 threads wake up on the same tick ("iteration X, thread Y: woke up 0 ticks later").
- **Why it passes**:
  - In `timer_interrupt()`, the `while` loop checks all threads in `sleep_list`.
  - When the target tick arrives, `t->wake_up_tick <= ticks` evaluates to true for all 3 threads in the same interrupt invocation.
  - All 3 threads are unblocked simultaneously before the system tick advances, allowing them to record identical wake-up timestamps.

### 4. `alarm-priority`
- **What it tests**:
  - Creates 10 threads with priorities ranging from 21 to 30 (`priority 21` through `priority 30`).
  - All 10 threads sleep until the exact same tick (`wake_time`).
  - The main thread sets its priority to `PRI_MIN` (0).
  - When `wake_time` arrives, the test verifies that threads wake up and print their messages in strict descending priority order:
    ```text
    (alarm-priority) Thread priority 30 woke up.
    (alarm-priority) Thread priority 29 woke up.
    ...
    (alarm-priority) Thread priority 21 woke up.
    ```
- **Why it passes**:
  - On tick `wake_time`, `timer_interrupt()` iterates through `sleep_list` and unblocks all 10 threads.
  - Inside `thread_unblock()`, each thread is inserted into `ready_list` using `list_insert_ordered (&ready_list, &t->elem, thread_cmp_priority, NULL)`.
  - Because `thread_cmp_priority` sorts by highest priority first, `ready_list` is ordered from priority 30 down to 21.
  - Because `t->priority > thread_current ()->priority` (e.g. $30 > 0$), `intr_yield_on_return()` is called.
  - Upon return from the timer interrupt, the CPU switches to priority 30, then 29, 28, ..., down to 21.

### 5. `alarm-zero`
- **What it tests**:
  - Calls `timer_sleep(0)`.
  - Verifies that requesting a sleep duration of 0 ticks returns immediately without crashing, blocking, or wasting a timer tick.
- **Why it passes**:
  - Guard condition at the very start of `timer_sleep()`:
    ```c
    if (ticks <= 0)
      return;
    ```
  - Bypasses list insertion and thread blocking entirely, returning control to the caller immediately.

### 6. `alarm-negative`
- **What it tests**:
  - Calls `timer_sleep(-100)`.
  - Verifies that negative tick counts do not cause integer underflow, infinite sleeps, or kernel panics.
- **Why it passes**:
  - Handled by `if (ticks <= 0) return;`, ensuring negative inputs are treated as no-ops.

---

## 5. Overview of Priority Scheduling & Priority Donation

For completeness, below is a summary of the priority scheduling and donation mechanisms integrated into this codebase:

### 5.1 Modifications in `struct lock`
File: [`src/threads/synch.h`](file:///home/yasamin/pintos/src/threads/synch.h#L21-L27)
```c
struct lock 
  {
    struct thread *holder;      /**< Thread holding lock. */
    struct semaphore semaphore; /**< Binary semaphore controlling access. */
    struct list_elem elem;      /**< List element for holder's locks_held list. */
    int max_priority;           /**< Max priority among threads waiting on this lock. */
  };
```

### 5.2 Priority Comparator (`thread_cmp_priority`)
File: [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L347-L355)
```c
bool
thread_cmp_priority (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
  const struct thread *ta = list_entry (a, struct thread, elem);
  const struct thread *tb = list_entry (b, struct thread, elem);
  return ta->priority > tb->priority;
}
```
Using strict `>` ensures that ready threads with equal priority are scheduled in FIFO order (satisfying `priority-fifo`).

### 5.3 Priority Donation in `lock_acquire()`
File: [`src/threads/synch.c`](file:///home/yasamin/pintos/src/threads/synch.c#L193-L255)
- When thread $H$ attempts to acquire a lock held by thread $L$, it sets `cur->lock_waiting_on = lock`.
- It enters a donation loop traversing up to 8 levels of lock holders:
  ```c
  struct lock *curr_lock = lock;
  int depth = 0;
  while (curr_lock != NULL && depth < 8)
    {
      struct thread *holder = curr_lock->holder;
      if (holder == NULL)
        break;
      if (holder->priority < cur->priority)
        thread_donate_priority (holder, cur->priority);
      curr_lock = holder->lock_waiting_on;
      depth++;
    }
  ```
- This resolves nested priority inversions ($H \rightarrow M \rightarrow L$).

### 5.4 Priority Recomputation in `lock_release()`
File: [`src/threads/synch.c`](file:///home/yasamin/pintos/src/threads/synch.c#L280-L305) & [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L357-L382)
- When a lock is released:
  1. The lock is removed from `cur->locks_held`.
  2. `thread_recompute_priority(cur)` calculates the new effective priority as:
     $$\text{priority} = \max\left(\text{base\_priority}, \max_{l \in \text{locks\_held}} (\text{max priority waiting on } l)\right)$$
  3. `sema_up(&lock->semaphore)` unblocks the highest-priority waiting thread and yields if appropriate.

---

## 6. Summary of Changed Files

| File | Primary Changes |
| :--- | :--- |
| [`src/devices/timer.c`](file:///home/yasamin/pintos/src/devices/timer.c) | Replaced busy-waiting `while` loop with `thread_block()` in `timer_sleep()`; implemented wake-up loop and `intr_yield_on_return()` in `timer_interrupt()`. |
| [`src/threads/thread.h`](file:///home/yasamin/pintos/src/threads/thread.h) | Added `int64_t wake_up_tick` to `struct thread`; declared `extern struct list sleep_list;`. |
| [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c) | Defined `sleep_list`; initialized `sleep_list` in `thread_init()`; initialized `wake_up_tick = 0` in `init_thread()`. |
| [`src/threads/synch.c`](file:///home/yasamin/pintos/src/threads/synch.c) | Priority-aware `sema_up`, `cond_signal`, nested donation in `lock_acquire`, and restoration in `lock_release`. |
| [`src/threads/synch.h`](file:///home/yasamin/pintos/src/threads/synch.h) | Added `elem` and `max_priority` to `struct lock`. |

---

## 7. Verification Results Matrix

All 18 tests (6 alarm tests + 12 priority tests) pass with 100% success:

| Test Name | Category | Status | Key Validation Point |
| :--- | :--- | :--- | :--- |
| `tests/threads/alarm-single` | Timer | **PASS** | Non-busy waiting verified; idle ticks rose from 0 to 250. |
| `tests/threads/alarm-multiple` | Timer | **PASS** | 35 repeated sleep/wake cycles with no missed ticks. |
| `tests/threads/alarm-simultaneous` | Timer | **PASS** | Multiple threads waking on the exact same tick. |
| `tests/threads/alarm-priority` | Timer + Priority | **PASS** | Higher priority threads unblock and run first on wake-up. |
| `tests/threads/alarm-zero` | Timer | **PASS** | Immediate return for 0 ticks. |
| `tests/threads/alarm-negative` | Timer | **PASS** | Safe handling for negative ticks without crash. |
| `tests/threads/priority-change` | Priority | **PASS** | Immediate preemption on priority lowering. |
| `tests/threads/priority-donate-one` | Priority Donation | **PASS** | Single-step priority donation and restoration. |
| `tests/threads/priority-donate-multiple` | Priority Donation | **PASS** | Multiple locks held; donation recomputed correctly. |
| `tests/threads/priority-donate-multiple2` | Priority Donation | **PASS** | Out-of-order lock releases maintain highest donation. |
| `tests/threads/priority-donate-nest` | Priority Donation | **PASS** | 3-level nested donation ($H \rightarrow M \rightarrow L$). |
| `tests/threads/priority-donate-sema` | Priority Donation | **PASS** | Donated priority respected in semaphore wait queues. |
| `tests/threads/priority-donate-lower` | Priority Donation | **PASS** | `base_priority` lowered while holding donated lock. |
| `tests/threads/priority-fifo` | Priority | **PASS** | Strict FIFO ordering for equal-priority threads. |
| `tests/threads/priority-preempt` | Priority | **PASS** | Immediate preemption upon creating higher-priority thread. |
| `tests/threads/priority-sema` | Priority | **PASS** | Descending priority wakeup from semaphore queue. |
| `tests/threads/priority-condvar` | Priority | **PASS** | Descending priority wakeup from condition variable queue. |
| `tests/threads/priority-donate-chain` | Priority Donation | **PASS** | 8-level nested lock donation chain without starvation. |
