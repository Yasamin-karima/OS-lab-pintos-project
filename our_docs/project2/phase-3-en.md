# Pintos Project 2: User Programs — Phase 3 Engineering & Deep-Dive Study Report
## Process Lifecycle Management & Parent-Child Synchronization (SYS_EXEC, SYS_WAIT, SYS_EXIT, SYS_HALT)

---

## 1. Project & Phase Objectives

### 1.1 High-Level Project Goals (Pintos Project 2: User Programs)
In Pintos Project 2, the operating system transitions from executing exclusively trusted, cooperating kernel threads (Project 1) to executing untrusted user-space programs running under x86 hardware protection (Ring 3). The core responsibilities of the operating system in this environment are:
1. **Isolation & Protection:** Ensuring that an unprivileged user process cannot corrupt, inspect, or crash the kernel or other concurrent processes.
2. **Controlled Access (System Calls):** Providing a structured, secure gateway (via software interrupt `0x30`) through which user programs request kernel-mediated services (I/O, process control, file system operations).
3. **Resource Reclamation:** Cleanly reclaiming memory, locks, and file descriptors upon normal program exit or abnormal fault termination.
4. **Process Lifecycle Management:** Providing robust process creation (`exec`), waiting (`wait`), termination (`exit`), and machine shutdown (`halt`) semantics that mirror standard Unix process control.

---

### 1.2 Architectural Goals of Phase 3
Phase 1 established argument passing on the user stack, and Phase 2 implemented user memory validation and safe page fault handling. However, base Pintos lacked a coherent, race-free process lifecycle model:
- `process_execute()` was completely asynchronous: it returned the child's thread ID immediately after calling `thread_create()`, without checking whether the child actually succeeded in loading its executable file into virtual memory (`load()`). If the executable file did not exist (as in `exec-missing`) or was corrupted, `exec()` returned a valid PID instead of `TID_ERROR` (`-1`).
- `process_wait()` had an empty dummy implementation returning `-1`. Parents could neither synchronize on child termination nor retrieve the child's exit status.
- Once a child thread terminated in `thread_exit()`, its `struct thread` and 4 KB kernel page were immediately freed by the scheduler in `thread_schedule_tail()`. Any pointer held by the parent to the child's thread became a dangling pointer, leading to use-after-free bugs.
- There was no mechanism to handle the fundamental concurrency permutations of process lifecycles:
  1. A child terminates before its parent ever calls `wait()`.
  2. A parent waits on a child while the child is still executing.
  3. A parent exits without ever calling `wait()` on its children (orphan processes).
  4. A parent calls `wait()` more than once on the same child (`wait-twice`).
  5. A parent attempts to wait on an invalid PID or a process that is not its direct child (`wait-bad-pid`).
  6. A child crashes abnormally due to an unhandled exception or bad pointer (`wait-killed`).
  7. Hundreds of recursive processes spawn and terminate under extreme memory pressure without leaking a single byte of kernel heap or page memory (`multi-oom`).

The architectural goal of **Phase 3** is to design and implement a **reference-counted, semaphore-synchronized process lifecycle architecture**:
1. **Synchronous `exec()` Load Verification:** `process_execute()` blocks the parent thread on a dedicated semaphore (`load_sema`) until the child thread completes `load()`. If loading fails, `process_execute()` and `SYS_EXEC` return `TID_ERROR` (`-1`).
2. **Deterministic Parent-Child Synchronization:** A dedicated shared status structure (`struct child_status`) decouples process tracking from thread execution. A child signals termination via `wait_sema` and records its exit status.
3. **Zero Memory Leaks & Race-Free Deallocation:** A reference-counted (`ref_count = 2`) pattern protected by `ref_lock` ensures that whichever party (parent or child) finishes last frees the shared `child_status` block, completely eliminating memory leaks and dangling pointers under stress testing (`multi-oom`).
4. **System Call Integration:** Wire `SYS_EXEC`, `SYS_WAIT`, `SYS_EXIT`, and `SYS_HALT` into `syscall_handler()` with rigorous user argument validation.

---

### 1.3 Targeted and Verified Pintos Test Cases

| Test Case | Description & Verification Objective | Expected Exit / Behavior |
| :--- | :--- | :--- |
| `exec-once` | A parent process calls `exec("child-simple")` and calls `wait()` on the returned child PID. | Child prints `child-simple: exit(81)` and parent successfully waits, exiting with `81`. |
| `exec-arg` | Parent calls `exec("child-args childarg")` verifying that arguments pass correctly to the newly executed process image. | Child verifies arguments and exits with `0`; parent waits and exits with `0`. |
| `exec-multiple` | Parent process executes multiple children in sequence (`child-simple` multiple times) and waits on each. | Each child completes and exits with `81`; parent retrieves `81` for each child. |
| `exec-missing` | Parent attempts to call `exec("no-such-file")` for a nonexistent binary. | `load()` fails; `process_execute()` detects failure via `load_sema` and returns `-1`; parent prints `exec("no-such-file"): -1`. |
| `exec-bad-ptr` | Passes an invalid, unmapped pointer (`0x20101234`) as the command-line string to `exec()`. | `validate_user_string()` rejects the pointer; calling process terminates with exit status `-1`. |
| `wait-simple` | Parent spawns `child-simple` and immediately calls `wait(child_pid)`. | Parent blocks until child exits; child exits with `81`; parent unblocks and returns `81`. |
| `wait-twice` | Parent spawns `child-simple`, calls `wait(child_pid)` (succeeds with `81`), and then calls `wait(child_pid)` a second time. | Second call to `wait()` must immediately return `-1` without blocking. |
| `wait-killed` | Parent spawns `child-bad`, which triggers a user-mode exception by reading from unmapped memory `*(volatile int *) NULL`. | Kernel terminates child with `-1`; parent's `wait(child_pid)` receives exit status `-1`. |
| `wait-bad-pid` | Parent calls `wait()` with an arbitrary nonexistent PID (e.g., `wait(0x42)`). | Kernel searches parent's children list, fails to find the PID, and immediately returns `-1`. |
| `multi-oom` | Stresses kernel page and heap allocators by recursively spawning children up to 10 repetitions, each time recursing until physical memory is exhausted (`EXPECTED_DEPTH_TO_PASS >= 30`). Tests abnormal crash children (`-k`) and open file descriptors. | Kernel must not crash, panic, or leak memory. Reaches identical recursion depth across all 10 repetitions. |

---

## 2. Detailed Code Walkthrough

Four source files implement Phase 3:
1. `src/threads/thread.h`: Definitions for `struct child_status` and process control fields in `struct thread`.
2. `src/threads/thread.c`: Reference-count release helper and thread initialization.
3. `src/userprog/process.c`: Synchronous `process_execute()`, `start_process()`, `process_wait()`, and `process_exit()`.
4. `src/userprog/syscall.c`: System call handlers for `SYS_EXEC`, `SYS_WAIT`, `SYS_EXIT`, `SYS_HALT`, and file resource stubs.

---

### 2.1 File: `src/threads/thread.h`

#### 2.1.1 Parent-Child Tracking Structure: `struct child_status` (Lines 26–41)
```c
#ifdef USERPROG
struct child_status
  {
    tid_t tid;
    int exit_status;
    bool waited;
    bool exited;
    bool load_success;
    int ref_count;
    struct lock ref_lock;
    struct semaphore load_sema;
    struct semaphore wait_sema;
    struct list_elem elem;
  };

void release_child_status (struct child_status *cp);
#endif
```
- **Line Numbers:** Lines 26–41 in `src/threads/thread.h`.
- **Why Needed:** In base Pintos, thread state was tied directly to `struct thread`. When a thread died, `struct thread` was deallocated, destroying its exit status before the parent could read it. `struct child_status` lives independently on the kernel heap (`malloc`), persisting across the child's termination so the parent can safely retrieve `exit_status`.
- **Field Breakdown:**
  - `tid`: The thread/process identifier of the child.
  - `exit_status`: Exit status code passed to `exit()` or `-1` if killed by kernel exception; initialized to `-1`.
  - `waited`: Boolean flag indicating whether the parent has already waited on this child (prevents `wait-twice`).
  - `exited`: Boolean flag indicating whether the child has completed `process_exit()`.
  - `load_success`: Boolean flag set by the child in `start_process()` indicating whether `load()` succeeded.
  - `ref_count`: Reference counter initialized to `2` (1 held by parent, 1 held by child).
  - `ref_lock`: Mutual exclusion lock protecting decrements of `ref_count`.
  - `load_sema`: Semaphore initialized to `0`; child signals `sema_up(&load_sema)` once loading is complete, unblocking `process_execute()`.
  - `wait_sema`: Semaphore initialized to `0`; child signals `sema_up(&wait_sema)` in `process_exit()`, unblocking `process_wait()`.
  - `elem`: List element for insertion into the parent's `cur->children` list.
- **Runtime Impact:** Provides a thread-safe, heap-allocated synchronization anchor between the parent and child threads.

---

#### 2.1.2 Userprog Fields in `struct thread` (Lines 126–132)
```c
#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint32_t *pagedir;                  /**< Page directory. */
    struct list children;               /**< List of children (struct child_status). */
    struct child_status *cp;            /**< Child status tracking this thread. */
    int exit_status;                    /**< Exit status. */
#endif
```
- **Line Numbers:** Lines 126–132 in `src/threads/thread.h`.
- **Why Needed:** 
  - `children`: A list head in each parent thread to maintain records of all its direct children.
  - `cp`: A pointer inside each child thread pointing back to its shared `child_status` structure so it can record its exit code and up its semaphores on exit.
  - `exit_status`: Stores the thread's exit code (defaults to `-1`).
- **Runtime Impact:** Allows any running process to locate its own tracking record (`cur->cp`) and iterate through its children (`cur->children`).

---

### 2.2 File: `src/threads/thread.c`

#### 2.2.1 Header Inclusions (Lines 15–18)
```c
#ifdef USERPROG
#include "userprog/process.h"
#include "threads/malloc.h"
#endif
```
- **Line Numbers:** Lines 15–18 in `src/threads/thread.c`.
- **Why Needed:** `threads/malloc.h` provides `free()`, required by `release_child_status()` when the reference count drops to zero.

---

#### 2.2.2 Process Fields Initialization in `init_thread()` (Lines 677–682)
```c
#ifdef USERPROG
  t->pagedir = NULL;
  list_init (&t->children);
  t->cp = NULL;
  t->exit_status = -1;
#endif
```
- **Line Numbers:** Lines 677–682 in `src/threads/thread.c`.
- **Why Needed:** Every new thread (kernel or user) must have its child list initialized and its status pointers set to `NULL` to avoid traversing garbage pointers.
- **Runtime Impact:** `children` is an empty doubly-linked list ready for child additions.

---

#### 2.2.3 Reference-Counted Cleanup: `release_child_status()` (Lines 803–820)
```c
#ifdef USERPROG
/** Safely decrements cp->ref_count under cp->ref_lock.
   If ref_count == 0 after releasing the lock, frees cp. */
void
release_child_status (struct child_status *cp)
{
  if (cp == NULL)
    return;

  lock_acquire (&cp->ref_lock);
  cp->ref_count--;
  bool should_free = (cp->ref_count == 0);
  lock_release (&cp->ref_lock);

  if (should_free)
    free (cp);
}
#endif
```
- **Line Numbers:** Lines 803–820 in `src/threads/thread.c`.
- **Why Needed:** Solves the core lifecycle dilemma: either the parent exits before the child, or the child exits before the parent. Neither party alone can unconditionally free `struct child_status` without risking a dangling pointer or a memory leak.
- **Runtime Impact:** 
  - Under `cp->ref_lock`, decrements `cp->ref_count`.
  - If `ref_count` reaches `0`, the caller is the last referencing party; `free(cp)` releases the heap block.
  - If `ref_count` is still `1`, the other party is still using it; `cp` remains allocated.

---

### 2.3 File: `src/userprog/process.c`

#### 2.3.1 Execution Arguments Structure (Lines 22–26)
```c
struct exec_args
  {
    char *fn_copy;
    struct child_status *cp;
  };
```
- **Line Numbers:** Lines 22–26 in `src/userprog/process.c`.
- **Why Needed:** `thread_create()` accepts only a single `void *aux` parameter. The child needs both the raw command-line string page (`fn_copy`) and the shared `child_status` pointer (`cp`).
- **Runtime Impact:** Placed on the parent's stack in `process_execute()`. Because the parent blocks on `sema_down(&cp->load_sema)` before `start_process()` finishes using `aux`, the stack memory remains completely valid without requiring heap allocation.

---

#### 2.3.2 Synchronous `process_execute()` (Lines 37–105)
```c
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy;
  char prog_name[16];
  size_t i;
  tid_t tid;
  struct exec_args args;
  struct thread *cur = thread_current ();

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  /* Extract program name for thread_create(). */
  for (i = 0; i < sizeof prog_name - 1 && fn_copy[i] != '\0' && fn_copy[i] != ' '; i++)
    prog_name[i] = fn_copy[i];
  prog_name[i] = '\0';

  if (prog_name[0] == '\0')
    {
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }

  struct child_status *cp = malloc (sizeof (struct child_status));
  if (cp == NULL)
    {
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }

  cp->tid = TID_ERROR;
  cp->exit_status = -1;
  cp->waited = false;
  cp->exited = false;
  cp->load_success = false;
  cp->ref_count = 2;
  lock_init (&cp->ref_lock);
  sema_init (&cp->load_sema, 0);
  sema_init (&cp->wait_sema, 0);

  args.fn_copy = fn_copy;
  args.cp = cp;

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (prog_name, PRI_DEFAULT, start_process, &args);
  if (tid == TID_ERROR)
    {
      palloc_free_page (fn_copy);
      free (cp);
      return TID_ERROR;
    }

  cp->tid = tid;
  list_push_back (&cur->children, &cp->elem);
  sema_down (&cp->load_sema);

  if (!cp->load_success)
    {
      list_remove (&cp->elem);
      release_child_status (cp);
      return TID_ERROR;
    }

  return tid;
}
```
- **Line Numbers:** Lines 37–105 in `src/userprog/process.c`.
- **Why Needed:** 
  1. Base Pintos allocated an unnecessary second 4 KB page just to extract the program name, causing early failures in `multi-oom`. Here, `prog_name` is extracted using a local 16-byte stack buffer.
  2. Base Pintos did not wait for the child to load. If loading failed (e.g. `exec-missing`), `process_execute()` returned a valid TID.
- **Runtime Impact:**
  - Allocates 1 page for `fn_copy` and 1 heap block for `cp`.
  - Initializes `ref_count = 2`, `load_sema = 0`, `wait_sema = 0`.
  - Spawns child via `thread_create()`. On failure, cleanly frees `fn_copy` and `cp` and returns `TID_ERROR`.
  - Adds `cp` to `cur->children` and blocks via `sema_down(&cp->load_sema)`.
  - When unblocked, checks `cp->load_success`. If `false`, removes `cp` from `cur->children`, releases parent's reference via `release_child_status(cp)`, and returns `TID_ERROR` (`-1`).

---

#### 2.3.3 Initializing Child in `start_process()` (Lines 109–163)
```c
static void
start_process (void *aux)
{
  struct exec_args *args = aux;
  char *file_name = args->fn_copy;
  struct child_status *cp = args->cp;
  struct thread *cur = thread_current ();
  struct intr_frame if_;
  bool success;

  cur->cp = cp;

  char *argv[128];
  int argc = 0;
  char *token, *save_ptr;

  for (token = strtok_r (file_name, " ", &save_ptr); token != NULL;
       token = strtok_r (NULL, " ", &save_ptr))
    {
      if (argc < 127)
        argv[argc++] = token;
    }
  argv[argc] = NULL;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  if (argc > 0)
    success = load (argv[0], &if_.eip, &if_.esp, argc, argv);
  else
    success = false;

  palloc_free_page (file_name);
  cp->load_success = success;
  sema_up (&cp->load_sema);

  /* If load failed, quit. */
  if (!success)
    {
      cur->exit_status = -1;
      thread_exit ();
    }

  /* Start the user process by simulating a return from an interrupt... */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}
```
- **Line Numbers:** Lines 109–163 in `src/userprog/process.c`.
- **Why Needed:** Immediately associates `cur->cp = cp` before parsing arguments or loading ELF binaries. Ensures that `palloc_free_page(file_name)` is **always** called, `cp->load_success` is updated, and `sema_up(&cp->load_sema)` is executed before deciding whether to exit or switch to user mode.
- **Runtime Impact:**
  - Guarantees the waiting parent in `process_execute()` unblocks deterministically.
  - If loading fails, sets `cur->exit_status = -1` and exits via `thread_exit()`.
  - Frees the 4 KB command-line page before running the program, preventing memory waste.

---

#### 2.3.4 Waiting for Child Termination: `process_wait()` (Lines 171–199)
```c
int
process_wait (tid_t child_tid) 
{
  struct thread *cur = thread_current ();
  struct list_elem *e;
  struct child_status *cp = NULL;

  for (e = list_begin (&cur->children); e != list_end (&cur->children); e = list_next (e))
    {
      struct child_status *candidate = list_entry (e, struct child_status, elem);
      if (candidate->tid == child_tid)
        {
          cp = candidate;
          break;
        }
    }

  if (cp == NULL || cp->waited)
    return -1;

  cp->waited = true;
  sema_down (&cp->wait_sema);

  int status = cp->exit_status;
  list_remove (&cp->elem);
  release_child_status (cp);

  return status;
}
```
- **Line Numbers:** Lines 171–199 in `src/userprog/process.c`.
- **Why Needed:** In base Pintos, `process_wait()` immediately returned `-1`. Parents could not synchronize on child exit.
- **Runtime Impact:**
  - Searches `cur->children` for `child_tid`. If the TID does not correspond to an immediate child, returns `-1` (`wait-bad-pid`).
  - Checks `cp->waited`. If `true`, returns `-1` immediately (`wait-twice`).
  - Sets `cp->waited = true` and blocks on `sema_down(&cp->wait_sema)`.
  - If the child is already dead, `wait_sema` is already positive, so `sema_down` returns without sleeping.
  - If the child is still running, the parent sleeps until the child calls `sema_up(&cp->wait_sema)` in `process_exit()`.
  - Captures `status = cp->exit_status`, removes `cp` from `cur->children`, calls `release_child_status(cp)` (parent's reference), and returns `status`.

---

#### 2.3.5 Releasing Resources in `process_exit()` (Lines 202–243)
```c
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

  if (cur->pagedir != NULL)
    printf ("%s: exit(%d)\n", cur->name, cur->exit_status);

  if (cur->cp != NULL)
    {
      cur->cp->exit_status = cur->exit_status;
      cur->cp->exited = true;
      sema_up (&cur->cp->wait_sema);
      release_child_status (cur->cp);
      cur->cp = NULL;
    }

  while (!list_empty (&cur->children))
    {
      struct list_elem *e = list_pop_front (&cur->children);
      struct child_status *cp = list_entry (e, struct child_status, elem);
      release_child_status (cp);
    }

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}
```
- **Line Numbers:** Lines 202–243 in `src/userprog/process.c`.
- **Why Needed:** Ensures that:
  1. The required `<name>: exit(<status>)` message is emitted only for user processes (`pagedir != NULL`).
  2. The waiting parent is unblocked via `sema_up(&cur->cp->wait_sema)`.
  3. The child releases its own reference (`release_child_status(cur->cp)`).
  4. Any children that this exiting process spawned but never waited upon are cleaned up: the parent's reference to each child is released (`release_child_status(cp)`), preventing memory leaks for orphaned children.
  5. The user page directory and all mapped user physical pages are returned to the user pool via `pagedir_destroy()`.

---

### 2.4 File: `src/userprog/syscall.c`

#### 2.4.1 System Call Dispatcher & Handlers (Lines 93–161)
```c
  switch (syscall_num)
    {
    case SYS_HALT:
      shutdown_power_off ();
      break;

    case SYS_EXIT:
      validate_user_range ((const uint8_t *) f->esp + 4, sizeof (int));
      {
        int status = *((int *) f->esp + 1);
        struct thread *cur = thread_current ();
        cur->exit_status = status;
        thread_exit ();
      }
      break;

    case SYS_EXEC:
      validate_user_range ((const uint8_t *) f->esp + 4, sizeof (char *));
      {
        const char *cmd_line = (const char *) *((uintptr_t *) f->esp + 1);
        validate_user_string (cmd_line);
        f->eax = process_execute (cmd_line);
      }
      break;

    case SYS_WAIT:
      validate_user_range ((const uint8_t *) f->esp + 4, sizeof (tid_t));
      {
        tid_t child_tid = *((int *) f->esp + 1);
        f->eax = process_wait (child_tid);
      }
      break;

    case SYS_WRITE:
      validate_user_range ((const uint8_t *) f->esp + 4, sizeof (int) * 3);
      {
        int fd = *((int *) f->esp + 1);
        const void *buffer = (const void *) *((uintptr_t *) f->esp + 2);
        unsigned size = *((unsigned *) f->esp + 3);
        validate_user_range (buffer, size);
        if (fd == 1)
          {
            putbuf (buffer, size);
            f->eax = size;
          }
        else
          {
            f->eax = 0;
          }
      }
      break;

    case SYS_OPEN:
      validate_user_range ((const uint8_t *) f->esp + 4, sizeof (char *));
      {
        const char *file = (const char *) *((uintptr_t *) f->esp + 1);
        validate_user_string (file);
        f->eax = -1;
      }
      break;

    case SYS_CLOSE:
      validate_user_range ((const uint8_t *) f->esp + 4, sizeof (int));
      break;

    default:
      terminate_invalid_process ();
      break;
    }
```
- **Line Numbers:** Lines 93–161 in `src/userprog/syscall.c`.
- **Why Needed:** 
  - `SYS_HALT`: Calls `shutdown_power_off()` to power off QEMU/Bochs.
  - `SYS_EXIT`: Reads `status` after validating the 4-byte stack offset `esp + 4`, sets `cur->exit_status = status`, and calls `thread_exit()`.
  - `SYS_EXEC`: Validates the 4-byte string pointer `cmd_line`, verifies every character up to `\0` with `validate_user_string(cmd_line)`, and stores the result of `process_execute(cmd_line)` in `f->eax`.
  - `SYS_WAIT`: Validates the 4-byte `child_tid`, calls `process_wait(child_tid)`, and places the returned exit code in `f->eax`.
  - `SYS_OPEN` & `SYS_CLOSE`: In Phase 3, file system system calls have not yet been implemented. However, `multi-oom` calls `open()` up to 126 times (`consume_some_resources()`) and tests passing `(char *)PHYS_BASE` to `open()`. By validating the string argument and returning `-1`, invalid pointers cleanly terminate the test with exit status `-1` (`consume_some_resources_and_die()`), while valid calls fail gracefully without killing the recursing processes.

---

## 3. Operating System Concepts & Deep-Dive Study Guide

### 3.1 Process Hierarchy, Trees, and the Lifecycle Model
Operating systems organize processes into a strict hierarchical tree:
- In POSIX, a parent creates a child by cloning itself via `fork()`, and the child then replaces its address space via `exec()`.
- In Pintos (similar to Windows `CreateProcess` or Unix `spawn`), `exec(cmd_line)` combines address space creation, ELF loading, and thread initialization into a single atomic operational model.

```
                  [ Root Process (PID 1) ]
                        /          \
                       /            \
             [ Child A (PID 2) ]   [ Child B (PID 3) ]
                   /
                  /
         [ Grandchild (PID 4) ]
```

Every user process has exactly one parent. The parent-child relationship imposes strict rules:
1. **Scope of Wait:** A process can wait **only** on its immediate direct children. A process cannot wait on a sibling, parent, grandchild, or an arbitrary PID.
2. **Deterministic Exit Collection:** When a process exits, its return value must remain accessible until its parent explicitly reads it via `wait()`.
3. **Single Wait Guarantee:** An exit status can be collected at most once. Successive calls to `wait()` on the same child must fail (`-1`).

---

### 3.2 The Zombie Process Dilemma and Reference Counting
When a child process finishes executing, the operating system faces a classic dilemma:
- If the OS immediately destroys the child's entire state, the exit status is lost. When the parent calls `wait()`, the child no longer exists to report how it died.
- If the OS keeps the child's thread and stack intact, system memory is consumed by dead processes ("zombies"). Under heavy load or deep recursion (`multi-oom`), the system quickly exhausts RAM.

#### The Pintos Reference-Counted Solution
Pintos decouples the **thread execution context** (the 4 KB page holding `struct thread` and the kernel stack) from the **child status tracking block** (`struct child_status` on the heap):

```
+--------------------------------------------------------------------------------+
|                         struct child_status (Heap Allocated)                   |
|  - tid: 42                                                                     |
|  - exit_status: 81                                                             |
|  - ref_count: 2  <---+ Reference 1 (Parent)          <---+ Reference 2 (Child) |
|  - load_sema: [0]    |                                   |                     |
|  - wait_sema: [0]    |                                   |                     |
+----------------------|-----------------------------------|---------------------+
                       |                                   |
           Parent struct thread                    Child struct thread
          +--------------------+                  +--------------------+
          | children: list     |                  | cp: pointer        |
          +--------------------+                  +--------------------+
```

1. **Child Exits First (Scenario A):**
   - The child process completes execution and calls `process_exit()`.
   - The child sets `cp->exit_status = cur->exit_status` and signals `sema_up(&cp->wait_sema)`.
   - The child calls `release_child_status(cp)`. Under `ref_lock`, `ref_count` drops from `2` to `1`.
   - The child's `struct thread` and 4 KB kernel page are freed by the scheduler (`thread_schedule_tail()`).
   - `struct child_status` **remains alive** on the heap with `ref_count == 1`.
   - Later, the parent calls `process_wait()`. `sema_down(&cp->wait_sema)` returns immediately.
   - The parent reads `exit_status`, removes `cp` from `children`, and calls `release_child_status(cp)`.
   - Under `ref_lock`, `ref_count` drops from `1` to `0`. The parent calls `free(cp)`. Memory is reclaimed!

2. **Parent Exits First (Scenario B - Orphaned Children):**
   - The parent exits while its children are still executing or without having called `wait()`.
   - In `process_exit()`, the parent iterates through `cur->children`.
   - For each child, the parent calls `release_child_status(cp)`.
   - Under `ref_lock`, `ref_count` drops from `2` to `1`.
   - Later, when each child exits, it executes `process_exit()` and calls `release_child_status(cur->cp)`.
   - Under `ref_lock`, `ref_count` drops from `1` to `0`. The exiting child calls `free(cp)`. Memory is reclaimed!

Zero memory leaks occur in either scenario!

---

### 3.3 Control Flow & Synchronization Timelines

#### Diagram 1: Synchronous `process_execute()` Timeline
```
Parent Thread (Process A)                          Child Thread (Process B)
------------------------                          ------------------------
process_execute("child")
  |-- Allocate fn_copy page
  |-- Allocate struct child_status (cp)
  |-- thread_create("child", ..., start_process)
  |     |-- creates Thread B
  |     +-- unblocks Thread B
  |-- list_push_back(&children, &cp->elem)
  |-- sema_down(&cp->load_sema) [BLOCKS]
  .                                                start_process(aux)
  .                                                  |-- cur->cp = cp
  .                                                  |-- Parse argv
  .                                                  |-- success = load(...)
  .                                                  |-- palloc_free_page(fn_copy)
  .                                                  |-- cp->load_success = success
  .                                                  |-- sema_up(&cp->load_sema) ---+
  |<-------------------------------------------------+ (WAKES UP PARENT)            |
  |-- if (!cp->load_success) -> return -1            |                              |
  |-- return tid                                     +-- if (!success) thread_exit()
  |                                                  +-- intr_exit (enters user Ring 3)
```

#### Diagram 2: `process_wait()` and `process_exit()` Timeline
```
Parent Thread                                      Child Thread
-------------                                      ------------
process_wait(child_tid)                            User code executes...
  |-- Search cur->children for child_tid             |
  |-- if (cp->waited) return -1                      |
  |-- cp->waited = true                              |-- Calls exit(81) / fault
  |-- sema_down(&cp->wait_sema) [BLOCKS]             |-- thread_exit()
  .                                                    |-- process_exit()
  .                                                    |     |-- printf("child: exit(81)")
  .                                                    |     |-- cur->cp->exit_status = 81
  .                                                    |     |-- sema_up(&cur->cp->wait_sema) --+
  |<---------------------------------------------------+-----+ (WAKES UP PARENT)               |
  |-- status = cp->exit_status                         |     |-- release_child_status(cp)       |
  |-- list_remove(&cp->elem)                           |     |   (ref_count: 2 -> 1)            |
  |-- release_child_status(cp)                         |     +-- pagedir_destroy()              |
  |   (ref_count: 1 -> 0 -> free(cp))                  +-- Scheduler frees thread page          |
  +-- return status (81)
```

---

### 3.4 Common Pitfalls and Debugging Guide

#### Pitfall 1: Over-Allocating Pages in `process_execute()`
- **Symptom:** `multi-oom` fails after recursion depth ~15 with `FAIL: should have forked at least 30 times`.
- **Root Cause:** Base implementations frequently allocated a second 4 KB page with `palloc_get_page(0)` solely to extract the program name before calling `thread_create()`. In Pintos, there are only ~367 user pool pages and ~367 kernel pool pages. Allocating 2 pages per `exec()` consumes the kernel pool twice as fast.
- **Solution:** Extract the program name using a local stack buffer (`char prog_name[16]`) directly from `fn_copy`. Stack buffers consume no heap and no pages.

#### Pitfall 2: Releasing References Under Active Locks
- **Symptom:** Kernel panic or deadlock inside `free()` or `lock_acquire()`.
- **Root Cause:** Calling `free(cp)` while holding `&cp->ref_lock`. The memory holding the lock is returned to the heap while still locked.
- **Solution:** Determine `bool should_free = (cp->ref_count == 0);`, release `&cp->ref_lock`, and **then** conditionally invoke `free(cp)`.

#### Pitfall 3: Child Thread Deadlocking Parent on Load Failure
- **Symptom:** Kernel hangs indefinitely during `exec-missing`.
- **Root Cause:** In `start_process()`, if `load()` returned `false`, an early exit branch called `thread_exit()` before executing `sema_up(&cp->load_sema)`. The parent thread remained blocked on `sema_down(&cp->load_sema)` forever.
- **Solution:** Set `cp->load_success = success;` and immediately invoke `sema_up(&cp->load_sema)` unconditionally, before checking `if (!success)`.

#### Pitfall 4: Memory Leaks from Unwaited Children in `multi-oom`
- **Symptom:** `multi-oom` passes iteration 1 but fails in iteration 2 with `actual depth != expected depth`.
- **Root Cause:** In `multi-oom`, some processes spawn children with `-k` (crash mode) or recurse without waiting on every child before exiting. If `process_exit()` does not drain `cur->children` and release each child's status reference, heap blocks accumulate across iterations, causing subsequent runs to achieve lower depth.
- **Solution:** Always drain `cur->children` in `process_exit()` and call `release_child_status(cp)` on every remaining element.
