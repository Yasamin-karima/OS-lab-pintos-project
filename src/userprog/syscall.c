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

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
terminate_invalid_process (void)
{
  struct thread *cur = thread_current ();
  cur->exit_status = -1;
  thread_exit ();
}

static void
validate_user_addr (const void *uaddr)
{
  if (uaddr == NULL || !is_user_vaddr (uaddr))
    terminate_invalid_process ();
  if (pagedir_get_page (thread_current ()->pagedir, uaddr) == NULL)
    terminate_invalid_process ();
}

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
