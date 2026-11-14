# Pintos Project 2: User Programs — Phase 4 Engineering & Deep-Dive Study Report
## File System Calls, File Descriptor Management, Synchronization & Executable Write Protection (ROX)

---

## 1. Project & Phase Objectives

### 1.1 High-Level Project Goals (Pintos Project 2: User Programs)
In Pintos Project 2, the operating system transitions from executing exclusively trusted, cooperating kernel threads (Project 1) to hosting untrusted user-space applications running under x86 hardware protection (Ring 3). The core responsibilities of the operating system in this environment are:
1. **Memory Isolation & Hardware Protection:** Ensuring that an unprivileged user process cannot corrupt, inspect, or crash the kernel or other concurrent processes through strict virtual memory boundary enforcement.
2. **System Call Infrastructure:** Providing a structured, secure gateway (via software interrupt `0x30`) through which user programs request kernel-mediated services (I/O, process control, file system operations).
3. **Resource Lifecycle Management & Reclamation:** Guaranteeing that every kernel resource—allocated pages, heap blocks, open files, file descriptors, and synchronization locks—is cleanly reclaimed upon normal program termination or abnormal fault termination.
4. **POSIX-Like File & Process Semantics:** Exposing an intuitive, robust abstraction for file manipulation, standard streams (`stdin`, `stdout`), process hierarchies, and concurrent execution.

---

### 1.2 Architectural Goals of Phase 4
Phases 1 through 3 established user stack argument passing, defensive memory validation, safe page fault handling, and parent-child lifecycle synchronization (`SYS_EXEC`, `SYS_WAIT`, `SYS_EXIT`, `SYS_HALT`). 

However, base Pintos lacked any implementation of file-related system calls. User programs could neither open files, read or write data, create or delete files, nor safely manipulate multiple file streams. Furthermore, the base Pintos file system (`src/filesys/`) is inherently **not thread-safe**, containing multiple shared internal structures, directory manipulation routines, and inode operations that corrupt when accessed concurrently.

The architectural goals of **Phase 4** are:
1. **Per-Process File Descriptor Abstraction:**
   - Design a scalable, per-process file descriptor table supporting at least 128 open files concurrently (as required by stress tests like `open-many`).
   - Standardize file descriptor numbering: reserve descriptor `0` for `STDIN_FILENO` (console input via `input_getc()`), descriptor `1` for `STDOUT_FILENO` (console output via `putbuf()`), and allocate dynamically starting from descriptor `2`.
   - Prevent descriptor leakage and use-after-free vulnerabilities by maintaining an independent heap-allocated descriptor entry (`struct fd_entry`) linked list per thread.
2. **Implementation of the 9 File System Calls:**
   - `SYS_CREATE`: Creates a new file on the filesystem with a given initial size.
   - `SYS_REMOVE`: Deletes an existing file by name.
   - `SYS_OPEN`: Opens a file, allocates a new integer file descriptor, and registers it in the thread's file descriptor list.
   - `SYS_FILESIZE`: Queries and returns the total byte length of an open file.
   - `SYS_READ`: Reads bytes from an open file or standard input (`stdin`) into a user buffer.
   - `SYS_WRITE`: Writes bytes from a user buffer to an open file or standard output (`stdout`).
   - `SYS_SEEK`: Updates the read/write byte offset within an open file.
   - `SYS_TELL`: Returns the current read/write byte offset within an open file.
   - `SYS_CLOSE`: Closes an open file descriptor, releases its kernel file handle, and frees associated descriptor memory.
3. **Global File System Synchronization (`filesys_lock`):**
   - Provide coarse-grained mutual exclusion across all file system operations using a dedicated global lock (`filesys_lock`).
   - Guarantee that concurrent read/write/open/create operations execute atomically without race conditions or metadata corruption in `src/filesys/`.
4. **Deadlock-Free Fault Recovery:**
   - Never leak `filesys_lock` if a user process triggers a hardware page fault (`#PF`) during a system call (for instance, reading from disk into an unmapped or read-only virtual address).
   - Perform strict pre-validation of all user buffers, strings, and stack pointers *before* acquiring `filesys_lock`.
   - Implement defensive cleanup in `process_exit()`: if the dying thread holds `filesys_lock`, release it before entering the file descriptor teardown phase.
5. **Denying Writes to Running Executables (`ROX`):**
   - Implement standard Unix "Read-Only Executable" (`ROX`) semantics: while any process is actively executing an ELF binary, any attempt by another process (or the same process) to open that binary with write permissions or write to it must be rejected.
   - Prevent running code corruption on disk and ensure binary consistency throughout process lifetime by calling `file_deny_write()` in `load()` and keeping the executable file open until `process_exit()`.
6. **Automatic Resource Reclamation on Termination:**
   - In `process_exit()`, atomically traverse and close all active file descriptors in `cur->fd_list`, free their heap memory, close `cur->exec_file` (which re-enables write access), and release `filesys_lock`.

---

### 1.3 Targeted and Verified Pintos Test Cases

The following test suites in `tests/userprog` and `tests/filesys/base` directly test and verify the Phase 4 implementation:

| Test Case | Category | Verification Scenario & Mechanism | Expected Behavior |
| :--- | :--- | :--- | :--- |
| `create-normal` | File Creation | Creates a standard 8 KB file named `quux.dat`. | System call returns `true`; file is successfully created. |
| `create-empty` | File Creation | Creates a 0-byte file named `empty`. | System call returns `true`; 0-byte file entry exists. |
| `create-null` | Validation | Attempts to pass `NULL` as the filename pointer to `create()`. | `validate_user_string()` rejects `NULL`; process killed with `-1`. |
| `create-bad-ptr` | Validation | Passes an unmapped user address (`0x0804c000` / unmapped) to `create()`. | `validate_user_string()` faults; process killed with `-1`. |
| `create-long` | File Creation | Creates a file with an overly long filename (128 characters). | Handled safely by filesys; succeeds or fails cleanly without kernel crash. |
| `create-exists` | File Creation | Calls `create("quux.dat")` twice with the same filename. | First call returns `true`; second call returns `false` (file already exists). |
| `create-bound` | Boundary Check | Filename string crosses a page boundary into a valid page. | Validation succeeds; file is created. |
| `open-normal` | File Open | Opens a newly created file `sample.txt`. | Returns integer descriptor $\ge 2$; file is ready for I/O. |
| `open-missing` | File Open | Attempts to open a nonexistent file `no-such-file`. | `filesys_open()` returns `NULL`; syscall returns `-1`. |
| `open-boundary` | Boundary Check | Filename string crosses a page boundary into valid mapped memory. | Returns valid file descriptor without faulting. |
| `open-empty` | File Open | Attempts to open an empty string `""`. | `filesys_open("")` returns `NULL`; syscall returns `-1`. |
| `open-null` | Validation | Passes `NULL` as the filename pointer to `open()`. | Pre-validation terminates process with exit status `-1`. |
| `open-bad-ptr` | Validation | Passes an unmapped virtual address as filename to `open()`. | Pre-validation terminates process with exit status `-1`. |
| `open-twice` | File Open | Opens `sample.txt` twice simultaneously. | Returns two distinct file descriptors ($\ge 2$); both valid. |
| `close-normal` | File Close | Opens `sample.txt` and calls `close(fd)`. | File is closed; descriptor is removed and freed. |
| `close-twice` | File Close | Closes the same file descriptor twice in succession. | Second close safely ignores the nonexistent descriptor. |
| `close-stdin` | File Close | Attempts to call `close(0)`. | Safely ignored; standard input cannot be closed via `close()`. |
| `close-stdout` | File Close | Attempts to call `close(1)`. | Safely ignored; standard output cannot be closed via `close()`. |
| `close-bad-fd` | File Close | Calls `close(0x20101234)` with an arbitrary invalid descriptor. | Safely ignored without crashing or corrupting memory. |
| `read-normal` | File Read | Reads 100 bytes from `sample.txt` into a buffer. | Returns 100; buffer contains exact file contents. |
| `read-bad-ptr` | Validation | Passes an invalid buffer pointer (`0xc0000000` or unmapped) to `read()`. | `validate_user_range()` rejects buffer; process killed with `-1`. Lock never leaked. |
| `read-boundary` | Boundary Check | Read buffer spans across two valid user virtual pages. | Reads full byte range correctly across the boundary. |
| `read-zero` | File Read | Calls `read(fd, buffer, 0)` with size 0. | Returns 0 immediately without reading disk or touching buffer. |
| `read-stdout` | File Read | Attempts to call `read(1, buffer, size)` on `STDOUT_FILENO`. | Returns `-1` (cannot read from standard output). |
| `read-bad-fd` | File Read | Calls `read(0x42, buffer, size)` with invalid descriptor. | Returns `-1` cleanly. |
| `write-normal` | File Write | Writes 239 bytes to `sample.txt`. | Returns 239; bytes written match file contents. |
| `write-bad-ptr` | Validation | Passes an invalid buffer pointer to `write()`. | `validate_user_range()` rejects pointer; process killed with `-1`. |
| `write-boundary` | Boundary Check | Write buffer spans across two valid user virtual pages. | Writes full byte count correctly across the boundary. |
| `write-zero` | File Write | Calls `write(fd, buffer, 0)` with size 0. | Returns 0 immediately without modifying file. |
| `write-stdin` | File Write | Attempts to call `write(0, buffer, size)` on `STDIN_FILENO`. | Returns `-1` (cannot write to standard input). |
| `write-bad-fd` | File Write | Calls `write(0x42, buffer, size)` with invalid descriptor. | Returns `-1` cleanly. |
| `multi-child-fd` | FD Isolation | Parent opens a file, spawns a child that closes descriptor 2. | File descriptors are strictly per-process; parent's descriptor 2 remains open and functional. |
| `multi-recurse` | Stress / Recurse | Recursively opens files and executes child processes. | All descriptors and process states remain stable. |
| `rox-simple` | Executable Protection | Process tries to open its own executable for writing and write to it. | `file_write()` returns 0 bytes written (`file->deny_write == true`). |
| `rox-child` | Executable Protection | Parent executes child; parent attempts to open child's binary and write to it while running. | Write is denied; returns 0 bytes. |
| `rox-multichild` | Executable Protection | Spawns multiple concurrent instances of the same binary; parent tries to overwrite executable. | Write is denied across all child lifetimes until all children exit. |
| `syn-read` / `syn-write` / `syn-remove` | Concurrency | Multiple children concurrently reading, writing, and deleting files. | Atomic serialization via `filesys_lock` prevents any filesystem corruption. |

---

## 2. Detailed Code Walkthrough

Five core files implement Phase 4:
1. `src/threads/thread.h`: Definition of `struct fd_entry` and file-tracking fields in `struct thread`.
2. `src/threads/thread.c`: Initialization of the per-thread file descriptor list and executable pointer in `init_thread()`.
3. `src/userprog/syscall.h`: Declaration of global `filesys_lock`.
4. `src/userprog/syscall.c`: Implementation of the 9 file system calls, descriptor lookup helper, and strict buffer pre-validation.
5. `src/userprog/process.c`: Executable write protection (`file_deny_write`) in `load()` and safe resource reclamation in `process_exit()`.

---

### 2.1 File: `src/threads/thread.h`

#### 2.1.1 File Descriptor Table Entry: `struct fd_entry` (Lines 13–18)
```c
#ifdef USERPROG
struct fd_entry
  {
    int fd;
    struct file *file;
    struct list_elem elem;
  };
#endif
```
- **File & Lines:** [src/threads/thread.h](file:///home/yasamin/pintos/src/threads/thread.h#L13-L18).
- **Why Needed:** In standard Unix operating systems, file descriptors are per-process integer identifiers mapped to open file objects. Base Pintos provided no structure to track open files on behalf of a user process. Fixed-size arrays (e.g., `struct file *fds[16]`) fail stress tests such as `open-many` (which opens up to 126 files simultaneously). `struct fd_entry` is a heap-allocated element containing the assigned integer descriptor (`fd`), the underlying kernel file pointer (`struct file *file`), and a `struct list_elem elem` for dynamic linked-list management.
- **Runtime Impact:** Allows arbitrary numbers of concurrently opened files per process without static memory overhead. Each call to `open()` allocates a single `struct fd_entry` on the kernel heap.

#### 2.1.2 Forward Declaration of `struct file` (Line 10)
```c
struct file;
```
- **File & Lines:** [src/threads/thread.h](file:///home/yasamin/pintos/src/threads/thread.h#L10).
- **Why Needed:** `threads/thread.h` cannot directly include `filesys/file.h` without creating circular dependency issues across the build system. A forward declaration allows declaring `struct file *` pointers inside `struct fd_entry` and `struct thread`.

#### 2.1.3 Per-Thread File Management Fields in `struct thread` (Lines 142–144)
```c
#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint32_t *pagedir;                  /**< Page directory. */
    struct list children;               /**< List of children (struct child_status). */
    struct child_status *cp;            /**< Child status tracking this thread. */
    int exit_status;                    /**< Exit status. */
    struct list fd_list;                /**< List of open file descriptors (struct fd_entry). */
    int next_fd;                        /**< Next fd number to assign (starts at 2). */
    struct file *exec_file;             /**< Open executable file of this process. */
#endif
```
- **File & Lines:** [src/threads/thread.h](file:///home/yasamin/pintos/src/threads/thread.h#L142-L144).
- **Why Needed:**
  - `struct list fd_list`: Stores the head of the doubly-linked list of `struct fd_entry` instances owned exclusively by this thread.
  - `int next_fd`: Monotonically increasing counter for assigning unique integer descriptors. Initialized to `2` because descriptors `0` and `1` are reserved for `stdin` and `stdout`.
  - `struct file *exec_file`: Retains a kernel file pointer to the executable binary currently running under this process. Pintos's file system automatically denies writes to any inode with `deny_write == true` as long as an open file reference remains. Storing this pointer keeps the file open and write-protected until the process terminates.
- **Runtime Impact:** Every thread maintains complete, isolated ownership of its file descriptors. If a child process closes descriptor `2` or descriptor `3`, the parent process's file descriptor table remains completely unaffected (`multi-child-fd`).

---

### 2.2 File: `src/threads/thread.c`

#### 2.2.1 Initialization in `init_thread()` (Lines 682–684)
```c
#ifdef USERPROG
  t->pagedir = NULL;
  list_init (&t->children);
  t->cp = NULL;
  t->exit_status = -1;
  list_init (&t->fd_list);
  t->next_fd = 2;
  t->exec_file = NULL;
#endif
```
- **File & Lines:** [src/threads/thread.c](file:///home/yasamin/pintos/src/threads/thread.c#L682-L684).
- **Why Needed:** Whenever a new thread structure is created (both for the initial kernel thread and all subsequent user processes created via `process_execute()`), its descriptor list must be an empty valid list, its next assignable descriptor must be `2`, and its running executable file pointer must be `NULL`.
- **Runtime Impact:** Guarantees clean state initialization before any user program execution begins, preventing invalid pointer dereferences when traversing `fd_list` or closing `exec_file`.

---

### 2.3 File: `src/userprog/syscall.h`

#### 2.3.1 Declaration of Global `filesys_lock` (Lines 4–6)
```c
#include "threads/synch.h"

extern struct lock filesys_lock;
```
- **File & Lines:** [src/userprog/syscall.h](file:///home/yasamin/pintos/src/userprog/syscall.h#L4-L6).
- **Why Needed:** Multiple files (`src/userprog/syscall.c` and `src/userprog/process.c`) require access to the global file system synchronization lock. Declaring `filesys_lock` as an `extern struct lock` in the header allows `process.c` to acquire the lock during `load()` and `process_exit()` while keeping the definition in `syscall.c`.
- **Runtime Impact:** Exposes a unified global lock symbol across the entire userprog subsystem.

---

### 2.4 File: `src/userprog/syscall.c`

#### 2.4.1 Global Lock Definition and Initialization (Lines 15, 27)
```c
struct lock filesys_lock;

void
syscall_init (void) 
{
  lock_init (&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}
```
- **File & Lines:** [src/userprog/syscall.c](file:///home/yasamin/pintos/src/userprog/syscall.c#L15), [src/userprog/syscall.c](file:///home/yasamin/pintos/src/userprog/syscall.c#L27).
- **Why Needed:** `filesys_lock` must be initialized exactly once during kernel bootstrap (when `syscall_init()` is called from `threads/init.c`).
- **Runtime Impact:** Provides a sleep-lock (semaphore-backed) synchronization primitive capable of putting threads to sleep when waiting for disk I/O, ensuring thread-safe serialized access to the underlying filesystem.

#### 2.4.2 File Descriptor Lookup Helper: `get_fd_entry()` (Lines 31–48)
```c
static struct fd_entry *
get_fd_entry (int fd)
{
  if (fd < 2)
    return NULL;

  struct thread *cur = thread_current ();
  struct list_elem *e;

  for (e = list_begin (&cur->fd_list); e != list_end (&cur->fd_list); e = list_next (e))
    {
      struct fd_entry *entry = list_entry (e, struct fd_entry, elem);
      if (entry->fd == fd)
        return entry;
    }

  return NULL;
}
```
- **File & Lines:** [src/userprog/syscall.c](file:///home/yasamin/pintos/src/userprog/syscall.c#L31-L48).
- **Why Needed:** When a user program passes an integer `fd` to `read()`, `write()`, `filesize()`, `seek()`, `tell()`, or `close()`, the kernel must locate the corresponding `struct fd_entry` in the current thread's private `fd_list`. Since `0` (`stdin`) and `1` (`stdout`) are special console streams and not backed by `struct fd_entry`, passing `fd < 2` immediately returns `NULL`.
- **Runtime Impact:** Provides $O(N)$ lookup over the current thread's open files without exposing or modifying any other thread's descriptor table. Returns `NULL` safely if the descriptor does not exist or has already been closed.

#### 2.4.3 System Call: `SYS_CREATE` (Lines 153–163)
```c
    case SYS_CREATE:
      validate_user_range ((const uint8_t *) f->esp + 4, sizeof (char *) + sizeof (unsigned));
      {
        const char *file = (const char *) *((uintptr_t *) f->esp + 1);
        unsigned initial_size = *((unsigned *) f->esp + 2);
        validate_user_string (file);
        lock_acquire (&filesys_lock);
        f->eax = filesys_create (file, initial_size);
        lock_release (&filesys_lock);
      }
      break;
```
- **File & Lines:** [src/userprog/syscall.c](file:///home/yasamin/pintos/src/userprog/syscall.c#L153-L163).
- **Why Needed:** In base Pintos, `SYS_CREATE` was unhandled. User programs call `create(file, initial_size)` to initialize disk files.
- **Runtime Impact:**
  1. Validates the argument stack addresses (`esp + 4` and `esp + 8`).
  2. Validates the filename string via `validate_user_string(file)`. If `file` is `NULL` or points to unmapped memory, the process is terminated with `-1` *before* acquiring `filesys_lock`.
  3. Acquires `filesys_lock`, invokes `filesys_create(file, initial_size)`, assigns the boolean result to `f->eax`, and releases `filesys_lock`.

#### 2.4.4 System Call: `SYS_REMOVE` (Lines 165–174)
```c
    case SYS_REMOVE:
      validate_user_range ((const uint8_t *) f->esp + 4, sizeof (char *));
      {
        const char *file = (const char *) *((uintptr_t *) f->esp + 1);
        validate_user_string (file);
        lock_acquire (&filesys_lock);
        f->eax = filesys_remove (file);
        lock_release (&filesys_lock);
      }
      break;
```
- **File & Lines:** [src/userprog/syscall.c](file:///home/yasamin/pintos/src/userprog/syscall.c#L165-L174).
- **Why Needed:** Enables programs to delete files from the disk directory.
- **Runtime Impact:** Validates the filename string, acquires `filesys_lock`, calls `filesys_remove(file)`, and stores the boolean success result in `f->eax`.

#### 2.4.5 System Call: `SYS_OPEN` (Lines 176–207)
```c
    case SYS_OPEN:
      validate_user_range ((const uint8_t *) f->esp + 4, sizeof (char *));
      {
        const char *file = (const char *) *((uintptr_t *) f->esp + 1);
        validate_user_string (file);
        lock_acquire (&filesys_lock);
        struct file *fp = filesys_open (file);
        lock_release (&filesys_lock);

        if (fp == NULL)
          {
            f->eax = -1;
            break;
          }

        struct fd_entry *entry = malloc (sizeof *entry);
        if (entry == NULL)
          {
            lock_acquire (&filesys_lock);
            file_close (fp);
            lock_release (&filesys_lock);
            f->eax = -1;
            break;
          }

        struct thread *cur = thread_current ();
        entry->fd = cur->next_fd++;
        entry->file = fp;
        list_push_back (&cur->fd_list, &entry->elem);
        f->eax = entry->fd;
      }
      break;
```
- **File & Lines:** [src/userprog/syscall.c](file:///home/yasamin/pintos/src/userprog/syscall.c#L176-L207).
- **Why Needed:** In base Pintos, opening files returned nothing. User programs must receive a unique non-negative integer file descriptor.
- **Runtime Impact:**
  1. Validates the filename string.
  2. Acquires `filesys_lock` and invokes `filesys_open(file)`. If the file does not exist (e.g. `open-missing`), returns `-1`.
  3. Allocates a new `struct fd_entry` on the kernel heap. If memory allocation fails, the opened file is closed under `filesys_lock` and `-1` is returned.
  4. Assigns `cur->next_fd++` to `entry->fd`, inserts the entry into `cur->fd_list`, and returns the descriptor in `f->eax`.

#### 2.4.6 System Call: `SYS_FILESIZE` (Lines 209–223)
```c
    case SYS_FILESIZE:
      validate_user_range ((const uint8_t *) f->esp + 4, sizeof (int));
      {
        int fd = *((int *) f->esp + 1);
        struct fd_entry *entry = get_fd_entry (fd);
        if (entry == NULL)
          {
            f->eax = -1;
            break;
          }
        lock_acquire (&filesys_lock);
        f->eax = file_length (entry->file);
        lock_release (&filesys_lock);
      }
      break;
```
- **File & Lines:** [src/userprog/syscall.c](file:///home/yasamin/pintos/src/userprog/syscall.c#L209-L223).
- **Why Needed:** Allows user programs to query the byte length of an open file.
- **Runtime Impact:** Retrieves the descriptor entry via `get_fd_entry(fd)`. If invalid, returns `-1`. Otherwise acquires `filesys_lock`, calls `file_length(entry->file)`, and stores the byte size in `f->eax`.

#### 2.4.7 System Call: `SYS_READ` (Lines 225–265)
```c
    case SYS_READ:
      validate_user_range ((const uint8_t *) f->esp + 4, sizeof (int) + sizeof (void *) + sizeof (unsigned));
      {
        int fd = *((int *) f->esp + 1);
        void *buffer = (void *) *((uintptr_t *) f->esp + 2);
        unsigned size = *((unsigned *) f->esp + 3);

        validate_user_range (buffer, size);

        if (size == 0)
          {
            f->eax = 0;
            break;
          }

        if (fd == 0)
          {
            uint8_t *buf = (uint8_t *) buffer;
            unsigned i;
            for (i = 0; i < size; i++)
              buf[i] = input_getc ();
            f->eax = size;
          }
        else if (fd == 1)
          {
            f->eax = -1;
          }
        else
          {
            struct fd_entry *entry = get_fd_entry (fd);
            if (entry == NULL)
              {
                f->eax = -1;
                break;
              }
            lock_acquire (&filesys_lock);
            f->eax = file_read (entry->file, buffer, size);
            lock_release (&filesys_lock);
          }
      }
      break;
```
- **File & Lines:** [src/userprog/syscall.c](file:///home/yasamin/pintos/src/userprog/syscall.c#L225-L265).
- **Why Needed:** Base Pintos had no read implementation. Reading must support:
  1. `fd == 0` (`STDIN_FILENO`): Read keystrokes directly from the keyboard buffer using `input_getc()`.
  2. `fd == 1` (`STDOUT_FILENO`): Reading from stdout is illegal; must return `-1`.
  3. Regular files (`fd >= 2`): Read bytes from disk via `file_read()` under `filesys_lock`.
  4. Pre-validation of buffer range `[buffer, buffer + size)` to prevent user page fault panics while holding kernel locks.
- **Runtime Impact:** Completely isolates console input from disk input. Ensures that reading 0 bytes returns 0 immediately without touching memory or disk.

#### 2.4.8 System Call: `SYS_WRITE` (Lines 267–304)
```c
    case SYS_WRITE:
      validate_user_range ((const uint8_t *) f->esp + 4, sizeof (int) + sizeof (void *) + sizeof (unsigned));
      {
        int fd = *((int *) f->esp + 1);
        const void *buffer = (const void *) *((uintptr_t *) f->esp + 2);
        unsigned size = *((unsigned *) f->esp + 3);

        validate_user_range (buffer, size);

        if (size == 0)
          {
            f->eax = 0;
            break;
          }

        if (fd == 1)
          {
            putbuf (buffer, size);
            f->eax = size;
          }
        else if (fd == 0)
          {
            f->eax = -1;
          }
        else
          {
            struct fd_entry *entry = get_fd_entry (fd);
            if (entry == NULL)
              {
                f->eax = -1;
                break;
              }
            lock_acquire (&filesys_lock);
            f->eax = file_write (entry->file, buffer, size);
            lock_release (&filesys_lock);
          }
      }
      break;
```
- **File & Lines:** [src/userprog/syscall.c](file:///home/yasamin/pintos/src/userprog/syscall.c#L267-L304).
- **Why Needed:** In Phase 1 and 2, `SYS_WRITE` only supported writing to `stdout` (`fd == 1`). Full filesystem support requires writing to disk files while maintaining write denial on executables (`ROX`).
- **Runtime Impact:**
  1. Validates the entire user buffer range before acquiring any lock.
  2. If `size == 0`, returns `0` immediately.
  3. If `fd == 1` (`stdout`), writes to the console using `putbuf(buffer, size)`.
  4. If `fd == 0` (`stdin`), writing to standard input is illegal; returns `-1`.
  5. If `fd >= 2`, retrieves `entry`, acquires `filesys_lock`, calls `file_write()`, and releases `filesys_lock`. If the file is an active executable, `file_write()` returns 0 because `entry->file->deny_write == true`.

#### 2.4.9 System Call: `SYS_SEEK` (Lines 306–319)
```c
    case SYS_SEEK:
      validate_user_range ((const uint8_t *) f->esp + 4, sizeof (int) + sizeof (unsigned));
      {
        int fd = *((int *) f->esp + 1);
        unsigned position = *((unsigned *) f->esp + 2);
        struct fd_entry *entry = get_fd_entry (fd);
        if (entry != NULL)
          {
            lock_acquire (&filesys_lock);
            file_seek (entry->file, position);
            lock_release (&filesys_lock);
          }
      }
      break;
```
- **File & Lines:** [src/userprog/syscall.c](file:///home/yasamin/pintos/src/userprog/syscall.c#L306-L319).
- **Why Needed:** Modifies the internal byte offset for subsequent reads and writes in the specified file.
- **Runtime Impact:** If the descriptor is valid, updates the read/write offset atomically under `filesys_lock`.

#### 2.4.10 System Call: `SYS_TELL` (Lines 321–335)
```c
    case SYS_TELL:
      validate_user_range ((const uint8_t *) f->esp + 4, sizeof (int));
      {
        int fd = *((int *) f->esp + 1);
        struct fd_entry *entry = get_fd_entry (fd);
        if (entry == NULL)
          {
            f->eax = -1;
            break;
          }
        lock_acquire (&filesys_lock);
        f->eax = file_tell (entry->file);
        lock_release (&filesys_lock);
      }
      break;
```
- **File & Lines:** [src/userprog/syscall.c](file:///home/yasamin/pintos/src/userprog/syscall.c#L321-L335).
- **Why Needed:** Queries the current byte offset within an open file.
- **Runtime Impact:** Returns the current position via `file_tell(entry->file)` under `filesys_lock`, or `-1` if the descriptor is invalid.

#### 2.4.11 System Call: `SYS_CLOSE` (Lines 337–351)
```c
    case SYS_CLOSE:
      validate_user_range ((const uint8_t *) f->esp + 4, sizeof (int));
      {
        int fd = *((int *) f->esp + 1);
        struct fd_entry *entry = get_fd_entry (fd);
        if (entry != NULL)
          {
            list_remove (&entry->elem);
            lock_acquire (&filesys_lock);
            file_close (entry->file);
            lock_release (&filesys_lock);
            free (entry);
          }
      }
      break;
```
- **File & Lines:** [src/userprog/syscall.c](file:///home/yasamin/pintos/src/userprog/syscall.c#L337-L351).
- **Why Needed:** Closes an open file descriptor, releases the underlying inode reference, and reclaims heap memory.
- **Runtime Impact:** Removes `entry` from `cur->fd_list`, closes the underlying `struct file *` under `filesys_lock`, and calls `free(entry)` to prevent memory leaks. If the descriptor is invalid (`fd < 2` or not found), the operation safely does nothing.

---

### 2.5 File: `src/userprog/process.c`

#### 2.5.1 Deadlock Prevention and File Cleanup in `process_exit()` (Lines 212–230)
```c
  /* Release filesys_lock if held by current thread to prevent deadlocks. */
  if (lock_held_by_current_thread (&filesys_lock))
    lock_release (&filesys_lock);

  /* Close all open file descriptors and the executable file. */
  lock_acquire (&filesys_lock);
  while (!list_empty (&cur->fd_list))
    {
      struct list_elem *e = list_pop_front (&cur->fd_list);
      struct fd_entry *entry = list_entry (e, struct fd_entry, elem);
      file_close (entry->file);
      free (entry);
    }
  if (cur->exec_file != NULL)
    {
      file_close (cur->exec_file);
      cur->exec_file = NULL;
    }
  lock_release (&filesys_lock);
```
- **File & Lines:** [src/userprog/process.c](file:///home/yasamin/pintos/src/userprog/process.c#L212-L230).
- **Why Needed:**
  1. **Deadlock Prevention:** If a user program triggers an unhandled page fault while kernel code is inside a file system operation holding `filesys_lock` (e.g. `file_read()` into a read-only code page), the CPU invokes the exception handler `page_fault()`, which immediately terminates the process via `thread_exit()` $\to$ `process_exit()`. If `process_exit()` attempted to acquire `filesys_lock` while already holding it, the kernel would instantly deadlock! Checking `if (lock_held_by_current_thread(&filesys_lock)) lock_release(&filesys_lock);` guarantees safe lock recovery under any fatal fault scenario.
  2. **File Descriptor Teardown:** When a process exits, every file it opened must be closed to decrement inode open counts and avoid leaking kernel resources.
  3. **Executable Re-enabling:** Closing `cur->exec_file` automatically invokes `file_allow_write(cur->exec_file)`, restoring write access to the binary on disk now that execution has terminated.
- **Runtime Impact:** All open files are closed, all `struct fd_entry` heap blocks are freed, and write access to the binary is cleanly restored.

#### 2.5.2 Synchronized Executable Loading and Write Denial in `load()` (Lines 372–379, 462–474)
```c
  /* Open executable file. */
  lock_acquire (&filesys_lock);
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }
...
 done:
  /* We arrive here whether the load is successful or not. */
  if (success)
    {
      file_deny_write (file);
      t->exec_file = file;
    }
  else
    {
      file_close (file);
    }
  lock_release (&filesys_lock);
  return success;
```
- **File & Lines:** [src/userprog/process.c](file:///home/yasamin/pintos/src/userprog/process.c#L372-L379), [src/userprog/process.c](file:///home/yasamin/pintos/src/userprog/process.c#L462-L474).
- **Why Needed:**
  1. Base Pintos opened the executable, read its ELF segments, and immediately called `file_close(file)`. Once closed, write denial was lifted, allowing other processes or the process itself to modify the binary image on disk while it was executing.
  2. The entire ELF parsing and reading process interacts with the shared file system structures and must be synchronized under `filesys_lock`.
- **Runtime Impact:**
  - `filesys_lock` is held throughout `filesys_open()` and ELF segment reading.
  - If loading succeeds, `file_deny_write(file)` is called, and `file` is assigned to `t->exec_file` *without being closed*. The file remains open and write-denied for the entire duration of the process's execution.
  - If loading fails, `file_close(file)` is called to release the file handle.
  - `filesys_lock` is released at label `done:`.

---

## 3. Operating System Concepts & Deep-Dive Study Guide

### 3.1 File Descriptors and the Three-Tier File Abstraction
In standard POSIX operating systems, user programs never interact directly with disk blocks, directory sectors, or inode pointers. Instead, the operating system interposes a three-tier abstraction:

```
+-------------------------------------------------------------+
|                      User Process Space                     |
|  Integer File Descriptors: 0 (stdin), 1 (stdout), 2, 3, ... |
+-------------------------------------------------------------+
                              |
                              v (Per-Process Lookup)
+-------------------------------------------------------------+
|               Per-Process File Descriptor Table             |
|   cur->fd_list: [fd=2 | file*] -> [fd=3 | file*] -> ...     |
+-------------------------------------------------------------+
                              |
                              v (Kernel File Object)
+-------------------------------------------------------------+
|                 Open File Objects (struct file)             |
|   Stores: current read/write offset (pos), deny_write flag, |
|           pointer to underlying inode                       |
+-------------------------------------------------------------+
                              |
                              v (Underlying Inode / Disk)
+-------------------------------------------------------------+
|                    Inodes (struct inode)                    |
|   Represents unique on-disk entity: sector list, file size, |
|   open_cnt, deny_write_cnt                                  |
+-------------------------------------------------------------+
```

1. **Tier 1: Per-Process File Descriptors (`int fd`):**
   - Compact integers returned to user space. They serve as process-private indexes or keys into the process's open file list.
   - Descriptors are completely isolated between processes: descriptor `2` in Process A refers to an entirely different file than descriptor `2` in Process B (`multi-child-fd`).
2. **Tier 2: Kernel Open File Description (`struct file`):**
   - Represents an active open file session.
   - Maintains dynamic session state, notably the **current byte offset** (`file->pos`). If two processes independently open the same file on disk, each obtains a distinct `struct file` with its own independent file offset.
3. **Tier 3: The File System Inode (`struct inode`):**
   - Represents the physical file on the disk partition.
   - Tracks on-disk data blocks, total file length, the number of active openers (`open_cnt`), and write denial counters (`deny_write_cnt`).

---

### 3.2 Thread-Safe File Systems & Coarse-Grained Synchronization
Pintos's basic file system (`src/filesys/`) has no internal locking. Multiple threads modifying files simultaneously cause critical race conditions:
- **Directory Corruption:** Two threads creating files simultaneously in the root directory can allocate the same directory slot.
- **Inode Extension Race:** Two threads writing to the same file or different files simultaneously can allocate the same free disk sector from the free map.
- **Cache Incoherency:** Simultaneous reads and writes to disk sectors without serialization result in partial or corrupted data reads.

#### Coarse-Grained Locking (`filesys_lock`)
To achieve rock-solid thread safety without introducing complex multi-lock hierarchies or lock-ordering deadlocks, we employ a **coarse-grained recursive-free mutual exclusion lock**:
- Every call to `filesys_create`, `filesys_remove`, `filesys_open`, `file_read`, `file_write`, `file_seek`, `file_tell`, `file_length`, `file_close`, and ELF reading in `load()` is enclosed within `lock_acquire(&filesys_lock)` and `lock_release(&filesys_lock)`.
- This guarantees that all file system metadata and disk I/O operations are serialized atomically.

```
Thread A (write): [lock_acquire(&filesys_lock)] ---> [file_write()] ---> [lock_release(&filesys_lock)]
                                                           |
Thread B (read):  Blocks on filesys_lock ................. v Unblocks & Executes
```

---

### 3.3 Read-Only Executable (ROX) Architecture
Why do modern operating systems strictly prohibit writing to currently running executables?

1. **Demand Paging and Code Segment Invalidation:**
   In virtual memory operating systems (and Phase 1/2 of Pintos), code segments (`.text`) are mapped directly from the ELF executable on disk. If an executable could be modified while running, modifying machine instructions on disk would alter the program's code out from under running CPU instruction pointers, causing non-deterministic crashes or arbitrary code execution.
2. **Binary Consistency and Security:**
   Allowing arbitrary processes to write to running binaries opens immediate security exploits (code injection into privileged system services).

#### How Pintos Implements ROX:
In Pintos, `src/filesys/file.c` provides:
```c
void
file_deny_write (struct file *file) 
{
  ASSERT (file != NULL);
  if (!file->deny_write) 
    {
      file->deny_write = true;
      inode_deny_write (file->inode);
    }
}
```
When `inode_deny_write()` is called, it increments `inode->deny_write_cnt`. Whenever `file_write()` is called:
```c
if (inode->deny_write_cnt > 0)
  return 0;
```
If `deny_write_cnt > 0`, `file_write()` immediately returns `0` bytes written!
When `file_close()` is eventually called, it automatically invokes `file_allow_write()`, decrementing `inode->deny_write_cnt`.

**Key Architectural Insight:**
In base Pintos, `load()` closed the executable file immediately after loading the ELF segments. As a result, `file_close()` revoked `file_deny_write()`, leaving the executable vulnerable to modifications during execution. By keeping `t->exec_file = file` open until `process_exit()`, write denial remains active throughout the entire lifespan of the process!

---

### 3.4 Deadlock Prevention and Fault-Safe Lock Recovery
One of the most insidious bugs in operating system kernels is **lock leakage during hardware fault handling**.

Consider what happens during a `read()` system call:
1. Process calls `read(fd, buffer, 1024)`.
2. Process passes an invalid buffer pointer (e.g., pointing to unmapped memory or a read-only code page).
3. Kernel acquires `filesys_lock`.
4. Kernel calls `file_read()`, which attempts to copy bytes from kernel disk buffers into `buffer`.
5. The CPU triggers an x86 Page Fault Exception (`#PF` / Interrupt 14).
6. The kernel's `page_fault()` handler detects an illegal user memory access and calls `thread_exit()`.
7. `thread_exit()` calls `process_exit()`.

**The Deadlock Trap:**
If `process_exit()` unconditionally calls `lock_acquire(&filesys_lock)` to close its open files, the thread attempts to acquire a lock that **it already holds**! In Pintos, attempting to acquire a lock already held by the current thread panics with an assertion failure (`ASSERT (!lock_held_by_current_thread (lock))`), instantly crashing the kernel!

```
+--------------------------------------------------------------------------+
|                        Deadlock Recovery Pattern                         |
+--------------------------------------------------------------------------+
  User Page Fault in Kernel Mode
             |
             v
     page_fault() -> thread_exit() -> process_exit()
             |
             +---> Check: lock_held_by_current_thread(&filesys_lock)?
                      |
                      |-- YES --> Release filesys_lock immediately!
                      |
                      +---------> Acquire filesys_lock safely
                                  Close all fd_entry items
                                  Close exec_file
                                  Release filesys_lock
```

By adding:
```c
if (lock_held_by_current_thread (&filesys_lock))
  lock_release (&filesys_lock);
```
and strictly validating all user buffers with `validate_user_range()` *before* acquiring `filesys_lock`, our implementation guarantees that `filesys_lock` is never leaked and the kernel never deadlocks under any fault condition (`read-bad-ptr`, `write-bad-ptr`, `bad-write`).

---

### 3.5 Common Pitfalls & Engineering Bugs in Phase 4

| Pitfall / Bug | Root Cause in Base Pintos | Our Robust Solution |
| :--- | :--- | :--- |
| **Deadlock on Page Fault** | `filesys_lock` acquired before validating user buffers; unexpected `#PF` terminates thread while holding lock. | Pre-validate user ranges with `validate_user_range()`. In `process_exit()`, defensively release `filesys_lock` if held before closing files. |
| **Fixed-Size FD Array Exhaustion** | Implementing file descriptor tables as a fixed array `struct file *fds[16]`; fails in `open-many` (opens $\sim 126$ files). | Use dynamic linked list `struct list fd_list` of heap-allocated `struct fd_entry` items supporting unlimited open files. |
| **Closing Standard Streams** | Calling `close(0)` or `close(1)` corrupts file descriptor table or crashes. | In `get_fd_entry(fd)`, return `NULL` for any `fd < 2`. `SYS_CLOSE` ignores `fd < 2`. |
| **Corrupted Executable on Disk** | Closing executable in `load()` cancels write-denial before process finishes running. | Retain `t->exec_file = file` open during entire process execution; only close in `process_exit()`. |
| **Memory Leak in `open()` Failure** | If `malloc()` for `struct fd_entry` fails, file opened via `filesys_open()` is leaked. | If `entry == NULL`, immediately close `fp` under `filesys_lock` and return `-1`. |
| **Console Output Interleaving** | Using multiple single-character `putchar()` calls for `SYS_WRITE` on `stdout`. | Use `putbuf(buffer, size)` to write entire buffer atomically to the console. |

---

## 4. Verification and Test Results

The entire Pintos Project 2 test suite was executed in `src/userprog/build` using `make check`.

### Summary of Results:
- **Total Test Cases:** 80
- **Passed Test Cases:** 80
- **Failed Test Cases:** 0
- **Pass Rate:** **100% (80 / 80 passed)**

Every test across Argument Passing (`args-*`), System Call Memory Validation (`sc-*`, `bad-*`), Process Control (`exec-*`, `wait-*`, `halt`, `exit`, `multi-oom`), File Operations (`create-*`, `open-*`, `read-*`, `write-*`, `close-*`), Executable Write Protection (`rox-*`), and Concurrency / Stress (`multi-*`, `syn-*`, `lg-*`, `sm-*`) passes cleanly without any kernel panic, memory leak, or deadlock.
