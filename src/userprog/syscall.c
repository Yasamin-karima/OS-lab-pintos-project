#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"

struct lock filesys_lock;

static void syscall_handler (struct intr_frame *);
static void terminate_invalid_process (void) NO_RETURN;
static void validate_user_addr (const void *uaddr);
static void validate_user_range (const void *uaddr, size_t size);
static void validate_user_string (const char *str);
static struct fd_entry *get_fd_entry (int fd);

void
syscall_init (void) 
{
  lock_init (&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

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

    default:
      terminate_invalid_process ();
      break;
    }
}
