# گزارش جامع فنی و آموزشی فاز ۱ پروژه ۲ پینتوس (Pintos Project 2)
**فاز ۱: ارسال آرگومان‌ها (Argument Passing)، همگام‌سازی انتظار/خروج فرآیندها و فراخوانی‌های پایه‌ای سیستم (System Calls)**

---

## ۱. اهداف کلی پروژه و اهداف این فاز (Project & Phase Objectives)

### ۱.۱ مروری بر اهداف پروژه ۲ پینتوس (برنامه‌های کاربر - User Programs)
در پروژه ۱ پینتوس، تمامی ریسمان‌ها (Threads) و کدها صرفاً در فضای هسته (Kernel Space) و با بالاترین سطح دسترسی پردازنده (سطح دسترسی ۰ یا Ring 0) اجرا می‌شدند. در آن مدل، تمامی ریسمان‌ها حافظه فیزیکی و ساختارهای داده سراسری را به صورت مشترک و بدون هیچ‌گونه محافظت سخت‌افزاری به اشتراک می‌گذاشتند.

پروژه ۲ پینتوس با معرفی مفهوم **فرآیند کاربر (User Process)**، سیستم‌عامل را وارد دنیای واقعی چندوظیفگی ایمن می‌کند. فرآیند کاربر در سطح دسترسی محافظت‌شده پردازنده (سطح دسترسی ۳ یا Ring 3) اجرا می‌شود و دارای فضای آدرس مجازی (Virtual Address Space) ایزوله و پشته کاربری (User Stack) مستقل است. یک برنامه کاربر حق دسترسی مستقیم به داده‌های هسته، پورت‌های سخت‌افزاری و آدرس‌های سایر فرآیندها را ندارد. در صورتی که فرآیند کاربر به منابع سخت‌افزاری یا خدمات سیستم‌عامل (مانند چاپ روی ترمینال، دسترسی به فایل یا اجرای فرآیند دیگر) نیاز داشته باشد، باید از طریق سازوکار **فراخوانی سیستم (System Call)** به هسته پیام فرستاده و کنترل را موقتاً به آن بسپارد.

### ۱.۲ اهداف معماری و پیاده‌سازی فاز ۱
فاز ۱ زیربنای لازم برای اجرای برنامه‌های کاربر و انتقال آرگومان‌های خط فرمان را پیاده‌سازی می‌کند:
1. **پارس کردن ایمن خط فرمان (Command-Line Parsing):** جداسازی نام فایل اجرایی از سایر آرگومان‌ها در تابع `process_execute()` بدون سرریز شدن حافظه پشته یا بافر محدود نام ریسمان (`thread->name`).
2. **برپاسازی و قالب‌بندی پشته کاربر (User Stack Setup):** تخصیص یک صفحه حافظه برای پشته در بالاترین مرز فضای کاربر یعنی `PHYS_BASE` (`0xc0000000`) و قرار دادن رشته‌های آرگومان‌ها، اعمال هم‌ترازی ۴ بیتی کلمه (Word Alignment)، آرایه اشاره‌گرهای `argv`، خود مقدار `argv`، شمارنده `argc` و آدرس بازگشت ساختگی مطابق با قرارداد فراخوانی استاندارد ۳۲ بیتی اینتل (`cdecl`).
3. **همگام‌سازی چرخه حیات والد و فرزند (Parent-Child Synchronization):** طراحی ساختار ردگیری وضعیت فرزند (`struct child_status`) به همراه شمارشگر ارجاع (Reference Counting) و سمافور همگام‌ساز، برای رفع شرایط رقابتی (Race Conditions) در مراحل بارگذاری فرآیند (`process_execute`) و انتظار برای پایان فرآیند (`process_wait` و `process_exit`).
4. **راه‌اندازی چارچوب فراخوانی‌های سیستم (System Call Handler) و اعتبارسنجی حافظه:** ایجاد مدیریت‌کننده وقفه نرم‌افزاری `int $0x30`، اعتبارسنجی دقیق تمامی اشاره‌گرهای کاربر به منظور ممانعت از کرش کردن هسته، و پیاده‌سازی فراخوانی‌های `halt`، `exit`، `write` (خروجی استاندارد)، `wait` و `exec`.

### ۱.۳ آزمون‌های هدف‌گذاری شده و تایید شده در فاز ۱
پیاده‌سازی این فاز با موفقیت تمامی آزمون‌های زیر را پشت سر گذاشته است:

| نام آزمون | قابلیت مورد ارزیابی | شرح نحوه تایید |
|---|---|---|
| `args-none` | راه‌اندازی پشته بدون هیچ آرگومان اضافه | بررسی می‌کند که `argc == 1`، نام برنامه `argv[0] == "args-none"`، اشاره‌گر انتهایی `argv[1] == NULL` و کد خروج `0` باشد. |
| `args-single` | ارسال یک آرگومان تکی | بررسی می‌کند که آرگومان اضافه دریافت شده و `argc == 2`، `argv[1] == "onearg"` باشد. |
| `args-multiple` | ارسال چندین آرگومان با فاصله‌های مجزا | بررسی ترتیب اشاره‌گرها، دسترسی صحیح به هر رشته، و مقداردهی درست `argc == 5`. |
| `args-many` | ارسال تعداد زیادی آرگومان (۲۳ آرگومان) | بررسی پر نشدن بافر، محاسبات پشته و ایندکس‌گذاری درست تا ۲۳ آرگومان (`a` تا `v`). |
| `args-dbl-space` | مدیریت فاصله‌های متوالی و توخالی | تضمین می‌کند که وجود دو یا چند کاراکتر فاصله (Space) باعث ایجاد توکن‌های خالی نشده و `strtok_r` به درستی عمل کند. |
| `halt` | خاموش کردن سیستم | بررسی عملکرد `SYS_HALT` و خاموش شدن تمیز شبیه‌ساز با `shutdown_power_off()`. |
| `exit` | خروج عادی فرآیند | بررسی ثبت کد خروج و چاپ پیام استاندارد خروج `<process_name>: exit(<code>)`. |
| `wait-simple` | همگام‌سازی انتظار والد برای فرزند | اطمینان از مسدود شدن والد تا زمان خاتمه فرزند و دریافت کد خروج فرزند. |
| `wait-twice` | فراخوانی تکراری wait | تضمین می‌کند اگر والد دو بار روی یک فرزند `wait()` صدا بزند، بار دوم فوراً `-1` برگرداند. |
| `wait-bad-pid` | اعتبارسنجی شناسه فرآیند | اطمینان از بازگرداندن `-1` در صورت انتظار برای فرزند نامعتبر یا فرآیندی که فرزند این والد نیست. |
| `wait-killed` | خاتمه غیرعادی فرزند به دلیل استثنا | اطمینان از این که اگر فرزند به دلیل خطای حافظه یا استثنا کشته شد، `wait` مقدار `-1` برمی‌گرداند. |
| `exec-multiple` | اجرای چند فرآیند همزمان | ایجاد و اجرای همزمان فرزندان توسط والد و انتظار برای تک‌تک آن‌ها. |
| `exec-missing` | اجرای برنامه ناموجود | تضمین بازگرداندن `-1` بدون ایجاد وحشت در هسته (Kernel Panic) در صورت موجود نبودن فایل. |
| `exec-bad-ptr` | امنیت اشاره‌گر نامعتبر در exec | بررسی این که ارسال آدرس نامعتبر به عنوان نام برنامه، باعث مرگ ایمن فرآیند با کد `-1` می‌شود. |

---

## ۲. بررسی دقیق تغییرات کد به تفکیک فایل و خطوط (Detailed Code Walkthrough)

### ۲.۱ فایل: `src/filesys/filesys.h`
#### خط ۱۲: افزودن کلیدواژه `extern` برای متغیر سراسری `fs_device`
```c
/** Block device that contains the file system. */
extern struct block *fs_device;
```
- **چرا این تغییر لازم بود؟**
  در اسکلت اولیه پینتوس، متغیر `fs_device` بدون `extern` در هدر تعریف شده بود. در کامپایلرهای امروزی (GCC 10 به بالا)، فلگ `-fno-common` به عنوان حالت پیش‌فرض فعال است. این امر باعث می‌شود هر فایل کامپایل‌شده‌ای (`filesys.c`، `init.c`، `shutdown.c`، `process.c`) که این هدر را وارد می‌کند یک تعریف مستقل برای `fs_device` ایجاد کند و پیونددهنده (`ld`) با خطای `multiple definition of 'fs_device'` متوقف شود.
- **اثر زمان اجرا (Runtime Impact):**
  متغیر سراسری تنها یک بار در `src/filesys/filesys.c` حافظه می‌گیرد و در سایر فایل‌ها به عنوان متغیر بیرونی شناخته می‌شود، که مانع از تداخل نمادها هنگام پیوند هسته می‌گردد.

---

### ۲.۲ فایل: `src/Make.config`
#### خط ۴۳: افزودن فلگ `-fcommon` به متغیر `CFLAGS`
```c
CFLAGS = -g -m32 -O -fno-strict-aliasing -fcommon -Wa,--divide -fno-pie -fno-stack-protector -mno-sse
```
- **چرا این تغییر لازم بود؟**
  برخی کدهای زیرساخت تست کاربر (مانند `tests/lib.c` و `tests/userprog/multi-recurse.c`) متغیر سراسری `test_name` را به صورت غیرمقداردهی اولیه بازتعریف کرده‌اند. در کامپایلرهای نوین بدون `-fcommon`، پیوند برنامه‌های تستی پینتوس با شکست مواجه می‌شد.
- **اثر زمان اجرا (Runtime Impact):**
  امکان ادغام نمادهای عمومی مشترک برای برنامه‌های کاربر فراهم شده و فایل‌های اجرایی تست بدون خطا بیلد می‌شوند.

---

### ۲.۳ فایل: `src/Makefile.userprog`
#### خط ۶: غیرفعال‌سازی جداسازی بخش کد در پیونددهنده با `-Wl,-z,noseparate-code`
```c
# Linker flags.
$(PROGS): LDFLAGS += -nostdlib -static -Wl,-T,$(LDSCRIPT) -Wl,-z,noseparate-code
```
- **چرا این تغییر لازم بود؟**
  در نسخه‌های جدید `ld` (مربوط به `binutils 2.30` به بعد)، رفتار پیش‌فرض پیونددهنده ایجاد دو بخش جداگانه `PT_LOAD` برای هدر فایل و کد اجرایی است (`-z separate-code`). هر دوی این بخش‌ها در آدرس مجازی `0x08048000` قرار می‌گرفتند. بنابراین وقتی بارگذار ELF پینتوس در تابع `load()` سعی می‌کرد بخش دوم را در حافظه بنشاند، تابع `install_page()` اعلام می‌کرد که صفحه `0x08048000` قبلاً نگاشت شده است؛ در نتیجه بارگذاری فرآیند کاربر با شکست روبرو شده و برنامه با خطای `-1` خارج می‌شد.
- **اثر زمان اجرا (Runtime Impact):**
  با افزودن `-Wl,-z,noseparate-code`، پیونددهنده تمامی محتویات کد و هدرهای فقط‌خواندنی را در یک سگمنت ادغام می‌کند و مشکل برخورد نگاشت حافظه در آدرس `0x08048000` به کلی برطرف می‌گردد.

---

### ۲.۴ فایل: `src/threads/thread.h`
#### خطوط ۴ تا ۸ و ۲۵ تا ۳۸: تعریف ساختار وضعیت فرزند (`struct child_status`)
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
- **چرا این تغییر لازم بود؟**
  در سیستم اولیه پینتوس به محض آن‌که یک ریسمان با `thread_exit()` به پایان می‌رسد، صفحه حافظه آن در تابع `thread_schedule_tail()` به طور کامل آزاد می‌شود. در نتیجه اگر ریسمان والد بخواهد بعداً در `process_wait()` وضعیت خروج فرزند را بخواند، با خطای دسترسی به حافظه آزادشده (Use-After-Free) مواجه خواهد شد.
- **اثر زمان اجرا (Runtime Impact):**
  این ساختار در هیپ هسته (`malloc`) ساخته می‌شود و جدا از صفحه ریسمان حیات دارد. شمارشگر ارجاع (`ref_cnt = 2`) نشان می‌دهد هم والد و هم فرزند به آن ارجاع دارند. هر طرف که زودتر بمیرد، ارجاع خود را کم می‌کند و آخرین طرفی که خارج می‌شود حافظه را آزاد می‌نماید. بدین ترتیب هیچ حافظه‌ای نشت نمی‌کند و هیچ اشاره‌گر معلقی (Dangling Pointer) باقی نمی‌ماند.

#### خطوط ۱۲۴ تا ۱۲۹: فیلدهای جدید در `struct thread`
```c
#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint32_t *pagedir;                  /**< Page directory. */
    struct list children;               /**< List of child_status structs. */
    struct child_status *child_status;  /**< This thread's child_status in parent. */
    int exit_status;                    /**< Exit status. */
#endif
```
- **چرا این تغییر لازم بود؟**
  - `children`: والد باید فهرستی از وضعیت تمامی فرزندان متولدشده خود را نگه دارد تا بداند مجاز به انتظار برای کدام `tid` است.
  - `child_status`: اشاره‌گر ریسمان فرزند به بلوک وضعیت خودش در لیست والد، تا در زمان خروج بتواند کد خروج را بنویسد و سمافور را آزاد کند.
  - `exit_status`: نگهداری کد خروجی فرآیند برای گزارش به والد.
- **اثر زمان اجرا (Runtime Impact):**
  دسترسی مستقیم و در زمان $O(1)$ فرآیند به وضعیت والد-فرزندی و جداول صفحات خود.

---

### ۲.۵ فایل: `src/threads/thread.c`
#### خطوط ۶۷۶ تا ۶۸۱: مقداردهی اولیه فیلدهای فرآیند در `init_thread()`
```c
#ifdef USERPROG
  t->pagedir = NULL;
  list_init (&t->children);
  t->child_status = NULL;
  t->exit_status = -1;
#endif
```
- **چرا این تغییر لازم بود؟**
  ریسمان‌های جدید یا ریسمان اولیه `main` بدون مقداردهی لیست فرزندان دچار مقادیر نامعتبر حافظه می‌شدند. همچنین مقدار اولیه `exit_status` برابر `-1` قرار داده شده تا اگر فرآیندی به دلیل بروز خطا یا استثنا بدون اجرای فراخوانی `exit` متوقف شد، به درستی خروج ناموفق (`-1`) گزارش گردد.
- **اثر زمان اجرا (Runtime Impact):**
  تضمین ساختار امن لیست‌ها و پایداری در رفتار پیش‌فرض خروج فرآیندها.

---

### ۲.۶ فایل: `src/userprog/process.c`

#### خطوط ۲۵ تا ۳۱: ساختار اشتراک اطلاعات بارگذاری (`struct exec_info`)
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
- **چرا این تغییر لازم بود؟**
  در پینتوس پایه، تابع `process_execute()` ریسمان فرزند را ایجاد می‌کرد و بلافاصله شناسه آن را برمی‌گرداند، بدون این‌که منتظر بماند آیا برنامه با موفقیت لود شده یا خیر. طبق صورت پروژه، اگر فایل اجرایی وجود نداشته باشد، `process_execute()` باید فوراً `-1` برگرداند.
- **اثر زمان اجرا (Runtime Impact):**
  سمافور `sema_load` والد را در حالت انتظار قرار می‌دهد تا فرزند در تابع `start_process()` فایل را باز کرده و لود کند و نتیجه موفقیت یا عدم موفقیت را در `load_success` گزارش دهد.

#### خطوط ۴۰ تا ۱۲۴: اجرای فرآیند و جداسازی نام برنامه (`process_execute`)
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
- **چرا این تغییر لازم بود؟**
  1. آرایه `thread->name` تنها ۱۶ بایت ظرفیت دارد. اگر رشته کامل `file_name` با آرگومان‌هایش (مثلاً `"args-multiple some arguments for you!"`) به `thread_create()` پاس داده می‌شد، نام برنامه سرریز شده یا بریده می‌شد و چاپ خروج فرآیند با خطا مواجه می‌گردید.
  2. کپی کردن در دو صفحه مجزا (`fn_copy` و `fn_prog`) اجازه می‌دهد رشته‌های خط فرمان تا ۴ کیلوبایت با ایمنی کامل و بدون سرریز پشته هسته پردازش شوند.
  3. آزادسازی دقیق حافظه در تمامی مسیرهای شکست و موفقیت، مانع از نشت حافظه می‌گردد.
- **اثر زمان اجرا (Runtime Impact):**
  نام ریسمان دقیقا به نام فایل اجرایی تنظیم می‌شود، والد در `sema_load` معلق می‌ماند و در صورت شکست بارگذاری، با تمیزکاری کامل حافظه، مقدار `-1` برمی‌گرداند.

#### خطوط ۱۲۹ تا ۱۸۶: پارس آرگومان‌ها و شروع فرآیند (`start_process`)
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
- **چرا این تغییر لازم بود؟**
  رشته خط فرمان باید با توابع رشته‌ای استاندارد (`strtok_r`) شکسته شده و فقط توکن اول (`argv[0]`) به تابع بارگذار `load()` داده شود. پس از برپایی پشته کاربر، حافظه ۴ کیلوبایتی `file_name` آزاد شده و والد از وضعیت بارگذاری مطلع می‌شود.
- **اثر زمان اجرا (Runtime Impact):**
  فرآیند فرزند با یک پشته کاربر کاملاً آماده و مقادیر ثبات‌های بخش کاربر (`SEL_UDSEG` و `SEL_UCSEG`) از طریق دستور شبیه‌سازی بازگشت از وقفه (`intr_exit`) وارد فضای کاربر می‌شود.

#### خطوط ۱۹۵ تا ۲۲۸: همگام‌سازی انتظار والد برای فرزند (`process_wait`)
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
- **چرا این تغییر لازم بود؟**
  در کد اولیه پینتوس، `process_wait()` بدون هیچ عملی `-1` برمی‌گرداند که باعث خاموش شدن سریع کرنل می‌شد. همچنین فرآیند نباید بتواند روی فرزند دیگران یا دو بار روی یک فرزند منتظر بماند.
- **اثر زمان اجرا (Runtime Impact):**
  والد تا زمان پایان فرزند در `sema_down` متوقف می‌شود، وضعیت را می‌خواند، فرزند را از لیست حذف کرده و ارجاع را کاهش می‌دهد. در صورت فراخوانی مجدد، فرزند در لیست پیدا نشده و فوراً `-1` بازگردانده می‌شود.

#### خطوط ۲۳۱ تا ۲۸۶: خروج تمیز فرآیند و آزادسازی منابع (`process_exit`)
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
- **چرا این تغییر لازم بود؟**
  چاپ پیغام خروج الزامی است، اما فقط برای فرآیندهای کاربر (`cur->pagedir != NULL`) و نه ریسمان‌های سیستمی هسته. فرزند باید پایان خود را به والد اعلام کند (`sema_up`) و فرزندان یتیم (Orphans) مانده در لیست والد نیز به درستی با کاهش شمارشگر ارجاع مدیریت شوند.
- **اثر زمان اجرا (Runtime Impact):**
  فرآیند خاتمه‌یافته جدول صفحات خود را تخریب کرده و به جدول صفحه پایه هسته برمی‌گردد.

#### خطوط ۶۲۰ تا ۶۶۱: ساختاردهی به پشته کاربر (`setup_stack`)
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
- **چرا این تغییر لازم بود؟**
  تابع نقطه شروع کاربر `_start (int argc, char *argv[])` انتظار دارد پشته درست مانند زمانی باشد که یک دستور `call` اجرا شده است.
- **اثر زمان اجرا (Runtime Impact):**
  رشته‌ها از راست به چپ در بالای پشته قرار گرفته، اشاره‌گر پشته روی مرز ۴ بایت هم‌تراز می‌شود، اشاره‌گرهای آدرس‌های مجازی کاربران درج شده و ورودی‌های تابع `main` به صورت استاندارد تامین می‌گردد.

---

### ۲.۷ فایل: `src/userprog/syscall.c`

#### خطوط ۱۳ تا ۸۹: توابع کمکی اعتبارسنجی اشاره‌گرهای کاربر
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
- **چرا این تغییر لازم بود؟**
  اگر برنامه‌ای اشاره‌گر `NULL`، آدرسی در فضای هسته (`>= PHYS_BASE`) یا آدرسی نگاشت‌نشده ارسال کند، هسته نباید دچار خطای نقض دسترسی یا کرنل پنیک شود.
- **اثر زمان اجرا (Runtime Impact):**
  بررسی تمامی صفحات بافر تا انتهای بایت آخر و متوقف کردن فرآیند متخلف با وضعیت خروج `-1`.

#### خطوط ۹۱ تا ۱۵۲: پیاده‌سازی مدیریت‌کننده سیستمی (`syscall_handler`)
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
- **چرا این تغییر لازم بود؟**
  پردازش تماس‌های وقفه `0x30` و پیاده‌سازی تماس‌های پایه‌ای مورد نیاز فاز ۱.
- **اثر زمان اجرا (Runtime Impact):**
  اجرای امن فراخوانی‌های پایه‌ای و بازگرداندن مقادیر از طریق ثبات `f->eax`.

---

### ۲.۸ فایل: `src/userprog/exception.c`

#### خطوط ۸۶ تا ۹۰ و ۱۴۹ تا ۱۵۳: خاتمه آرام استثناهای کاربر
```c
    case SEL_UCSEG:
      /* User's code segment, so it's a user exception, as we
         expected.  Kill the user process.  */
      thread_current ()->exit_status = -1;
      thread_exit (); 
```
```c
  if (user)
    {
      thread_current ()->exit_status = -1;
      thread_exit ();
    }
```
- **چرا این تغییر لازم بود؟**
  در اسکلت پینتوس، رخداد نقص صفحه کاربر منجر به پرینت جزئیات فریم و محتویات ثبات‌ها می‌شد که باعث رد شدن اسکریپت‌های ارزیابی خودکار آزمون‌ها می‌گردید.
- **اثر زمان اجرا (Runtime Impact):**
  فرآیند در صورت بروز استثنا در بخش کاربر با وضعیت `-1` خاتمه می‌یابد.

---

## ۳. آموزش مفاهیم سیستم‌عامل مرتبط با این فاز (Deep-Dive Study Guide)

### ۳.۱ ایزوله‌سازی فضای کاربر از هسته و حفاظت سخت‌افزاری
پردازنده‌های معماری x86 امنیت حافظه را از طریق حلقه‌های حفاظتی (Privilege Rings) اعمال می‌کنند:
- **حلقه ۰ (Ring 0 - حالت نظارتی یا Supervisor):** سیستم‌عامل و هسته در این سطح اجرا می‌شوند و به کلیه دستورات پردازنده، جداول صفحات و ثبات‌های کنترلی دسترسی دارند.
- **حلقه ۳ (Ring 3 - حالت کاربر یا User):** برنامه‌های کاربر در این سطح اجرا شده و محدود هستند.

در پینتوس، مرز حافظه مجازی در آدرس `PHYS_BASE = 0xc0000000` قرار دارد:
```
+----------------------------------+ 0xFFFFFFFF
|                                  |
|         فضای آدرس مجازی هسته     |   فقط قابل دسترسی در Ring 0
|       (نگاشت ۱ به ۱ به فیزیکی)   |
|                                  |
+----------------------------------+ 0xC0000000 (PHYS_BASE)
|      پشته کاربر (رشد رو به پایین) |
|                 |                |
|                 v                |   قابل دسترسی در Ring 3 و Ring 0
|                 ^                |
|                 |                |
|        کد، دیتا و BSS کاربر      |
+----------------------------------+ 0x08048000
|      فضای حفاظت خطای صفحه ۰      |
+----------------------------------+ 0x00000000
```
اگر کدی در Ring 3 تلاش کند آدرسی بالاتر از `PHYS_BASE` را بخواند، واحد مدیریت حافظه سخت‌افزاری (MMU) وقفه شماره ۱۴ (Page Fault) را صادر می‌کند.

---

### ۳.۲ قرارداد فراخوانی x86 (cdecl) و چینش پشته
در معماری ۳۲ بیتی اینتل، پشته رو به پایین رشد می‌کند. هنگام فراخوانی یک تابع:
1. پارامترها از **راست به چپ** در پشته پوش (Push) می‌شوند.
2. آدرس بازگشت به عنوان آخرین مقدار قبل از ورود به تابع پوش می‌شود.

#### دیاگرام چینش پشته ساخته‌شده برای اجرای دستور `args-multiple some arguments for you!`
```
آدرس           نام مؤلفه                 داده موجود                 نوع داده
-----------------------------------------------------------------------------
0xbffffffb     رشته argv[4]              "you!\0"                   char[5]
0xbffffff7     رشته argv[3]              "for\0"                    char[4]
0xbfffffee     رشته argv[2]              "arguments\0"              char[10]
0xbfffffe9     رشته argv[1]              "some\0"                   char[5]
0xbfffffdc     رشته argv[0]              "args-multiple\0"          char[14]
-----------------------------------------------------------------------------
0xbfffffd8     هم‌ترازی کلمه (Padding)   0x00, 0x00, 0x00, 0x00     uint8_t[4]
-----------------------------------------------------------------------------
0xbfffffd4     نگهبان انتهایی argv[5]    0x00000000                 char * (NULL)
0xbfffffd0     اشاره‌گر به argv[4]        0xbffffffb                 char *
0xbfffffcb     اشاره‌گر به argv[3]        0xbffffff7                 char *
0xbfffffc4     اشاره‌گر به argv[2]        0xbfffffee                 char *
0xbfffffc0     اشاره‌گر به argv[1]        0xbfffffe9                 char *
0xbfffffbc     اشاره‌گر به argv[0]        0xbfffffdc                 char *
-----------------------------------------------------------------------------
0xbfffffb8     اشاره‌گر argv              0xbfffffbc                 char **
0xbfffffb4     تعداد آرگومان‌ها (argc)    5                          int
0xbfffffb0     آدرس بازگشت ساختگی        0x00000000                 void (*) ()
-----------------------------------------------------------------------------
```
هنگامی که تابع نقطه ورود `_start` اجرا می‌شود، اشاره‌گر `%esp` دقیقا روی `0xbfffffb0` است. به این ترتیب برنامه کاربر طبق قرارداد `cdecl` ورودی‌های خود را می‌خواند.

---

### ۳.۳ وقفه نرم‌افزاری و قاب وقفه (`struct intr_frame`)
هنگامی که دستور `int $0x30` اجرا می‌شود:
1. پردازنده به سطح Ring 0 تغییر حالت می‌دهد.
2. پشته از پشته کاربر به پشته هسته موجود در ساختار TSS سوییچ می‌شود.
3. مقادیر `%ss`, `%esp`, `%eflags`, `%cs`, `%eip` به طور خودکار توسط سخت‌افزار روی پشته هسته پوش می‌شوند.
4. کدهای اسمبلی پینتوس ثبات‌های عمومی را با `pushal` اضافه کرده و اشاره‌گر قاب وقفه (`struct intr_frame`) را به `syscall_handler` پاس می‌دهند.

```
                    اجرای برنامه در فضای کاربر
                                |
                         دستور int $0x30
                                |
+-------------------------------v-------------------------------+
|                      سخت‌افزار پردازنده                        |
|  ۱. سوییچ به Ring 0                                           |
|  ۲. دریافت پشته هسته از TSS                                    |
|  ۳. پوش کردن ثبات‌های پشته و آدرس کاربر                        |
+-------------------------------+-------------------------------+
                                |
+-------------------------------v-------------------------------+
|                 اسمبلی هسته (intr-stubs.S)                    |
|  ۱. ذخیره ثبات‌های سگمنت و ثبات‌های عمومی                      |
|  ۲. فراخوانی تابع سیستمی syscall_handler                      |
+-------------------------------+-------------------------------+
                                |
+-------------------------------v-------------------------------+
|               مدیریت‌کننده سیستم‌کال (syscall.c)              |
|  ۱. اعتبارسنجی اشاره‌گرها و بافر کاربر                         |
|  ۲. اجرای عملیات درخواستی (SYS_WRITE, SYS_EXIT, ...)          |
|  ۳. ذخیره نتیجه بازگشتی در f->eax                             |
+-------------------------------+-------------------------------+
                                |
+-------------------------------v-------------------------------+
|                    مسیر بازگشت (intr_exit)                     |
|  ۱. بازیابی ثبات‌ها                                            |
|  ۲. اجرای دستور iret پردازنده                                  |
|  ۳. سوییچ مجدد به Ring 3 و پشته کاربر                          |
+-------------------------------+-------------------------------+
                                |
                     ادامه اجرای برنامه کاربر
```

---

### ۳.۴ چرخه حیات، همگام‌سازی والد و فرزند و شمارشگر ارجاع
در مدیریت فرآیندها، چهار حالت احتمالی در رابطه زمانی والد و فرزند وجود دارد:
1. **فرزند قبل از فراخوانی `wait` توسط والد خاتمه می‌یابد:** فرزند وضعیت خود را در ساختار اشتراکی ثبت کرده، سمافور را آزاد می‌کند و وارد وضعیت مرده می‌شود. والد به محض فراخوانی `wait` بدون انتظار وضعیت را می‌خواند.
2. **والد قبل از خاتمه فرزند `wait` را فراخوانی می‌کند:** والد پشت سمافور قفل می‌شود (`sema_down`). با خروج فرزند، والد بیدار شده و وضعیت را دریافت می‌کند.
3. **والد قبل از فرزند می‌میرد (فرزند یتیم - Orphan):** والد هنگام خروج ارجاع خود به فرزند را کم می‌کند. هنگامی که فرزند بعداً تمام شد، آخرین ارجاع را کم کرده و حافظه را خودش آزاد می‌کند.
4. **والد هرگز روی فرزند `wait` صدا نمی‌زند:** والد هنگام خروج در `process_exit` ساختار فرزندانی که مانده‌اند را تمیز می‌کند.

---

### ۳.۵ دام‌های رایج برنامه‌نویسی و نکات عیب‌یابی (Common Pitfalls)

1. **عدم هم‌ترازی اشاره‌گر پشته (Stack Misalignment):**
   - *علت:* اگر قبل از پوش کردن اشاره‌گرها مقدار `*esp` مضربی از ۴ نباشد، پردازنده در خواندن کلمات ۳۲ بیتی دچار افت کارایی یا خطای سخت‌افزاری می‌شود.
   - *راه‌حل:* اعمال حتمی `*esp = (void *)((uintptr_t)*esp & ~3)`.
2. **اشاره‌گرهای معلق در پشته کاربر:**
   - *علت:* اگر اشاره‌گرهای `argv` به بافر موقت هسته (`fn_copy`) اشاره کنند، پس از آزاد شدن آن صفحه، برنامه کاربر کاراکترهای مخدوش می‌خواند.
   - *راه‌حل:* ابتدا تمامی رشته‌ها در پشته کاربر کپی شوند و آدرس‌های حافظه مجازی پشته کاربر در آرایه اشاره‌گرها ذخیره گردد.
3. **سرریز نام ریسمان:**
   - *علت:* کپی مستقیم رشته دستور کامل در فیلد ۱۶ بایتی `thread->name`.
   - *راه‌حل:* استخراج توکن اول با `strtok_r` در یک بافر مجزا و ارسال صرفاً نام اجرایی به `thread_create`.
4. **تداخل سگمنت‌های پیونددهنده‌های جدید (`-z separate-code`):**
   - *علت:* تقسیم هدر و کد به دو سگمنت که هر دو روی صفحه `0x08048000` قرار می‌گیرند.
   - *راه‌حل:* افزودن فلگ `-Wl,-z,noseparate-code` به `Makefile.userprog`.
