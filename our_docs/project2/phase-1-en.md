# Pintos Project 2: User Programs — Phase 1 Technical & Educational Report
**Phase 1: Argument Passing, Process Wait/Exit Synchronization, and Basic System Calls**

---

## 1. Project & Phase Objectives

### 1.1 Overview of Pintos Project 2 (User Programs)
In Pintos Project 1, all execution occurred entirely within the kernel address space. Threads shared a single global address space, kernel code had direct access to all physical memory, and CPU privileges remained at privilege level 0 (Ring 0 / Supervisor Mode).

Pintos Project 2 introduces the fundamental abstraction of modern operating systems: **user processes**. A user process is an isolated execution context running in unprivileged CPU mode (Ring 3 / User Mode) with its own virtual address space, user stack, and restricted instruction set. User processes cannot directly access kernel data structures, hardware devices, or other processes' memory. When a user process requires services from the operating system (e.g., console output, file system access, or process creation), it must issue a **system call** to trap into the kernel.

### 1.2 Specific Goals of Phase 1
Phase 1 establishes the foundational infrastructure required to execute any user program and pass arguments to it:
1. **Command-Line Parsing:** Safely parse the command-line string passed to `process_execute()`, separating the executable program name from its arguments without exceeding kernel buffer limits.
2. **x86 User Stack Construction (Argument Passing):** Set up the user virtual stack page immediately below `PHYS_BASE` (`0xc0000000`), pushing argument strings, word-alignment padding, the `argv` array of pointers, `argv`, `argc`, and a fake return address in strict compliance with the x86 32-bit `cdecl` calling convention.
3. **Process Synchronization & Lifecycle Tracking:** Implement parent-child lifecycle tracking (`struct child_status`) with thread-safe synchronization (`struct semaphore`) and reference counting (`ref_cnt`) to eliminate race conditions between parent and child during loading (`process_execute`) and termination (`process_wait`, `process_exit`).
4. **Basic System Call Handling & Memory Verification:** Build the core system call dispatcher (`int $0x30`) with strict pointer and buffer validation against user virtual address bounds and active page directories, implementing `halt`, `exit`, `write` (STDOUT), `wait`, and `exec`.

### 1.3 Targeted and Verified Pintos Test Cases
The implementation in Phase 1 satisfies and verifies the following Pintos test suites:

| Test Name | Targeted Capability | Verification Check |
|---|---|---|
| `args-none` | Stack initialization with 0 arguments | Verifies `argc == 1`, `argv[0] == "args-none"`, `argv[1] == NULL`, and exit status `0`. |
| `args-single` | Single argument passing | Verifies `argc == 2`, `argv[0] == "args-single"`, `argv[1] == "onearg"`, `argv[2] == NULL`. |
| `args-multiple` | Multiple arguments passing | Verifies `argc == 5`, parses four distinct words, ensures correct pointer ordering and string contents. |
| `args-many` | High argument count | Passes 23 arguments (`a` through `v`), verifying stack space, pointer indexing, and stack boundary integrity. |
| `args-dbl-space` | Delimiter handling | Verifies that consecutive spaces between arguments are collapsed and do not produce empty string tokens. |
| `halt` | Power management | Verifies `SYS_HALT` successfully powers down the machine via `shutdown_power_off()`. |
| `exit` | Process termination | Verifies `SYS_EXIT` sets the thread's exit code and prints `<process_name>: exit(<code>)`. |
| `wait-simple` | Child wait synchronization | Verifies that a parent process blocks on `wait()` until the child exits and retrieves its exit code. |
| `wait-twice` | Wait idempotency | Verifies that calling `wait()` a second time on the same child immediately returns `-1`. |
| `wait-bad-pid` | Wait validation | Verifies that calling `wait()` on an invalid TID or non-child process immediately returns `-1`. |
| `wait-killed` | Abnormal child termination | Verifies that if a child process terminates due to an exception, the parent's `wait()` returns `-1`. |
| `exec-multiple` | Multiple process execution | Verifies that a parent process can execute child processes concurrently and wait on them. |
| `exec-missing` | Nonexistent binary handling | Verifies that attempting to execute a missing binary safely returns `-1` without panicking the kernel. |
| `exec-bad-ptr` | Pointer validation in `exec` | Verifies that passing an invalid pointer string to `exec` terminates the offending process with `-1`. |

---

## 2. Detailed Code Walkthrough by File and Line Numbers

### 2.1 File: `src/filesys/filesys.h`
#### Line 12: External Declaration of `fs_device`
```c
/** Block device that contains the file system. */
extern struct block *fs_device;
```
- **Why this change was necessary:**
  In base Pintos, line 12 contained `struct block *fs_device;` without the `extern` keyword. Modern compilers (GCC 10+) enable `-fno-common` by default, treating uninitialized global variables in header files as strong definitions. When multiple compilation units (`filesys.c`, `init.c`, `shutdown.c`, `process.c`) included `filesys.h`, the GNU linker (`ld`) failed with `multiple definition of 'fs_device'`.
- **Runtime Impact:**
  `fs_device` is now allocated as a single global variable in `src/filesys/filesys.c` and referenced externally by other compilation units, allowing clean kernel linkage without duplicate symbol collisions.

---

### 2.2 File: `src/Make.config`
#### Line 43: Adding `-fcommon` to `CFLAGS`
```c
CFLAGS = -g -m32 -O -fno-strict-aliasing -fcommon -Wa,--divide -fno-pie -fno-stack-protector -mno-sse
```
- **Why this change was necessary:**
  User-level test frameworks (such as `tests/lib.c` and `tests/userprog/multi-recurse.c`) declare and define `const char *test_name` in ways that require traditional common symbol merging. Under `-fno-common`, compiling test programs fails with duplicate symbol errors.
- **Runtime Impact:**
  Enables standard legacy C tentative definition merging for Pintos user test suites.

---

### 2.3 File: `src/Makefile.userprog`
#### Line 6: Disabling Separate Code Program Headers
```c
# Linker flags.
$(PROGS): LDFLAGS += -nostdlib -static -Wl,-T,$(LDSCRIPT) -Wl,-z,noseparate-code
```
- **Why this change was necessary:**
  Modern GNU `ld` (binutils 2.30+) defaults to `-z separate-code`, which isolates the ELF file header and read-only text into two distinct `PT_LOAD` segments. Both segments claimed virtual address `0x08048000`. When Pintos's ELF loader parsed `args-none`, the first segment mapped `0x08048000`, and the second segment attempted to map `0x08048000` again. `install_page()` detected that the page was already mapped and failed, causing `load()` to abort and user programs to exit with `-1`.
- **Runtime Impact:**
  `-Wl,-z,noseparate-code` instructs the linker to merge headers and executable code into a single, contiguous `PT_LOAD` segment starting at `0x08048000`, matching Pintos's original single-segment text loader design.

---

### 2.4 File: `src/threads/thread.h`
#### Lines 4–8 & 25–38: `struct child_status` Definition
```c
#include "threads/synch.h"

#ifdef USERPROG
/** Status and synchronization tracking for child processes. */
struct child_status
  {
    tid_t tid;                          /**< Child's thread id. */
    int exit_status;                    /**< Child's exit status. */
    bool waited;                        /**< Has parent already waited on this child? */
    bool exited;                        /**< Has child terminated? */
    struct semaphore sema;              /**< Signaled when child exits. */
    struct lock lock;                   /**< Protects ref_cnt and state updates. */
    int ref_cnt;                        /**< Reference counter (parent + child). */
    struct list_elem elem;              /**< Element in parent's children list. */
  };
#endif
```
- **Why this change was necessary:**
  When a thread calls `thread_exit()`, base Pintos immediately deallocates the thread's 4KB memory page (`palloc_free_page(t)` inside `thread_schedule_tail()`). Consequently, if a parent thread calls `process_wait()` after a child has already died, any pointer to `struct thread` would be dangling (use-after-free).
- **Runtime Impact:**
  `struct child_status` is dynamically allocated in heap memory via `malloc()`. It decouples process lifecycle tracking from the volatile `struct thread`. Because both the parent and the child share ownership of this structure, reference counting (`ref_cnt = 2`) ensures that whichever process outlives the other will safely free the memory.

#### Lines 124–129: Fields Added to `struct thread`
```c
#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint32_t *pagedir;                  /**< Page directory. */
    struct list children;               /**< List of child_status structs. */
    struct child_status *child_status;  /**< This thread's child_status in parent. */
    int exit_status;                    /**< Exit status. */
#endif
```
- **Why this change was necessary:**
  - `children`: A parent thread must keep track of all its children to validate `process_wait(child_tid)`.
  - `child_status`: A child thread must be able to update its exit code and up its exit semaphore upon death.
  - `exit_status`: Stores the return code passed to `SYS_EXIT` or sets `-1` on kernel exception.
- **Runtime Impact:**
  Every process has structured access to its child list and its own synchronization handle.

---

### 2.5 File: `src/threads/thread.c`
#### Lines 676–681: Initializing Userprog Fields in `init_thread()`
```c
#ifdef USERPROG
  t->pagedir = NULL;
  list_init (&t->children);
  t->child_status = NULL;
  t->exit_status = -1;
#endif
```
- **Why this change was necessary:**
  `init_thread()` initializes both the initial `main` thread and all subsequent threads created via `thread_create()`. Failing to initialize `children` would cause linked list operations to corrupt arbitrary memory. Defaulting `exit_status` to `-1` ensures that unhandled exceptions or failed loads correctly report failure.
- **Runtime Impact:**
  Guarantees consistent, predictable initialization for all processes.

---

### 2.6 File: `src/userprog/process.c`

#### Lines 25–31: Inter-Process Load Communication (`struct exec_info`)
```c
/* Data passed from process_execute to start_process. */
struct exec_info
  {
    char *fn_copy;
    struct child_status *child;
    struct semaphore sema_load;
    bool load_success;
  };
```
- **Why this change was necessary:**
  In base Pintos, `thread_create()` immediately returns the child's `tid` before the child thread has even been scheduled or attempted to load its executable. If the binary file does not exist or memory allocation fails, `process_execute()` had already returned a valid `tid` to the caller, violating the specification where `process_execute()` must return `TID_ERROR` (-1) on load failure.
- **Runtime Impact:**
  `sema_load` provides synchronous barrier coordination: the parent blocks until `start_process()` completes ELF loading and signals success or failure.

#### Lines 40–124: Safe Command-Line Copying & Execution (`process_execute`)
```c
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy;
  char *fn_prog;
  char *save_ptr;
  char *prog_name;
  tid_t tid;
  struct thread *cur = thread_current ();

  /* Make a page copy of FILE_NAME for the child process. */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  /* Make a second page copy to safely extract the program name. */
  fn_prog = palloc_get_page (0);
  if (fn_prog == NULL)
    {
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }
  strlcpy (fn_prog, file_name, PGSIZE);

  prog_name = strtok_r (fn_prog, " ", &save_ptr);
  if (prog_name == NULL)
    {
      palloc_free_page (fn_copy);
      palloc_free_page (fn_prog);
      return TID_ERROR;
    }

  /* Allocate child_status for tracking child lifecycle. */
  struct child_status *child = malloc (sizeof (struct child_status));
  if (child == NULL)
    {
      palloc_free_page (fn_copy);
      palloc_free_page (fn_prog);
      return TID_ERROR;
    }
  child->tid = TID_ERROR;
  child->exit_status = -1;
  child->waited = false;
  child->exited = false;
  child->ref_cnt = 2;
  sema_init (&child->sema, 0);
  lock_init (&child->lock);
  list_push_back (&cur->children, &child->elem);

  struct exec_info info;
  info.fn_copy = fn_copy;
  info.child = child;
  sema_init (&info.sema_load, 0);
  info.load_success = false;

  /* Create a new thread using only the executable program name. */
  tid = thread_create (prog_name, PRI_DEFAULT, start_process, &info);
  palloc_free_page (fn_prog);

  if (tid == TID_ERROR)
    {
      palloc_free_page (fn_copy);
      list_remove (&child->elem);
      free (child);
      return TID_ERROR;
    }

  /* Wait until start_process finishes loading the executable. */
  sema_down (&info.sema_load);

  if (!info.load_success)
    {
      list_remove (&child->elem);
      lock_acquire (&child->lock);
      child->ref_cnt--;
      bool should_free = (child->ref_cnt == 0);
      lock_release (&child->lock);
      if (should_free)
        free (child);
      return TID_ERROR;
    }

  return tid;
}
```
- **Why this change was necessary:**
  1. `thread->name` in Pintos is an array of 16 characters (`char name[16]`). If `file_name` is `"args-multiple some arguments for you!"`, passing `file_name` directly to `thread_create()` would overflow or truncate the name, resulting in corrupted output during `process_exit()` (`printf("%s: exit(%d)\n", cur->name, ...)`).
  2. Allocating two full 4KB pages (`fn_copy` and `fn_prog`) ensures argument strings up to 4KB can be safely manipulated without kernel stack overflow.
  3. All allocated pages and heap memory are strictly freed on both success and failure paths, preventing kernel memory leaks.
- **Runtime Impact:**
  `thread->name` is set to purely the executable name (e.g. `"args-multiple"`), the parent waits on `info.sema_load`, and load failures correctly return `TID_ERROR`.

#### Lines 129–186: Parsing Arguments & Launching Process (`start_process`)
```c
static void
start_process (void *aux)
{
  struct exec_info *info = aux;
  char *file_name = info->fn_copy;
  struct thread *cur = thread_current ();
  struct intr_frame if_;
  bool success;

  cur->child_status = info->child;
  cur->child_status->tid = cur->tid;

  /* Tokenize command line into argv[] and argc. */
  char *argv[128];
  int argc = 0;
  char *token;
  char *save_ptr;

  for (token = strtok_r (file_name, " ", &save_ptr); token != NULL;
       token = strtok_r (NULL, " ", &save_ptr))
    {
      if (argc < 127)
        argv[argc++] = token;
      else
        break;
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

  info->load_success = success;
  palloc_free_page (file_name);
  sema_up (&info->sema_load);

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
- **Why this change was necessary:**
  `strtok_r()` tokenizes `file_name` into `argv[]` pointers in place. Only `argv[0]` (the executable name) is passed to `load()`, while the full `argv` array and `argc` count are forwarded to `setup_stack()`.
- **Runtime Impact:**
  `palloc_free_page(file_name)` releases the 4KB command-line buffer after the arguments are safely copied onto the user stack, and `sema_up(&info->sema_load)` unblocks the waiting parent.

#### Lines 195–228: Child Synchronization (`process_wait`)
```c
int
process_wait (tid_t child_tid) 
{
  struct thread *cur = thread_current ();
  struct list_elem *e;
  struct child_status *child = NULL;

  for (e = list_begin (&cur->children); e != list_end (&cur->children); e = list_next (e))
    {
      struct child_status *cs = list_entry (e, struct child_status, elem);
      if (cs->tid == child_tid)
        {
          child = cs;
          break;
        }
    }

  if (child == NULL || child->waited)
    return -1;

  child->waited = true;
  sema_down (&child->sema);

  int exit_status = child->exit_status;
  list_remove (&child->elem);

  lock_acquire (&child->lock);
  child->ref_cnt--;
  bool should_free = (child->ref_cnt == 0);
  lock_release (&child->lock);
  if (should_free)
    free (child);

  return exit_status;
}
```
- **Why this change was necessary:**
  In base Pintos, `process_wait()` immediately returned `-1`, causing the kernel to shut down before user programs ran. Furthermore, Pintos specification dictates that waiting on a non-child, an invalid TID, or waiting twice on the same child must return `-1`.
- **Runtime Impact:**
  The parent locates the matching child, sets `child->waited = true`, and blocks on `child->sema`. Once unblocked, it removes the child from `children` and decrements `ref_cnt`. If called a second time, the child is no longer in `children`, correctly returning `-1`.

#### Lines 231–286: Process Exit & Cleanup (`process_exit`)
```c
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

  /* Print exit status only for user processes. */
  if (cur->pagedir != NULL)
    printf ("%s: exit(%d)\n", cur->name, cur->exit_status);

  /* Notify parent via child_status if present. */
  if (cur->child_status != NULL)
    {
      struct child_status *cs = cur->child_status;
      lock_acquire (&cs->lock);
      cs->exit_status = cur->exit_status;
      cs->exited = true;
      sema_up (&cs->sema);
      cs->ref_cnt--;
      bool should_free = (cs->ref_cnt == 0);
      lock_release (&cs->lock);
      if (should_free)
        free (cs);
      cur->child_status = NULL;
    }

  /* Clean up children list. */
  while (!list_empty (&cur->children))
    {
      struct list_elem *e = list_pop_front (&cur->children);
      struct child_status *cs = list_entry (e, struct child_status, elem);
      lock_acquire (&cs->lock);
      cs->ref_cnt--;
      bool should_free = (cs->ref_cnt == 0);
      lock_release (&cs->lock);
      if (should_free)
        free (cs);
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
- **Why this change was necessary:**
  1. The automated test suite requires every user process to print `"<name>: exit(<status>)\n"`. Kernel threads (`main`, `idle`) have `cur->pagedir == NULL` and must NOT print this message.
  2. The terminating process records its exit code into `cur->child_status`, marks `exited = true`, and wakes up any waiting parent with `sema_up()`.
  3. Orphan handling: If the parent exits before its children, `process_exit()` iterates over `cur->children`, decrementing their `ref_cnt`. Whichever process terminates last frees the allocated `child_status`.
- **Runtime Impact:**
  Zero memory leaks, clean page directory reclamation, and reliable inter-process notification.

#### Lines 620–661: Constructing the User Stack (`setup_stack`)
```c
static bool
setup_stack (void **esp, int argc, char **argv) 
{
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL) 
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        *esp = PHYS_BASE;
      else
        {
          palloc_free_page (kpage);
          return false;
        }
    }
  else
    return false;

  /* Push argument strings in reverse order (argv[argc - 1] down to argv[0]). */
  uintptr_t arg_addrs[128];
  int i;
  for (i = argc - 1; i >= 0; i--)
    {
      size_t len = strlen (argv[i]) + 1;
      *esp = (char *) *esp - len;
      memcpy (*esp, argv[i], len);
      arg_addrs[i] = (uintptr_t) *esp;
    }

  /* Word-align *esp downward to a multiple of 4 bytes. */
  *esp = (void *) ((uintptr_t) *esp & ~3);

  /* Push null pointer sentinel argv[argc]. */
  *esp = (char **) *esp - 1;
  *(char **) *esp = NULL;

  /* Push saved user addresses of argv[argc - 1] down to argv[0]. */
  for (i = argc - 1; i >= 0; i--)
    {
      *esp = (char **) *esp - 1;
      *(char **) *esp = (char *) arg_addrs[i];
    }

  /* Push argv (the user stack address of argv[0], type char **). */
  char **argv_user = (char **) *esp;
  *esp = (char ***) *esp - 1;
  *(char ***) *esp = argv_user;

  /* Push argc (type int). */
  *esp = (int *) *esp - 1;
  *(int *) *esp = argc;

  /* Push fake return address (void * = 0). */
  *esp = (void **) *esp - 1;
  *(void **) *esp = NULL;

  if ((uint8_t *) *esp < ((uint8_t *) PHYS_BASE - PGSIZE))
    return false;

  return true;
}
```
- **Why this change was necessary:**
  When user entry code `_start` (in `src/lib/user/entry.c`) begins executing:
  ```c
  void _start (int argc, char *argv[]) { exit (main (argc, argv)); }
  ```
  The x86 architecture expects the stack to be set up as if a `call` instruction had just executed. Thus:
  - `*esp` must point to the return address (`0`).
  - `*(esp + 1)` must be `argc`.
  - `*(esp + 2)` must be `argv` (address of `argv[0]`).
  - Word alignment (`*esp & ~3`) is strictly required so that 32-bit memory reads do not cross word boundaries or violate hardware conventions.
- **Runtime Impact:**
  `main(argc, argv)` in user programs receives valid, beautifully formatted command-line parameters.

---

### 2.7 File: `src/userprog/syscall.c`

#### Lines 13–89: Safe User-Memory Access Helpers
```c
static void
exit_with_error (void)
{
  struct thread *cur = thread_current ();
  cur->exit_status = -1;
  thread_exit ();
}

static bool
is_valid_user_ptr (const void *vaddr)
{
  if (vaddr == NULL || !is_user_vaddr (vaddr))
    return false;
  return pagedir_get_page (thread_current ()->pagedir, vaddr) != NULL;
}

static void
check_user_ptr (const void *vaddr)
{
  if (!is_valid_user_ptr (vaddr))
    exit_with_error ();
}

static void
check_user_buffer (const void *buffer, size_t size)
{
  if (size == 0)
    return;
  if (buffer == NULL)
    exit_with_error ();

  const uint8_t *start = (const uint8_t *) buffer;
  const uint8_t *end = start + size - 1;

  if (end < start)
    exit_with_error ();

  if (!is_user_vaddr (start) || !is_user_vaddr (end))
    exit_with_error ();

  const uint8_t *page = (const uint8_t *) pg_round_down (start);
  while (page <= end)
    {
      if (pagedir_get_page (thread_current ()->pagedir, page) == NULL)
        exit_with_error ();
      page += PGSIZE;
    }
}

static void
check_user_string (const char *str)
{
  if (str == NULL)
    exit_with_error ();

  const char *p = str;
  while (true)
    {
      check_user_ptr (p);
      if (*p == '\0')
        break;
      p++;
    }
}
```
- **Why this change was necessary:**
  A malicious or buggy user program could pass a null pointer, a pointer into kernel virtual memory (`>= PHYS_BASE`), an address outside mapped page tables, or a buffer spanning multiple unmapped virtual pages. If the kernel directly dereferenced such addresses, a kernel page fault would panic the entire operating system.
- **Runtime Impact:**
  Every user pointer or buffer is validated against `is_user_vaddr()` and verified against `thread_current()->pagedir`. Any invalid address causes the offending user process to terminate with exit code `-1`, protecting kernel stability.

#### Lines 91–152: `syscall_handler` Implementation
```c
static void
syscall_handler (struct intr_frame *f) 
{
  check_user_buffer (f->esp, sizeof (int));
  int syscall_num = *(int *) f->esp;

  switch (syscall_num)
    {
    case SYS_HALT:
      shutdown_power_off ();
      break;

    case SYS_EXIT:
      check_user_buffer (f->esp, sizeof (int) * 2);
      {
        int status = *((int *) f->esp + 1);
        struct thread *cur = thread_current ();
        cur->exit_status = status;
        thread_exit ();
      }
      break;

    case SYS_EXEC:
      check_user_buffer (f->esp, sizeof (int) * 2);
      {
        const char *cmd_line = (const char *) *((uintptr_t *) f->esp + 1);
        check_user_string (cmd_line);
        f->eax = process_execute (cmd_line);
      }
      break;

    case SYS_WAIT:
      check_user_buffer (f->esp, sizeof (int) * 2);
      {
        tid_t child_tid = *((int *) f->esp + 1);
        f->eax = process_wait (child_tid);
      }
      break;

    case SYS_WRITE:
      check_user_buffer (f->esp, sizeof (int) * 4);
      {
        int fd = *((int *) f->esp + 1);
        const void *buffer = (const void *) *((uintptr_t *) f->esp + 2);
        unsigned size = *((unsigned *) f->esp + 3);
        check_user_buffer (buffer, size);
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

    default:
      exit_with_error ();
      break;
    }
}
```
- **Why this change was necessary:**
  Handles system calls invoked via software interrupt `int $0x30`.
- **Runtime Impact:**
  - `SYS_HALT`: Shuts down the machine.
  - `SYS_EXIT`: Terminates the calling process with the given status code.
  - `SYS_EXEC`: Invokes `process_execute()` and returns the new `tid` (or `-1`).
  - `SYS_WAIT`: Invokes `process_wait()` and returns the child's exit status.
  - `SYS_WRITE`: For `fd == 1` (`STDOUT_FILENO`), outputs the buffer to the console via `putbuf()` and returns the byte count in `f->eax`.

---

### 2.8 File: `src/userprog/exception.c`

#### Lines 86–90: Clean User-Exception Termination in `kill()`
```c
    case SEL_UCSEG:
      /* User's code segment, so it's a user exception, as we
         expected.  Kill the user process.  */
      thread_current ()->exit_status = -1;
      thread_exit (); 
```
#### Lines 149–153: Handling User Page Faults in `page_fault()`
```c
  if (user)
    {
      thread_current ()->exit_status = -1;
      thread_exit ();
    }
```
- **Why this change was necessary:**
  In base Pintos, if a user process caused a page fault or CPU exception, `exception.c` printed diagnostic messages (`"Page fault at..."`, `"dying due to interrupt..."`) and dumped the entire interrupt frame. Automated test checkers expect only `<process_name>: exit(-1)` and fail if debug dumps are printed.
- **Runtime Impact:**
  User-level exceptions terminate the user process gracefully with exit status `-1` without printing kernel error logs.

---

## 3. Operating System Concepts & Deep-Dive Study Guide

### 3.1 User Space vs. Kernel Space Isolation & Hardware Protection
Modern microprocessors (such as the Intel x86) enforce protection through privilege levels known as **Rings**:
- **Ring 0 (Supervisor Mode):** The OS kernel runs in Ring 0 with unrestricted access to all CPU instructions (such as `lgdt`, `lidt`, `cli`, `sti`, modifying `%cr3`), hardware I/O ports, and all physical memory.
- **Ring 3 (User Mode):** User applications run in Ring 3. Privileged instructions cannot be executed, direct port I/O is disabled, and memory accesses are strictly mediated by the page table hardware.

In Pintos, virtual memory is partitioned at `PHYS_BASE = 0xc0000000` (3 GB):
```
+----------------------------------+ 0xFFFFFFFF
|                                  |
|       Kernel Virtual Memory      |   Accessible ONLY in Ring 0
|       (Mapped 1:1 to Physical)   |
|                                  |
+----------------------------------+ 0xC0000000 (PHYS_BASE)
|       User Stack (Grows Down)    |
|                 |                |
|                 v                |
|                                  |   Accessible in Ring 3 and Ring 0
|                 ^                |
|                 |                |
|       User Data / BSS / Code     |
+----------------------------------+ 0x08048000
|       Unmapped (Page 0 Trap)     |
+----------------------------------+ 0x00000000
```
When user code attempts to read or write an address $\ge \text{PHYS\_BASE}$, the CPU Memory Management Unit (MMU) raises a **Page Fault Exception (Interrupt 14)** because the page table entry's User/Supervisor bit (`PTE_U`) is set to 0 (Supervisor only).

---

### 3.2 The x86 32-bit Calling Convention (`cdecl`) & Stack Architecture
In the 32-bit x86 architecture, the stack grows downwards towards lower memory addresses. The stack pointer register (`%esp`) always points to the last item pushed onto the stack.

When a function call `foo(arg0, arg1, arg2)` is performed:
1. Arguments are pushed in **reverse order** (right-to-left): `arg2`, then `arg1`, then `arg0`.
2. The `call` instruction pushes the 4-byte return address (`%eip`) onto the stack.
3. Inside the callee function, the stack layout relative to `%esp` is:
   - `*(esp + 0)`: Return address
   - `*(esp + 4)`: First argument (`arg0`)
   - `*(esp + 8)`: Second argument (`arg1`)
   - `*(esp + 12)`: Third argument (`arg2`)

#### Stack Layout Constructed in Phase 1
Suppose a program is launched with command line:
`args-multiple some arguments for you!`
- `argc = 5`
- `argv[0] = "args-multiple"`
- `argv[1] = "some"`
- `argv[2] = "arguments"`
- `argv[3] = "for"`
- `argv[4] = "you!"`

The stack constructed by `setup_stack()` starting at `0xc0000000` is formatted as follows:

```
Address        Name               Data                       Type
-----------------------------------------------------------------------------
0xbffffffb     argv[4] string     "you!\0"                   char[5]
0xbffffff7     argv[3] string     "for\0"                    char[4]
0xbfffffee     argv[2] string     "arguments\0"              char[10]
0xbfffffe9     argv[1] string     "some\0"                   char[5]
0xbfffffdc     argv[0] string     "args-multiple\0"          char[14]
-----------------------------------------------------------------------------
0xbfffffd8     word-alignment     0x00, 0x00, 0x00, 0x00     uint8_t[4] (pad)
-----------------------------------------------------------------------------
0xbfffffd4     argv[5] sentinel   0x00000000                 char * (NULL)
0xbfffffd0     argv[4] pointer    0xbffffffb                 char *
0xbfffffcb     argv[3] pointer    0xbffffff7                 char *
0xbfffffc4     argv[2] pointer    0xbfffffee                 char *
0xbfffffc0     argv[1] pointer    0xbfffffe9                 char *
0xbfffffbc     argv[0] pointer    0xbfffffdc                 char *
-----------------------------------------------------------------------------
0xbfffffb8     argv               0xbfffffbc                 char **
0xbfffffb4     argc               5                          int
0xbfffffb0     return address     0x00000000                 void (*) ()
-----------------------------------------------------------------------------
```
When user entry point `_start` executes, `%esp` points to `0xbfffffb0`. The C startup code reads:
- `argc = *(int *)(esp + 4) = 5`
- `argv = *(char ***)(esp + 8) = 0xbfffffbc`
- `argv[0] = *(char **)(0xbfffffbc) = 0xbfffffdc` -> `"args-multiple"`
- `argv[5] = *(char **)(0xbfffffbc + 20) = NULL`

Everything aligns and satisfies both POSIX and Pintos conventions.

---

### 3.3 Hardware Interrupts, Trap Gates & `struct intr_frame`
When a user process executes `int $0x30`, the hardware performs the following operations atomically:
1. Reads Gate Descriptor `0x30` from the Interrupt Descriptor Table (IDT).
2. Verifies that the descriptor Privilege Level (DPL == 3) allows User Mode execution.
3. Switches privilege level from Ring 3 to Ring 0.
4. Switches stack from the User Stack to the Kernel Stack (defined in the Task State Segment, `tss->esp0`).
5. Pushes user `%ss`, user `%esp`, `%eflags`, `%cs`, and `%eip` onto the kernel stack.
6. Jumps to the interrupt stub in `intr-stubs.S`, which pushes the remaining registers (`%ds`, `%es`, `%fs`, `%gs`, general-purpose registers via `pushal`).
7. Constructs `struct intr_frame` on the kernel stack and calls `syscall_handler(&intr_frame)`.

```
                    User Mode Execution
                            |
                     int $0x30 instruction
                            |
+---------------------------v---------------------------+
|               CPU Hardware Transition                 |
|  1. Switch to Ring 0                                  |
|  2. Load Kernel %esp from TSS (tss->esp0)             |
|  3. Push User %ss, %esp, %eflags, %cs, %eip           |
+---------------------------+---------------------------+
                            |
+---------------------------v---------------------------+
|          Kernel Low-Level Stub (intr-stubs.S)         |
|  1. Push Segment Registers (%ds, %es, %fs, %gs)       |
|  2. Push General Purpose Registers (pushal)           |
|  3. Call intr_handler() -> syscall_handler()          |
+---------------------------+---------------------------+
                            |
+---------------------------v---------------------------+
|             Pintos Syscall Handler (syscall.c)        |
|  1. Validate User Pointer: check_user_buffer(f->esp)  |
|  2. Dispatch syscall_num (SYS_WRITE, SYS_EXIT, ...)   |
|  3. Write return value into f->eax                    |
+---------------------------+---------------------------+
                            |
+---------------------------v---------------------------+
|               Return Path (intr_exit)                 |
|  1. Pop General Purpose & Segment Registers           |
|  2. Execute iret instruction                          |
|  3. Hardware restores User %cs, %eip, %ss, %esp       |
+---------------------------+---------------------------+
                            |
                    User Mode Resumes
```

---

### 3.4 Process Lifecycle, Race Conditions, and Memory Management

A critical challenge in OS development is managing asynchronous parent-child lifecycles. There are four distinct lifecycle permutations:

```
Permutation 1: Child terminates before Parent waits
Child:  executes ---------> process_exit() [sema_up] -> dead
Parent:                     ... other work ... -------> process_wait() [retrieves status immediately]

Permutation 2: Parent waits before Child terminates
Parent: process_wait() [sema_down: BLOCKED] ------------------------> unblocked [retrieves status]
Child:  executes ----------------------------> process_exit() [sema_up] -> dead

Permutation 3: Parent exits before Child terminates (Orphan Child)
Parent: process_exit() [decrements ref_cnt to 1] -> dead
Child:  continues executing --------------------> process_exit() [decrements ref_cnt to 0: FREES child_status]

Permutation 4: Child is never waited on by Parent
Child:  process_exit() [decrements ref_cnt to 1] -> dead
Parent: continues executing --------------------> process_exit() [decrements ref_cnt to 0: FREES child_status]
```

#### Why Reference Counting (`ref_cnt`) is Essential:
If the parent allocated `child_status` and freed it on `wait()`, but the parent died without waiting, `child_status` would leak. Conversely, if the parent freed all child records upon exiting while children were still running, children would later dereference freed memory when exiting (`use-after-free`). 

With a two-party reference count (`ref_cnt = 2`):
- Both parent and child hold one reference.
- Whichever entity finishes its lifecycle decrements `ref_cnt` under `child->lock`.
- Whichever entity decrements `ref_cnt` to 0 performs `free(child)`.
- Memory leaks and dangling pointers are guaranteed to be eliminated.

---

### 3.5 Common Pitfalls & Debugging Techniques

1. **Stack Misalignment:**
   - *Symptom:* Subtle crashes or assertion failures in user library functions.
   - *Cause:* x86 instructions such as `movaps` or functions expecting word-aligned variables encounter unaligned stack addresses when `*esp` is not rounded down to a multiple of 4 before pushing pointers.
   - *Fix:* Always apply `*esp = (void *)((uintptr_t)*esp & ~3)`.

2. **Dangling String Pointers on Stack:**
   - *Symptom:* User program prints garbage for arguments (`argv[i] = '??'`).
   - *Cause:* Pushing pointer addresses into `argv` pointing to the kernel buffer `fn_copy` before it is freed by `palloc_free_page()`.
   - *Fix:* Copy strings onto the user stack first, record their *user virtual addresses* (`arg_addrs[i] = (uintptr_t)*esp`), and push those user addresses into the `argv` pointer array.

3. **Buffer Overflows on `thread->name`:**
   - *Symptom:* Kernel memory corruption, corrupted scheduling lists, or mangled process exit messages.
   - *Cause:* Passing the full argument string (e.g. `"args-many a b c d..."`) to `thread_create()`, which only allocates a 16-byte buffer for `name`.
   - *Fix:* Parse the first token (`prog_name`) into an isolated buffer and pass only the program name to `thread_create()`.

4. **Linker Segment Collisions (`-z separate-code`):**
   - *Symptom:* `load_segment()` fails with `already mapped` at `0x08048000`.
   - *Cause:* Modern binutils generates separate program headers for read-only headers and executable code that share the same page.
   - *Fix:* Add `-Wl,-z,noseparate-code` to `LDFLAGS` in `Makefile.userprog`.
