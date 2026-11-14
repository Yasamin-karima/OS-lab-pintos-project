# Pintos Project 2: User Programs — Phase 2 Engineering & Deep-Dive Study Report
## User Memory Access Validation & Safe Page Fault Handling

---

## 1. Project & Phase Objectives

### 1.1 High-Level Project Goals (Pintos Project 2: User Programs)
In Pintos Project 2, the operating system transitions from executing exclusively trusted, cooperating kernel threads (Project 1) to executing untrusted user-space programs running under x86 hardware protection (Ring 3). The core responsibilities of the operating system in this environment are:
1. **Isolation & Protection:** Ensuring that an unprivileged user process cannot corrupt, inspect, or crash the kernel or other concurrent processes.
2. **Controlled Access (System Calls):** Providing a structured, secure gateway (via software interrupt `0x30`) through which user programs request kernel-mediated services (I/O, process control, file system operations).
3. **Resource Reclamation:** Cleanly reclaiming memory, locks, and file descriptors upon normal program exit or abnormal fault termination.

### 1.2 Architectural Goals of Phase 2
Phase 1 established argument passing on the user stack and the foundational synchronization primitives for `exit`, `halt`, `exec`, and `wait`. However, base Pintos possessed no verification of user pointers passed via system calls or registers. A malicious, buggy, or exploratory user program could pass arbitrary pointers:
- A `NULL` pointer (`0x00000000`).
- An address within unmapped virtual address space (sparse address space).
- A kernel-space address (`>= PHYS_BASE`, typically `0xc0000000`).
- A multi-byte data structure, 32-bit integer, or string that begins in valid user memory but crosses a page boundary into unmapped or kernel memory (`sc-boundary`, `sc-boundary-2`, `sc-boundary-3`).
- A corrupted stack pointer (`%esp`) pointing to unmapped or kernel memory (`sc-bad-sp`, `sc-bad-arg`).

The architectural goal of **Phase 2** is to implement **bulletproof user memory access validation** and **safe page fault recovery**:
1. Every pointer, buffer, string, or stack argument supplied by user space must be validated across all its bytes before the kernel dereferences it.
2. If any access violates address boundaries, mapping rules, or memory protections, the kernel must cleanly terminate **only** that calling user process with exit status `-1` (emitting `<process_name>: exit(-1)` via `process_exit()`).
3. Under no circumstances may a user program cause a kernel panic, leak kernel memory, or crash the operating system.

---

### 1.3 Targeted and Verified Pintos Test Cases

| Test Case | Description & Verification Objective | Expected Exit / Behavior |
| :--- | :--- | :--- |
| `sc-bad-sp` | Invokes a system call with `%esp` intentionally set to an unmapped address roughly 64MB below the code segment. | Clean termination with exit code `-1`. |
| `sc-bad-arg` | Places `SYS_EXIT` at `0xbffffffc` (the very top word of user space). The argument at `esp + 4` lies at `0xc0000000` (`PHYS_BASE`, kernel space). | Clean termination with exit code `-1`. |
| `sc-boundary` | Places the system call number at the end of a mapped page and its argument at the start of the next mapped page. | Must succeed and execute `exit(42)`. |
| `sc-boundary-2` | Places a 4-byte system call argument hanging across the boundary between two mapped pages (3 bytes on page 1, 1 byte on page 2). | Must succeed and execute `exit(67)`. |
| `sc-boundary-3` | Places a system call number such that its first byte is on a valid mapped page, but the remaining 3 bytes spill into unmapped memory. | Clean termination with exit code `-1`. |
| `bad-read` | A user program attempts to dereference `NULL` in user space (`*(volatile int *) NULL`). | Hardware `#PF` caught; process terminates with `-1`. |
| `bad-read2` | A user program attempts to read from an unmapped address (`0x04000000`). | Hardware `#PF` caught; process terminates with `-1`. |
| `bad-write` | A user program attempts to write to an unmapped address (`*(int *) 0x04000000 = 42`). | Hardware `#PF` caught; process terminates with `-1`. |
| `bad-write2` | A user program attempts to write to the read-only code segment. | Rights violation `#PF` caught; process terminates with `-1`. |
| `bad-jump` | A user program attempts to execute instructions starting at `NULL` (`0x00000000`). | Hardware `#PF` caught; process terminates with `-1`. |
| `bad-jump2` | A user program attempts to jump to an unmapped virtual address. | Hardware `#PF` caught; process terminates with `-1`. |
| `exec-bad-ptr` | Passes an unmapped pointer (`0x20101234`) as the `cmd_line` string argument to `exec()`. | `validate_user_string` rejects; process exits with `-1`. |
| `write-bad-ptr` | Passes an unmapped pointer (`0x10123420`) as the buffer to `write()`. | `validate_user_range` rejects; process exits with `-1`. |

---

## 2. Detailed Code Walkthrough

Two source files in `src/userprog/` implement Phase 2:
1. `src/userprog/syscall.c`: Proactive memory validation helpers and stack argument range verification.
2. `src/userprog/exception.c`: Reactive page fault exception handling for user-mode faults and kernel-mode user pointer faults.

---

### 2.1 File: `src/userprog/syscall.c`

#### 2.1.1 Header Inclusions and Prototypes (Lines 1–15)
```c
#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "devices/shutdown.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"

static void syscall_handler (struct intr_frame *);
static void terminate_invalid_process (void) NO_RETURN;
static void validate_user_addr (const void *uaddr);
static void validate_user_range (const void *uaddr, size_t size);
static void validate_user_string (const char *str);
```
- **Why Needed:** 
  - `threads/vaddr.h` exposes `is_user_vaddr()`, `pg_round_down()`, `PHYS_BASE`, and `PGSIZE`.
  - `userprog/pagedir.h` exposes `pagedir_get_page()`, allowing the kernel to inspect the current thread's page directory.
- **Runtime Impact:** Provides the required interfaces for virtual memory interrogation and declares validation helper functions before their use in `syscall_handler()`.

---

#### 2.1.2 Process Termination Helper: `terminate_invalid_process()` (Lines 23–29)
```c
static void
terminate_invalid_process (void)
{
  struct thread *cur = thread_current ();
  cur->exit_status = -1;
  thread_exit ();
}
```
- **Line Numbers:** Lines 23–29 in `src/userprog/syscall.c`.
- **Why Needed:** In base Pintos, invalid operations or unrecognized system calls either printed a message and returned or panicked the kernel. Pintos Project 2 requires that any bad user pointer or memory violation terminates the faulting user process with an exit status of `-1`.
- **Runtime Impact:** Sets `cur->exit_status = -1` and calls `thread_exit()`. `thread_exit()` calls `process_exit()`, which prints `"<process_name>: exit(-1)\n"`, updates `cur->child_status->exit_status = -1`, wakes up any waiting parent via `sema_up(&cs->sema)`, cleans up child structures, and releases the process's page directory. The kernel remains running without panic.

---

#### 2.1.3 Single Pointer Validation: `validate_user_addr()` (Lines 31–38)
```c
static void
validate_user_addr (const void *uaddr)
{
  if (uaddr == NULL || !is_user_vaddr (uaddr))
    terminate_invalid_process ();
  if (pagedir_get_page (thread_current ()->pagedir, uaddr) == NULL)
    terminate_invalid_process ();
}
```
- **Line Numbers:** Lines 31–38 in `src/userprog/syscall.c`.
- **Why Needed:** Before dereferencing any byte of user memory, the kernel must ensure three conditions:
  1. `uaddr != NULL`: Dereferencing `NULL` causes a trap.
  2. `is_user_vaddr(uaddr)`: `(uintptr_t) uaddr < (uintptr_t) PHYS_BASE`. Without this check, a user program could pass a kernel address (e.g., `0xc0000000`), allowing user programs to trick the kernel into reading or overwriting internal kernel data structures.
  3. `pagedir_get_page(...) != NULL`: The virtual address must be mapped in the process's page table. Accessing an unmapped virtual page in kernel mode would trigger a kernel page fault.
- **Runtime Impact:** If any check fails, execution immediately diverts to `terminate_invalid_process()`, halting process execution before illegal memory access occurs.

---

#### 2.1.4 Multi-Byte Range and Page-Boundary Validation: `validate_user_range()` (Lines 40–69)
```c
static void
validate_user_range (const void *uaddr, size_t size)
{
  if (uaddr == NULL)
    terminate_invalid_process ();

  if (size == 0)
    {
      if (!is_user_vaddr (uaddr))
        terminate_invalid_process ();
      return;
    }

  const uint8_t *start = (const uint8_t *) uaddr;
  const uint8_t *end = start + size - 1;

  if (end < start)
    terminate_invalid_process ();

  if (!is_user_vaddr (start) || !is_user_vaddr (end))
    terminate_invalid_process ();

  const uint8_t *page = (const uint8_t *) pg_round_down (start);
  while (page <= end)
    {
      if (pagedir_get_page (thread_current ()->pagedir, page) == NULL)
        terminate_invalid_process ();
      page += PGSIZE;
    }
}
```
- **Line Numbers:** Lines 40–69 in `src/userprog/syscall.c`.
- **Why Needed:** 
  1. **Cross-Page Spanning:** Checking only the first byte of a 4-byte integer or multi-byte buffer fails if the buffer starts near the end of a page (e.g., offset `4094` of a 4096-byte page). Bytes 0 and 1 may reside on a valid mapped page, while bytes 2 and 3 cross into an unmapped page or kernel space (`PHYS_BASE`).
  2. **Integer Arithmetic Overflow:** An adversary could pass `uaddr = 0xfffffff0` and `size = 0x20`. Calculating `start + size` wraps around zero, bypassing naive `< PHYS_BASE` bounds checks. Testing `if (end < start)` detects and halts unsigned arithmetic wrap-around.
  3. **Zero-Byte Operations (`write-zero`):** A zero-byte buffer write (`write(fd, buf, 0)`) must verify that `uaddr` is non-NULL and points to user virtual memory, but should not fail if `size == 0`.
- **Runtime Impact:** Iterates through every unique page frame (`PGSIZE = 4096` stride) spanned by `[start, end]`. If any page is unmapped or lies outside user space, the process is terminated immediately.

---

#### 2.1.5 Null-Terminated String Validation: `validate_user_string()` (Lines 71–85)
```c
static void
validate_user_string (const char *str)
{
  if (str == NULL)
    terminate_invalid_process ();

  const char *p = str;
  while (true)
    {
      validate_user_addr (p);
      if (*p == '\0')
        break;
      p++;
    }
}
```
- **Line Numbers:** Lines 71–85 in `src/userprog/syscall.c`.
- **Why Needed:** String parameters (such as `cmd_line` in `exec()`) have variable length unknown in advance. A naive `strlen(str)` or string copy in the kernel without pre-validation would trigger a kernel-mode page fault if the string extends into unmapped memory without a null terminator.
- **Runtime Impact:** Validates each byte address `p` via `validate_user_addr(p)` *before* dereferencing `*p`. If `p` steps across a page boundary into unmapped space or hits `PHYS_BASE`, it is caught safely before reading.

---

#### 2.1.6 Stack Pointer and Argument Parsing in `syscall_handler()` (Lines 87–149)
```c
static void
syscall_handler (struct intr_frame *f) 
{
  validate_user_range (f->esp, sizeof (uint32_t));
  int syscall_num = *(int *) f->esp;

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

    default:
      terminate_invalid_process ();
      break;
    }
}
```
- **Line Numbers:** Lines 87–149 in `src/userprog/syscall.c`.
- **Why Needed:** 
  - `validate_user_range(f->esp, sizeof(uint32_t))` guarantees that the 4 bytes comprising `syscall_num` reside entirely in mapped user space (`sc-bad-sp`, `sc-boundary-3`).
  - For each specific system call, the arguments on the stack (`f->esp + 4`, `f->esp + 8`, etc.) are explicitly validated with `validate_user_range()` before being read. In `sc-bad-arg`, `f->esp = 0xbffffffc`; reading argument 1 at `f->esp + 4` (`0xc0000000`) is trapped and rejected cleanly before dereference.
  - In `SYS_WRITE`, the user buffer pointer and its length are verified across all pages via `validate_user_range(buffer, size)` before passing the pointer to `putbuf()`.
- **Runtime Impact:** Guarantees that invalid stack pointers or invalid buffers cannot cause a kernel crash during system call handling.

---

### 2.2 File: `src/userprog/exception.c`

#### 2.2.1 Header Inclusions (Line 7)
```c
#include "threads/vaddr.h"
```
- **Line Numbers:** Line 7 in `src/userprog/exception.c`.
- **Why Needed:** Provides access to the macro `is_user_vaddr()`.

---

#### 2.2.2 User-Safe Exception Trap: `page_fault()` (Lines 147–157)
```c
  /* Determine cause. */
  not_present = (f->error_code & PF_P) == 0;
  write = (f->error_code & PF_W) != 0;
  user = (f->error_code & PF_U) != 0;

  if (user || is_user_vaddr (fault_addr))
    {
      thread_current ()->exit_status = -1;
      thread_exit ();
    }

  /* To implement virtual memory, delete the rest of the function
     body, and replace it with code that brings in the page to
     which fault_addr refers. */
```
- **Line Numbers:** Lines 147–157 in `src/userprog/exception.c`.
- **Why Needed:** In base Pintos, `page_fault()` printed diagnostic output and called `kill(f)`. `kill(f)` forced a kernel panic if the fault occurred in kernel mode, or printed unhandled exception messages.
  - When user code directly executes an illegal read (`bad-read`, `bad-read2`), illegal write (`bad-write`, `bad-write2`), or illegal instruction fetch (`bad-jump`, `bad-jump2`), the x86 CPU raises Interrupt 14 with `user == true` (`PF_U` bit set in error code).
  - Furthermore, if a kernel system call accesses a user pointer, a page fault may occur with `user == false`, but `fault_addr` is a user virtual address (`is_user_vaddr(fault_addr)`).
- **Runtime Impact:** Whenever `user == true` or `is_user_vaddr(fault_addr)` is true, the fault is classified as an illegal user memory access. The handler sets `thread_current()->exit_status = -1` and calls `thread_exit()`. The process exits cleanly with `-1`, and the kernel continues normal scheduling without panicking. Only genuine kernel bugs accessing kernel memory (`!user && !is_user_vaddr(fault_addr)`) fall through to `kill(f)`.

---

## 3. Operating System Concepts & Deep-Dive Study Guide

### 3.1 User Space vs. Kernel Space Isolation and Ring Protection

On x86 architecture, the CPU operates in distinct privilege levels called **rings**:
- **Ring 0 (Kernel Mode):** Has unrestricted access to hardware instructions (e.g., `lgdt`, `lidt`, `mov %cr3`), all I/O ports, and the entire virtual address space.
- **Ring 3 (User Mode):** Restricted execution mode. Privileged instructions cause general protection faults (`#GP`). Memory access is restricted to pages marked with the User/Supervisor bit (`U/S = 1`) in page directory and page table entries.

```
+-------------------------------------------------------------------+ 0xFFFFFFFF
|                                                                   |
|                   Kernel Virtual Memory Space                     |
|                   (Mapped with U/S = 0, Ring 0 Only)              |
|                                                                   |
+-------------------------------------------------------------------+ 0xC0000000 (PHYS_BASE)
|                                                                   |
|                    User Virtual Memory Space                      |
|                    (Mapped with U/S = 1, Ring 3 Accessible)       |
|                                                                   |
|   - User Stack (expands downward from PHYS_BASE)                  |
|   - Unmapped Sparse Space                                         |
|   - User Code & Data Segments (loaded from ELF starting 0x08048000)|
|   - Reserved Page 0 (0x00000000 - 0x00000FFF unmapped)            |
+-------------------------------------------------------------------+ 0x00000000
```

When a user program issues `int $0x30`, the x86 hardware automatically transitions from Ring 3 to Ring 0:
1. Switches the stack pointer `%esp` to the kernel thread's stack via TSS (`tss->esp0`).
2. Pushes user-mode `%ss`, `%esp`, `%eflags`, `%cs`, and `%eip` onto the kernel stack.
3. Invokes `intr_entry` (in `threads/intr-stubs.S`), constructing `struct intr_frame`.
4. Executes `syscall_handler(&if_)`.

While the CPU is in Ring 0 during a system call, **hardware segment checks for Ring 3 no longer apply to data accesses**. The kernel can dereference any virtual address. Therefore, if the kernel blindly dereferences a pointer provided by the user without validation, the user process can manipulate the kernel into modifying kernel data structures or accessing arbitrary physical memory.

---

### 3.2 Two-Level Paging and Virtual-to-Physical Translation

In 32-bit x86 protected mode with standard 4KB paging, virtual addresses are translated to physical addresses using a **two-level page table tree**:

```
        Virtual Address (32 bits)
 31                  22 21                  12 11                   0
+----------------------+----------------------+----------------------+
|  Directory Index     |   Table Index        |    Byte Offset       |
|     (10 bits)        |     (10 bits)        |     (12 bits)        |
+----------------------+----------------------+----------------------+
           |                      |                      |
           v                      |                      |
    Page Directory                v                      |
   +---------------+          Page Table                 |
   |     PDE       |-----> +---------------+             |
   +---------------+       |     PTE       |----+        |
   |     ...       |       +---------------+    |        |
                           |     ...       |    |        |
                                                v        v
                                            Physical Page Frame
                                           +-------------------+
                                           |  Physical Data    |
                                           +-------------------+
```

1. **Page Directory Base Register (`%cr3`):** Contains the physical address of the current process's page directory (4KB, containing 1024 Page Directory Entries / PDEs).
2. **PDE (Bits 31–22):** Selects one of 1024 page tables.
3. **PTE (Bits 21–12):** Selects one of 1024 page frames (4KB each).
4. **Offset (Bits 11–0):** Selects the exact byte within the 4096-byte frame.

In Pintos, `pagedir_get_page(uint32_t *pd, const void *uaddr)` performs this lookup in software:
```c
void *
pagedir_get_page (uint32_t *pd, const void *uaddr) 
{
  uint32_t *pte = lookup_page (pd, uaddr, false);
  if (pte != NULL && (*pte & PTE_P))
    return pte_get_page (*pte) + pg_ofs (uaddr);
  else
    return NULL;
}
```
If a virtual address has no mapped page table or the Present bit (`PTE_P`) is `0`, `pagedir_get_page()` returns `NULL`. This provides the kernel with a software mechanism to check mapping validity without risking an uncontrolled page fault.

---

### 3.3 Hardware Interrupt 14: Page Fault Architecture

When an invalid memory access occurs on x86, the CPU triggers **Interrupt 14 (`#PF`)**:
1. **Faulting Address (`%cr2`):** The CPU automatically stores the 32-bit linear address that triggered the fault into control register `%cr2`.
2. **Error Code:** The CPU pushes a 32-bit error code onto the stack:
   - `bit 0 (PF_P)`: `0` if caused by a non-present page; `1` if caused by a page-level rights violation (e.g., write to read-only page).
   - `bit 1 (PF_W)`: `0` if caused by a read access; `1` if caused by a write access.
   - `bit 2 (PF_U)`: `0` if fault occurred while CPU was in supervisor mode (Ring 0); `1` if in user mode (Ring 3).

```
                      Hardware Page Fault (#PF) Occurs
                                     |
                                     v
                        Read %cr2 -> fault_addr
                     Read f->error_code -> (P, W, U)
                                     |
                                     v
                       Is fault user-originated?
                       (user == true || is_user_vaddr(fault_addr))
                                  /      \
                           YES   /        \   NO
                                v          v
                 thread_current()->exit_status = -1;    Kernel Panic / kill(f)
                 thread_exit();                        (Genuine Kernel Bug)
                                |
                                v
                 process_exit() prints:
                 "<name>: exit(-1)"
                 Reclaims memory and notifies parent
```

---

### 3.4 Multi-Byte & Page Boundary Hazards

A common vulnerability in OS kernels is validating only the base address of a data structure. In 32-bit x86, a word is 4 bytes. Consider a 4-byte argument spanning across the boundary of a mapped page:

```
Case A: sc-boundary-2 (Split Argument across Mapped Pages - Valid)
Page boundary at 0xBFFFF000:
   Page 1 (Mapped, User):    ... [Byte -3] [Byte -2] [Byte -1] |
   Page 2 (Mapped, User):                                      | [Byte 0] ...
   Syscall Argument = bytes [-3, -2, -1, 0]. Both pages valid -> Syscall proceeds!

Case B: sc-boundary-3 (Spill into Unmapped Memory - Invalid)
Page boundary at 0xBFFFF000:
   Page 1 (Mapped, User):    ... [Byte 0 (valid)] |
   Page 2 (Unmapped):                             | [Byte 1] [Byte 2] [Byte 3] ...
   Syscall Number = bytes [0, 1, 2, 3].
   Checking only Byte 0 would mistakenly allow the read, causing an unhandled fault.
   validate_user_range checks [Byte 0] through [Byte 3] -> Catches unmapped Page 2 -> Exits -1!

Case C: sc-bad-arg (Spill into Kernel Space - Invalid)
PHYS_BASE boundary at 0xC0000000:
   User Space (< PHYS_BASE):  ... [0xBFFFFFFC] (Syscall Number = SYS_EXIT)
   Kernel Space (>= PHYS_BASE):   [0xC0000000] (Argument status)
   f->esp lies in user space, but f->esp + 4 lies in kernel space.
   validate_user_range checks (f->esp + 4) -> Catches >= PHYS_BASE -> Exits -1!
```

---

### 3.5 Comparison: Proactive Validation vs. Reactive Page Fault Handling

Operating systems generally employ two strategies for user pointer handling:

| Feature / Strategy | Proactive Validation (Pintos Project 2 Approach) | Reactive Fault Handling (Modern Linux / BSD) |
| :--- | :--- | :--- |
| **Mechanism** | Kernel verifies `is_user_vaddr()` and traverses page tables via `pagedir_get_page()` before every read/write. | Kernel accesses user pointer directly in an assembly helper (`copy_from_user`), registering a fixup landing pad in an exception table. |
| **Overhead** | Software page directory lookup overhead on every system call argument. | Zero lookup overhead on valid accesses; fault cost paid only on bad pointers. |
| **Complexity** | Simple, explicit, portable C logic. | Requires assembler macros, exception tables, and stack frame rewinding. |
| **Coverage in Pintos** | Primary defense against invalid system call arguments and buffers in `syscall.c`. | Secondary safety net in `exception.c` catching direct user faults (`bad-read`, `bad-write`, `bad-jump`). |

Combining **both approaches** in Pintos guarantees that neither malicious syscall parameters nor faulty user instruction execution can compromise kernel stability.

---

### 3.6 Common Pitfalls & Implementation Bugs

1. **Off-by-One in Range End Calculation:**
   Using `const uint8_t *end = start + size;` instead of `start + size - 1`. If a buffer ends at offset `4095` (the last byte of a page), `start + size` points to byte `0` of the *next* page. If that next page is unmapped, an off-by-one bug causes the kernel to reject a completely valid buffer.
2. **Integer Wrap-Around Vulnerability:**
   Failing to check `if (end < start)`. If `start = 0xFFFFFFFC` and `size = 16`, `end = 0x0000000B`. A loop checking `page <= end` might fail to iterate or incorrectly validate only low addresses, leaving high memory unverified.
3. **Validating Only Pointer Pointers:**
   In `SYS_EXEC`, validating `f->esp + 4` (the address on the stack containing `cmd_line`), but forgetting to call `validate_user_string(cmd_line)` to validate the string characters themselves.
4. **Calling `printf` or Panicking on Bad Pointers:**
   Printing debug messages or executing `PANIC()` on invalid user addresses fails Pintos test grading harnesses. Test harnesses require strict conformance to the exit message format: `printf("%s: exit(%d)\n", cur->name, cur->exit_status);`.
