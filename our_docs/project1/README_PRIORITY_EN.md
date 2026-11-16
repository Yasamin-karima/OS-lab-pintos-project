# Pintos Project 1: Priority Scheduling & Priority Donation Report

## 1. Executive Summary & Objective

In the base implementation of Pintos, the thread scheduler operates as a simple Round-Robin scheduler based on FIFO ordering without honoring thread priorities. If a high-priority thread becomes ready, it does not immediately preempt a lower-priority running thread. Furthermore, synchronization primitives (semaphores, locks, and condition variables) wake up sleeping threads in FIFO order. Most critically, when a lower-priority thread holds a lock that a higher-priority thread needs, a classic **Priority Inversion** problem occurs: intermediate-priority threads can prevent the low-priority lock holder from running, thereby indefinitely starving the high-priority thread.

The objective of this project is to implement:
1. **Strict Priority Scheduling**: Threads on the ready queue run strictly in descending order of priority, with immediate preemption whenever a higher-priority thread appears or the running thread lowers its priority.
2. **Priority-Aware Synchronization**: Semaphores, locks, and condition variables wake up waiting threads in order of highest priority first.
3. **Nested Priority Donation**: If a high-priority thread attempts to acquire a lock held by a lower-priority thread, the lock holder temporarily inherits (donated) the higher priority. This donation recursively propagates through nested chains of lock holders (up to depth 8) and correctly restores the base priority upon lock release.
4. **Zero Regressions on Timer Tests**: Ensuring all non-busy-waiting timer sleep tests (`alarm-*`) continue to pass without kernel panics or race conditions in interrupt context.

---

## 2. Data Structure Modifications

### 2.1 Changes in `struct thread` ([`src/threads/thread.h`](file:///home/yasamin/pintos/src/threads/thread.h#L85-L109))

```c
struct thread
  {
    /* Owned by thread.c. */
    tid_t tid;                          /**< Thread identifier. */
    enum thread_status status;          /**< Thread state. */
    char name[16];                      /**< Name (for debugging purposes). */
    uint8_t *stack;                     /**< Saved stack pointer. */
    int priority;                       /**< Effective priority (including donations). */
    struct list_elem allelem;           /**< List element for all threads list. */

    /* Shared between thread.c and synch.c. */
    struct list_elem elem;              /**< List element for ready_list or sema waiters. */

    int base_priority;                  /**< Base priority set by thread_set_priority or creation. */
    struct list locks_held;             /**< List of struct lock held by this thread. */
    struct lock *lock_waiting_on;       /**< Lock this thread is currently blocked waiting to acquire. */
    ...
    unsigned magic;
  };
```

**Reasoning:**
- `base_priority`: Needed so that if a thread receives a priority donation and subsequently calls `thread_set_priority()`, its base priority changes, but its effective `priority` remains donated until the relevant locks are released.
- `locks_held`: A thread can acquire multiple locks simultaneously. To recalculate its effective priority when one lock is released, the thread must inspect all remaining held locks and find the highest priority among all threads waiting on them.
- `lock_waiting_on`: Essential for tracking nested priority donation. If thread A holds lock 1 and waits on lock 2 (held by thread B), and thread C wants lock 1, thread C donates to A, and by following `A->lock_waiting_on->holder`, donates to B.

### 2.2 Changes in `struct lock` ([`src/threads/synch.h`](file:///home/yasamin/pintos/src/threads/synch.h#L21-L27))

```c
struct lock 
  {
    struct thread *holder;      /**< Thread holding lock (for debugging). */
    struct semaphore semaphore; /**< Binary semaphore controlling access. */
    struct list_elem elem;      /**< List element for holder's locks_held list. */
    int max_priority;           /**< Max priority among threads waiting on this lock. */
  };
```

**Reasoning:**
- `elem`: Enables the lock to be inserted into the holder thread's `locks_held` list without dynamic memory allocation.
- `max_priority`: Caches the maximum priority among all threads blocked on this lock's semaphore waiters list.

### 2.3 Initialization in `init_thread()` ([`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L529-L547))

```c
static void
init_thread (struct thread *t, const char *name, int priority)
{
  ...
  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->base_priority = priority;
  list_init (&t->locks_held);
  t->lock_waiting_on = NULL;
  t->magic = THREAD_MAGIC;
  ...
}
```

**Crucial Bug Fixed:**
Previously, `locks_held` was zeroed out by `memset()` instead of properly initialized with `list_init()`. Because `locks_held.head.next` and `locks_held.tail.prev` were `NULL`, the very first lock acquisition during kernel boot (`allocate_tid()` acquiring `tid_lock`) caused a kernel panic in `list_insert()`:
```text
assertion `is_interior (before) || is_tail (before)' failed.
```
Initializing `t->base_priority = priority` and `list_init(&t->locks_held)` completely resolved this panic.

---

## 3. Core Mechanisms & Code Implementation

### 3.1 Strict Priority Comparator (`thread_cmp_priority`)
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

**Reasoning & Critical Constraint:**
- `list_insert_ordered()` and `list_sort()` in Pintos require a **strict weak ordering** comparator returning strictly greater than (`>`), **never** `>=`.
- If `>=` is used, elements with equal priority will be continuously reordered or inserted incorrectly, violating FIFO ordering for equal priorities and causing pointer corruption in the circular list structure.

### 3.2 Ordered Ready Queue & Idle Thread Protection
Files: [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L235-L246), [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L304-L318)

In `thread_unblock()`:
```c
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  list_insert_ordered (&ready_list, &t->elem, thread_cmp_priority, NULL);
  t->status = THREAD_READY;
  intr_set_level (old_level);
}
```

In `thread_yield()`:
```c
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) 
    list_insert_ordered (&ready_list, &cur->elem, thread_cmp_priority, NULL);
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}
```

**Critical Constraints Enforced:**
1. **Never yield in interrupts:** `thread_unblock()` does **not** call `thread_yield()`. Calling yield in `thread_unblock()` causes kernel panics when `timer_interrupt()` wakes sleeping threads.
2. **Protect the idle thread:** In `thread_yield()`, `list_insert_ordered()` is guarded by `if (cur != idle_thread)`. The idle thread must never be placed on the `ready_list` because the scheduler expects `ready_list` to contain only runnable worker threads, falling back to `idle_thread` only when empty.

### 3.3 Immediate Thread Preemption
File: [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L204-L206), [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L337-L345), [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L397-L416)

When a new thread is created in `thread_create()`:
```c
  /* Add to run queue. */
  thread_unblock (t);

  if (thread_current ()->priority < t->priority)
    thread_yield ();
```

When a thread modifies its priority in `thread_set_priority()`:
```c
void
thread_set_priority (int new_priority) 
{
  struct thread *cur = thread_current ();
  cur->base_priority = new_priority;
  thread_recompute_priority (cur);
  thread_yield_if_needed ();
}
```

Safe preemption helper `thread_yield_if_needed()`:
```c
void
thread_yield_if_needed (void)
{
  if (intr_context ())
    return;

  enum intr_level old_level = intr_disable ();
  if (!list_empty (&ready_list))
    {
      struct thread *highest = list_entry (list_front (&ready_list), struct thread, elem);
      if (thread_current ()->priority < highest->priority)
        {
          intr_set_level (old_level);
          thread_yield ();
          return;
        }
    }
  intr_set_level (old_level);
}
```

**Reasoning & Critical Constraints:**
- Checks `!list_empty(&ready_list)` before dereferencing `list_front(&ready_list)`. Calling `list_front` on an empty list reads unmapped memory and results in a fatal page fault.
- Returns immediately if `intr_context()` is true, ensuring context switches are never attempted during hardware interrupt handling.

### 3.4 Priority-Aware Synchronization (Semaphores & Condition Variables)
File: [`src/threads/synch.c`](file:///home/yasamin/pintos/src/threads/synch.c#L110-L127), [`src/threads/synch.c`](file:///home/yasamin/pintos/src/threads/synch.c#L365-L404)

In `sema_up()`:
```c
void
sema_up (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  if (!list_empty (&sema->waiters)) 
    {
      list_sort (&sema->waiters, thread_cmp_priority, NULL);
      thread_unblock (list_entry (list_pop_front (&sema->waiters),
                                  struct thread, elem));
    }
  sema->value++;
  intr_set_level (old_level);

  thread_yield_if_needed ();
}
```

In `cond_signal()`:
```c
static bool
cond_cmp_priority (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
  struct semaphore_elem *sa = list_entry (a, struct semaphore_elem, elem);
  struct semaphore_elem *sb = list_entry (b, struct semaphore_elem, elem);

  if (list_empty (&sa->semaphore.waiters))
    return false;
  if (list_empty (&sb->semaphore.waiters))
    return true;

  struct thread *ta = list_entry (list_front (&sa->semaphore.waiters), struct thread, elem);
  struct thread *tb = list_entry (list_front (&sb->semaphore.waiters), struct thread, elem);

  return ta->priority > tb->priority;
}

void
cond_signal (struct condition *cond, struct lock *lock UNUSED) 
{
  ...
  if (!list_empty (&cond->waiters)) 
    {
      list_sort (&cond->waiters, cond_cmp_priority, NULL);
      struct semaphore_elem *se = list_entry (list_pop_front (&cond->waiters),
                                              struct semaphore_elem, elem);
      sema_up (&se->semaphore);
    }
}
```

**Reasoning:**
- In `sema_up()`, sorting `sema->waiters` with `thread_cmp_priority` guarantees the thread with the highest effective priority is awoken first. After waking, `thread_yield_if_needed()` ensures the awoken thread preempts if its priority is higher than the currently executing thread.
- In `cond_signal()`, each waiting thread on a condition variable has its own `semaphore_elem`. To wake the highest-priority waiter, `cond_cmp_priority` compares the priority of the thread at the front of each semaphore's waiter list, sorts `cond->waiters`, and pops the highest.

### 3.5 Nested Priority Donation Logic

#### A. Acquisition & Donation Chain (`lock_acquire`)
File: [`src/threads/synch.c`](file:///home/yasamin/pintos/src/threads/synch.c#L218-L253)

```c
void
lock_acquire (struct lock *lock)
{
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (!lock_held_by_current_thread (lock));

  struct thread *cur = thread_current ();

  if (lock->holder != NULL)
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

  cur->lock_waiting_on = NULL;
  lock->holder = cur;
  list_push_back (&cur->locks_held, &lock->elem);
  lock->max_priority = lock_get_max_priority (lock);
}
```

#### B. Priority Donation Helper (`thread_donate_priority`)
File: [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L383-L395)

```c
void
thread_donate_priority (struct thread *t, int new_priority)
{
  enum intr_level old_level = intr_disable ();
  if (t->priority < new_priority)
    {
      t->priority = new_priority;
      if (t->status == THREAD_READY)
        list_sort (&ready_list, thread_cmp_priority, NULL);
    }
  intr_set_level (old_level);
}
```

#### C. Release & Priority Recalculation (`lock_release` & `thread_recompute_priority`)
Files: [`src/threads/synch.c`](file:///home/yasamin/pintos/src/threads/synch.c#L284-L297), [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L357-L381)

```c
void
lock_release (struct lock *lock) 
{
  ASSERT (lock != NULL);
  ASSERT (lock_held_by_current_thread (lock));

  struct thread *cur = thread_current ();

  list_remove (&lock->elem);
  thread_recompute_priority (cur);

  lock->holder = NULL;
  sema_up (&lock->semaphore);
}
```

```c
void
thread_recompute_priority (struct thread *t)
{
  enum intr_level old_level = intr_disable ();
  int max_pri = t->base_priority;

  struct list_elem *e;
  for (e = list_begin (&t->locks_held);
       e != list_end (&t->locks_held);
       e = list_next (e))
    {
      struct lock *l = list_entry (e, struct lock, elem);
      int lock_pri = lock_get_max_priority (l);
      l->max_priority = lock_pri;
      if (lock_pri > max_pri)
        max_pri = lock_pri;
    }

  t->priority = max_pri;
  if (t->status == THREAD_READY)
    list_sort (&ready_list, thread_cmp_priority, NULL);
  intr_set_level (old_level);
}
```

**Reasoning:**
- In `lock_acquire()`, if the lock is held, we record `cur->lock_waiting_on = lock` and trace up the chain of lock holders using a `while` loop (bounded by `depth < 8` to prevent infinite loops from circular waits). At each hop, we donate `cur->priority` if higher than the holder's current priority, and re-sort `ready_list` if that thread is ready.
- In `lock_release()`, the lock is removed from `cur->locks_held` **before** calling `thread_recompute_priority()`.
- `thread_recompute_priority()` starts with `t->base_priority` and iterates through all other locks still held by `t`. It evaluates the highest priority among all threads waiting on those locks (`lock_get_max_priority(l)`) and restores `t->priority` to the true highest value. Finally, `sema_up()` wakes the next lock acquirer and preempts if needed.

---

## 4. Test-by-Test Analysis: What Each Test Checks & Why It Passes

Below is a detailed breakdown of each of the 12 `priority-*` tests:

### 1. `priority-change`
- **What it tests**: Verifies that when a thread voluntarily lowers its own priority (`thread_set_priority()`) below that of a ready thread, it immediately yields the CPU.
- **Why it passes**: In `thread_set_priority()`, we call `thread_recompute_priority()` followed by `thread_yield_if_needed()`. Because the ready list has a thread with higher priority, the running thread immediately yields and control is transferred to the higher-priority thread.

### 2. `priority-donate-one`
- **What it tests**: A low-priority thread holds a lock. Two higher-priority threads (`acquire1` with priority 32, `acquire2` with priority 33) consecutively attempt to acquire the lock. The low-priority thread must inherit priority 32, then 33, complete its critical section, and when releasing the lock, `acquire2` must run before `acquire1`.
- **Why it passes**: In `lock_acquire()`, priority donation elevates the main thread's priority to 32 and then 33. When `lock_release()` is invoked, `sema_up()` sorts the waiters list by priority, waking `acquire2` first.

### 3. `priority-donate-multiple`
- **What it tests**: The main thread acquires two locks, $A$ and $B$. Thread $A$ (priority 32) waits on lock $A$, and Thread $B$ (priority 33) waits on lock $B$. The main thread's priority must become 33. When the main thread releases lock $B$, its priority must drop to 32 (not back to base 31), because it still holds lock $A$. Only after releasing lock $A$ should it return to 31.
- **Why it passes**: The `locks_held` list tracks both locks. When lock $B$ is released, `thread_recompute_priority()` inspects the remaining lock $A$, discovers thread $A$ waiting with priority 32, and keeps effective priority at 32.

### 4. `priority-donate-multiple2`
- **What it tests**: Similar to `priority-donate-multiple`, but locks are released in a different order (lock $A$ is released before lock $B$), and an independent thread $C$ with priority 32 is created that should not run until higher threads finish.
- **Why it passes**: When lock $A$ is released first, `thread_recompute_priority()` checks the remaining locks list (`locks_held`), finds lock $B$ with priority 36 waiting, and keeps the main thread's priority at 36. This prevents thread $C$ from erroneously running prematurely.

### 5. `priority-donate-nest`
- **What it tests**: Nested priority donation ($H \rightarrow M \rightarrow L$). Low-priority thread $L$ holds lock $A$. Medium-priority thread $M$ acquires lock $B$ and blocks waiting for lock $A$. High-priority thread $H$ blocks waiting for lock $B$. Thread $H$'s priority must donate through $M$ to thread $L$.
- **Why it passes**: In `lock_acquire()`, the `while (curr_lock != NULL && depth < 8)` loop inspects `holder->lock_waiting_on`. When $H$ blocks on $B$, it sees $B$'s holder is $M$, donates to $M$, sees $M$ is waiting on lock $A$, and donates directly to $A$'s holder $L$.

### 6. `priority-donate-sema`
- **What it tests**: Tests priority donation interacting with semaphores. Low-priority thread $L$ acquires a lock, then downs a semaphore. Medium thread $M$ downs the same semaphore. High-priority thread $H$ tries to acquire the lock held by $L$, donating priority to $L$. When the semaphore is upped, $L$ must wake before $M$ because $L$ has donated priority from $H$.
- **Why it passes**: `sema_up()` calls `list_sort(&sema->waiters, thread_cmp_priority, NULL)`. Because $L$'s effective priority was elevated by $H$'s donation, $L$ sits at the front of `sema->waiters` and is awoken before $M$.

### 7. `priority-donate-lower`
- **What it tests**: Main thread acquires a lock. A higher-priority thread blocks on that lock, donating priority. Main thread calls `thread_set_priority()` to lower its priority. The lowered priority must **not** take effect immediately while the lock is held, but must take effect as soon as the lock is released.
- **Why it passes**: We separate `t->base_priority` from `t->priority`. `thread_set_priority()` sets `cur->base_priority = new_priority`, but `thread_recompute_priority()` takes the maximum between `base_priority` and the highest donated priority of held locks. Once the lock is released, `recompute_priority` resets `t->priority` back to `t->base_priority`.

### 8. `priority-fifo`
- **What it tests**: Creates 16 threads with the exact same priority and verifies that they yield and run in strict FIFO round-robin order without starvation or reordering.
- **Why it passes**: In `thread_cmp_priority()`, we use strictly greater than (`>`), not `>=`. When a thread yields and calls `list_insert_ordered()`, existing ready threads with the same priority evaluate to false, causing the yielding thread to be inserted at the **tail** of its priority group.

### 9. `priority-preempt`
- **What it tests**: A low-priority thread creates a high-priority thread. The high-priority thread must run immediately before `thread_create()` finishes and returns to the creator.
- **Why it passes**: In `thread_create()`, right after `thread_unblock(t)`, we check:
  `if (thread_current ()->priority < t->priority) thread_yield ();`
  This immediately yields the CPU to the newly created thread.

### 10. `priority-sema`
- **What it tests**: 10 threads of varying priorities block on a single semaphore. When `sema_up()` is called 10 times, the threads must wake up strictly in descending order of priority.
- **Why it passes**: `sema_up()` sorts `sema->waiters` by `thread_cmp_priority` before popping the front element, guaranteeing the thread with the highest priority is awoken on each call.

### 11. `priority-condvar`
- **What it tests**: 10 threads of varying priorities wait on a condition variable (`cond_wait()`). When `cond_signal()` is called, the thread with the highest priority must wake up first.
- **Why it passes**: In Pintos, condition variables maintain a list of `semaphore_elem`s. In `cond_signal()`, we sort `cond->waiters` using `cond_cmp_priority()`, which inspects the priority of the thread waiting on each semaphore and selects the highest-priority waiter.

### 12. `priority-donate-chain`
- **What it tests**: An 8-level nested lock chain ($T_7 \rightarrow \dots \rightarrow T_1 \rightarrow \text{Main}$) with intermediate "interloper" threads. Each lock release must propagate along the chain, ensuring threads execute in strict priority sequence without allowing interlopers to run ahead of donors.
- **Why it passes**: Our nested donation loop supports traversing up to depth 8 (`depth < 8`). Priority is correctly propagated across all intermediate lock holders and recomputed dynamically at each step of lock release.

---

## 5. Summary of Files Changed

| File | Key Changes |
| :--- | :--- |
| [`src/threads/thread.h`](file:///home/yasamin/pintos/src/threads/thread.h) | Added `base_priority`, `locks_held`, `lock_waiting_on` to `struct thread`. Declared helper prototypes (`thread_cmp_priority`, `thread_recompute_priority`, `thread_donate_priority`, `thread_yield_if_needed`). |
| [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c) | Implemented `thread_cmp_priority`, `thread_recompute_priority`, `thread_donate_priority`, `thread_yield_if_needed`. Updated `thread_create`, `thread_unblock`, `thread_yield`, `thread_set_priority`, and initialized fields in `init_thread`. |
| [`src/threads/synch.h`](file:///home/yasamin/pintos/src/threads/synch.h) | Added `elem` and `max_priority` to `struct lock`. Declared `lock_get_max_priority`. |
| [`src/threads/synch.c`](file:///home/yasamin/pintos/src/threads/synch.c) | Implemented priority-aware `sema_up`, `cond_signal` with `cond_cmp_priority`. Implemented nested priority donation in `lock_acquire` and dynamic priority restoration in `lock_release`. |
