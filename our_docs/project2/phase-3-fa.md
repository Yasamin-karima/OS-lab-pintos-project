# پروژه ۲ پینتوس: برنامه‌های کاربر (User Programs) — گزارش فنی و راهنمای جامع فاز ۳
## مدیریت چرخه حیات پردازه و همگام‌سازی والد-فرزند (SYS_EXEC، SYS_WAIT، SYS_EXIT، SYS_HALT)

---

## ۱. اهداف کلی پروژه و اهداف این فاز (Project & Phase Objectives)

### ۱.۱ اهداف کلان پروژه ۲ پینتوس (User Programs)
در پروژه ۱ پینتوس، تمامی ریسه‌ها (Threads) به عنوان کدهای معتمد در فضای هسته (Kernel Mode یا Ring 0) و بدون تفکیک حافظه اجرا می‌شدند. در پروژه ۲، سیستم‌عامل گامی اساسی به سوی یک سیستم‌عامل واقعی برمی‌دارد و امکان بارگذاری و اجرای **برنامه‌های کاربر (User Programs)** در سطح دسترسی غیرممتاز سخت‌افزاری (Ring 3 معماری x86) را فراهم می‌سازد. اهداف کلان این معماری عبارتند از:
1. **جداسازی و حفاظت (Isolation & Protection):** یک برنامه کاربر غیرمعتمد هرگز نباید بتواند به حافظه اختصاصی هسته یا داده‌های سایر پردازه‌ها دستبرد بزند، در اجرای آن‌ها تداخل ایجاد کند یا سیستم‌عامل را دچار Kernel Panic نماید.
2. **درگاه فراخوانی‌های سیستمی (System Calls Gateway):** فراهم آوردن یک مسیر ارتباطی کنترل‌شده و امن از طریق وقفه نرم‌افزاری `int $0x30` تا پردازه‌ها بتوانند سرویس‌های هسته (مانند ورودی/خروجی، کنترل پردازه و مدیریت فایل) را درخواست کنند.
3. **آزادسازی امن منابع (Resource Reclamation):** بازپس‌گیری کامل تمام حافظه‌ها، قفل‌ها و توصیف‌کننده‌های فایل به هنگام خروج عادی یا کشته شدن پردازه به دلیل خطای حافظه.
4. **مدیریت چرخه حیات پردازه (Process Lifecycle Management):** فراهم‌سازی معناشناسی دقیق و مقاوم در برابر شرایط رقابتی (Race Conditions) برای ایجاد پردازه جدید (`exec`)، انتظار برای پایان فرزند (`wait`)، خروج پردازه (`exit`) و خاموش کردن ماشین (`halt`).

---

### ۱.۲ اهداف معماری فاز ۳ (Architectural Goals of Phase 3)
در فازهای ۱ و ۲، ارسال آرگومان‌ها در پشته و اعتبارسنجی نفوذناپذیر حافظه کاربر پیاده‌سازی شدند. با این وجود، پینتوس پایه از نقص‌های بنیادین در مدل چرخه حیات و همگام‌سازی پردازه‌ها رنج می‌برد:
- تابع `process_execute()` کاملاً ناهمگام (Asynchronous) بود؛ این تابع بلافاصله پس از صدا زدن `thread_create()` شناسه ریسه را بازمی‌گرداند، بدون آن‌که منتظر بماند تا مشخص شود آیا پردازه فرزند در بارگذاری فایل اجرایی در حافظه مجازی (`load()`) موفق شده است یا خیر. در مواردی که فایل اجرایی وجود نداشت (آزمون `exec-missing`)، تابع `exec()` به جای بازگرداندن مقدار خطای `TID_ERROR` (`-1`)، یک PID معتبر بازمی‌گرداند.
- تابع `process_wait()` دارای یک پیاده‌سازی صوری و توخالی بود که همواره `-1` برمی‌گرداند. والد به هیچ عنوان توانایی مسدود شدن و انتظار تا پایان فرزند، یا دریافت کد خروج (`exit_status`) فرزند را نداشت.
- به محض خروج یک ریسه فرزند در `thread_exit()`، ساختار `struct thread` و صفحه ۴ کیلوبایتی هسته آن بلافاصله توسط زمان‌بند در `thread_schedule_tail()` آزاد می‌شدند. در نتیجه، هرگونه اشاره‌گری که والد به فرزند داشت تبدیل به یک «اشاره‌گر آویزان» (Dangling Pointer) شده و خواندن وضعیت خروج با خطای Use-After-Free مواجه می‌گردید.
- هیچ سازوکاری برای مدیریت حالت‌های مختلف همزمانی در چرخه حیات والد و فرزند وجود نداشت:
  1. فرزند زودتر از فراخوانی `wait()` توسط والد خاتمه یابد.
  2. والد در حالی که فرزند هنوز در حال اجراست، فراخوانی `wait()` را صادر کند.
  3. والد بدون فراخوانی `wait()` خاتمه یابد (ایجاد پردازه‌های یتیم یا Orphan).
  4. والد بیش از یک بار برای یک فرزند واحد فراخوانی `wait()` انجام دهد (`wait-twice`).
  5. والد شناسه پردازه‌ای نامعتبر یا متعلق به غیر از فرزندان مستقیم خود را برای انتظار ارسال کند (`wait-bad-pid`).
  6. فرزند به دلیل خطای استثنا یا اشاره‌گر بد کشته شود (`wait-killed`).
  7. صدها پردازه بازگشتی به صورت متوالی در شرایط کمبود شدید حافظه اجرا شده و بدون نشت حتی یک بایت حافظه خاتمه یابند (`multi-oom`).

هدف معماری **فاز ۳** طراحی و پیاده‌سازی **معماری همگام‌سازی والد-فرزند مبتنی بر شمارش ارجاع (Reference Counting) و سمافورها** است:
1. **راستی‌آزمایی همگام بارگذاری در `exec()`:** والد در `process_execute()` بر روی سمافور اختصاصی `load_sema` مسدود می‌شود تا فرزند تلاش خود برای `load()` را کامل کند. در صورت شکست بارگذاری، مقدار `-1` بازگردانده می‌شود.
2. **جداسازی ردیابی چرخه حیات از ریسه اجرایی:** ساختار مجزای `struct child_status` بر روی هیپ (Heap) تخصیص می‌یابد تا حتی پس از نابودی کامل صفحه ریسه فرزند، اطلاعات خروج برای والد زنده و معتبر بماند.
3. **آزادسازی امن و بدون نشت حافظه (Zero Memory Leaks):** با استفاده از شمارشگر ارجاع (`ref_count = 2`) که توسط قفل `ref_lock` محافظت می‌شود، هر کدام از طرفین (والد یا فرزند) که دیرتر به کار خود پایان دهد، ساختار وضعیت مشترک را از روی هیپ آزاد (`free`) می‌کند.
4. **پیاده‌سازی فراخوانی‌های سیستمی:** اتصال فراخوانی‌های `SYS_EXEC`، `SYS_WAIT`، `SYS_EXIT` و `SYS_HALT` در درگاه وقفه `syscall_handler()` همراه با اعتبارسنجی دقیق پشته و اشاره‌گرها.

---

### ۱.۳ آزمون‌های هدف و راستی‌آزمایی‌شده در فاز ۳

| نام آزمون (Test Case) | شرح سناریو و هدف راستی‌آزمایی | رفتار و خروجی مورد انتظار |
| :--- | :--- | :--- |
| `exec-once` | پردازه والد دستور `exec("child-simple")` را اجرا کرده و بلافاصله منتظر پایان آن می‌ماند. | فرزند مقدار ۸۱ را چاپ کرده و خارج می‌شود؛ والد با موفقیت کد ۸۱ را دریافت کرده و با ۸۱ خارج می‌شود. |
| `exec-arg` | والد دستور `exec("child-args childarg")` را صدا زده و صحت ارسال آرگومان به برنامه جدید را می‌سنجد. | فرزند صحت آرگومان‌ها را تایید کرده و با ۰ خارج می‌شود؛ والد با ۰ خارج می‌شود. |
| `exec-multiple` | والد چندین پردازه فرزند را به صورت متوالی ایجاد کرده و برای تک‌تک آن‌ها منتظر می‌ماند. | هر فرزند با کد ۸۱ خاتمه یافته و والد کد خروج تک‌تک آن‌ها را به درستی جمع‌آوری می‌کند. |
| `exec-missing` | والد تلاش می‌کند یک فایل اجرایی ناموجود (`exec("no-such-file")`) را اجرا کند. | تابع `load()` شکست می‌خورد؛ والد از طریق `load_sema` مطلع شده و مقدار `-1` بازمی‌گرداند. |
| `exec-bad-ptr` | یک اشاره‌گر نامعتبر و نگاشت‌نشده (`0x20101234`) به عنوان نام دستور به `exec` ارسال می‌شود. | تابع `validate_user_string()` اشاره‌گر را رد کرده و پردازه با کد خروج `-1` کشته می‌شود. |
| `wait-simple` | والد یک پردازه فرزند ساده را ایجاد کرده و بلافاصله `wait(child_pid)` را صدا می‌زند. | والد مسدود شده تا فرزند پایان یابد؛ پس از خروج فرزند با کد ۸۱، والد بیدار شده و ۸۱ را برمی‌گرداند. |
| `wait-twice` | والد پس از یک بار انتظار موفق روی فرزند و دریافت کد ۸۱، مجدداً `wait(child_pid)` را صدا می‌زند. | فراخوانی دوم باید بلافاصله و بدون مسدودسازی مقدار `-1` بازگرداند. |
| `wait-killed` | والد فرزندی را اجرا می‌کند که با خواندن از حافظه نامعتبر (`NULL`) دچار نقص صفحه سخت‌افزاری می‌شود. | هسته فرزند را با کد `-1` خاتمه می‌دهد؛ فراخوانی `wait()` در والد مقدار `-1` را دریافت می‌کند. |
| `wait-bad-pid` | والد یک شناسه ساختگی یا نامربوط را به `wait()` تحویل می‌دهد. | هسته لیست فرزندان والد را جستجو کرده و پس از نیافتن PID، فوراً `-1` بازمی‌گرداند. |
| `multi-oom` | آزمون فشار حافظه با اجرای بازگشتی فرزندان در ۱۰ تکرار متوالی تا مرز اتمام کامل حافظه فیزیکی (`depth >= 30`). فرزندان معیوب (`-k`) و باز کردن فایل‌ها را شبیه‌سازی می‌کند. | هسته نباید کرش کند، قفل شود یا حافظه نشت دهد؛ عمق بازگشت در هر ۱۰ تکرار باید کاملاً یکسان باشد. |

---

## ۲. بررسی دقیق تغییرات کد به تفکیک فایل و خطوط (Detailed Code Walkthrough)

تغییرات فاز ۳ بر چهار فایل کلیدی در مسیرهای `src/threads/` و `src/userprog/` تمرکز دارد:
1. `src/threads/thread.h`: تعریف ساختار وضعیت فرزند و فیلدهای والد-فرزند در ریسه.
2. `src/threads/thread.c`: پیاده‌سازی تابع بازپس‌گیری شمارنده ارجاع و مقداردهی اولیه ریسه‌ها.
3. `src/userprog/process.c`: پیاده‌سازی همگام `process_execute()`، بارگذاری در `start_process()`، مدیریت انتظار در `process_wait()` و خروج در `process_exit()`.
4. `src/userprog/syscall.c`: رسیدگی به فراخوانی‌های سیستمی مرتبط با چرخه حیات.

---

### ۲.۱ فایل: `src/threads/thread.h`

#### ۲.۱.۱ ساختار ردیابی وضعیت فرزند: `struct child_status` (خطوط ۲۶ تا ۴۱)
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
- **شماره خطوط:** خطوط ۲۶ تا ۴۱ در فایل `src/threads/thread.h`.
- **علت ضرورت:** در نسخه پایه پینتوس، وضعیت پردازه درون `struct thread` ذخیره می‌شد. با مرگ ریسه، کل صفحه آن آزاد شده و کد وضعیت خروج از دست می‌رفت. این ساختار مستقل بر روی هیپ هسته تخصیص می‌یابد تا بقای اطلاعات خروج را پس از مرگ ریسه تضمین کند.
- **تحلیل فیلدها:**
  - `tid`: شناسه پردازه یا ریسه فرزند.
  - `exit_status`: کد وضعیت خروج (ارسال‌شده به `exit()` یا `-1` در صورت کشته شدن توسط هسته).
  - `waited`: پرچم بولی که نشان می‌دهد آیا والد قبلاً روی این فرزند منتظر شده است یا خیر (جلوگیری از `wait-twice`).
  - `exited`: نشان‌دهنده اتمام تابع `process_exit()` توسط فرزند.
  - `load_success`: پرچم بولی تنظیم‌شده توسط فرزند در پایان فراخوانی `load()`.
  - `ref_count`: شمارنده ارجاع که با مقدار ۲ مقداردهی می‌شود (یک ارجاع در دست والد و یک ارجاع در دست فرزند).
  - `ref_lock`: قفل همگام‌سازی برای کاهش امن `ref_count`.
  - `load_sema`: سمافور همگام‌سازی بارگذاری (مقدار اولیه ۰)؛ فرزند پس از پایان `load()` آن را بالا (`sema_up`) می‌برد.
  - `wait_sema`: سمافور همگام‌سازی خروج (مقدار اولیه ۰)؛ فرزند در انتهای `process_exit()` آن را بالا می‌برد.
  - `elem`: عنصر پیوندی جهت درج در لیست فرزندان والد (`cur->children`).
- **اثر اجرایی:** یک لنگرگاه حافظه‌ای همگام و ایمن در برابر شرایط رقابتی میان دو ریسه مستقل فراهم می‌آورد.

---

#### ۲.۱.۲ فیلدهای مدیریت پردازه در `struct thread` (خطوط ۱۲۶ تا ۱۳۲)
```c
#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint32_t *pagedir;                  /**< Page directory. */
    struct list children;               /**< List of children (struct child_status). */
    struct child_status *cp;            /**< Child status tracking this thread. */
    int exit_status;                    /**< Exit status. */
#endif
```
- **شماره خطوط:** خطوط ۱۲۶ تا ۱۳۲ در فایل `src/threads/thread.h`.
- **علت ضرورت:** 
  - `children`: سرلیست برای نگهداری تمام فرزندان مستقیم پردازه جاری.
  - `cp`: اشاره‌گر ریسه جاری به ساختار وضعیت مشترک خود تا بتواند کد خروج را ثبت کرده و سمافورها را آزاد کند.
  - `exit_status`: کد خروج ذخیره‌شده ریسه.
- **اثر اجرایی:** دسترسی سریع پردازه جاری به ساختار وضعیت خود (`cur->cp`) و لیست فرزندانش (`cur->children`).

---

### ۲.۲ فایل: `src/threads/thread.c`

#### ۲.۲.۱ افزودن هدر تخصیص حافظه (خطوط ۱۵ تا ۱۸)
```c
#ifdef USERPROG
#include "userprog/process.h"
#include "threads/malloc.h"
#endif
```
- **شماره خطوط:** خطوط ۱۵ تا ۱۸ در فایل `src/threads/thread.c`.
- **علت ضرورت:** فراهم آوردن تابع `free()` برای آزادسازی بلوک حافظه هیپ در صورت رسیدن شمارنده ارجاع به صفر.

---

#### ۲.۲.۲ مقداردهی فیلدهای پردازه در `init_thread()` (خطوط ۶۷۷ تا ۶۸۲)
```c
#ifdef USERPROG
  t->pagedir = NULL;
  list_init (&t->children);
  t->cp = NULL;
  t->exit_status = -1;
#endif
```
- **شماره خطوط:** خطوط ۶۷۷ تا ۶۸۲ در فایل `src/threads/thread.c`.
- **علت ضرورت:** مقداردهی اولیه لیست فرزندان و صفر کردن اشاره‌گرها تا هیچ ریسه‌ای با مقادیر تصادفی حافظه کار نکند.

---

#### ۲.۲.۳ تابع کمکی آزادسازی مبتنی بر شمارش ارجاع: `release_child_status()` (خطوط ۸۰۳ تا ۸۲۰)
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
- **شماره خطوط:** خطوط ۸۰۳ تا ۸۲۰ در فایل `src/threads/thread.c`.
- **علت ضرورت:** تضمین آزادسازی حافظه بدون ایجاد اشاره‌گر آویزان؛ نه والد به تنهایی و نه فرزند به تنهایی نمی‌توانند بدون اطلاع از وضعیت دیگری حافظه را آزاد کنند.
- **اثر اجرایی:** تحت حفاظت قفل، `ref_count` کاهش می‌یابد. اگر به ۰ برسد (یعنی هر دو طرف کار خود را به پایان رسانده‌اند)، حافظه بلافاصله با `free(cp)` به سیستم بازگردانده می‌شود.

---

### ۲.۳ فایل: `src/userprog/process.c`

#### ۲.۳.۱ ساختار ارسال آرگومان به تابع فرزند (خطوط ۲۲ تا ۲۶)
```c
struct exec_args
  {
    char *fn_copy;
    struct child_status *cp;
  };
```
- **شماره خطوط:** خطوط ۲۲ تا ۲۶ در فایل `src/userprog/process.c`.
- **علت ضرورت:** انتقال همزمان اشاره‌گر صفحه دستورات خط فرمان و اشاره‌گر ساختار وضعیت به تابع `start_process` از طریق تک‌آرگومان `void *aux`.
- **اثر اجرایی:** این ساختار روی پشته والد قرار می‌گیرد و چون والد تا زمان اتمام استفاده فرزند روی `load_sema` مسدود است، بدون نیاز به تخصیص هیپ کاملاً معتبر باقی می‌ماند.

---

#### ۲.۳.۲ همگام‌سازی در `process_execute()` (خطوط ۳۷ تا ۱۰۵)
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
- **شماره خطوط:** خطوط ۳۷ تا ۱۰۵ در فایل `src/userprog/process.c`.
- **علت ضرورت:** 
  1. در پینتوس پایه، یک صفحه کامل ۴ کیلوبایتی فقط برای استخراج نام برنامه تخصیص داده می‌شد که در آزمون `multi-oom` موجب اتمام زودهنگام صفحات می‌گردید. اکنون نام برنامه با بافر محلی روی پشته استخراج می‌شود.
  2. والد بدون اطلاع از نتیجه بارگذاری بازمی‌گشت. با افزودن `sema_down(&cp->load_sema)`، والد دقیقاً تا پایان تلاش بارگذاری فرزند به حالت انتظار می‌رود.
- **اثر اجرایی:** اگر بارگذاری با شکست مواجه شود، رکورد از لیست فرزندان حذف شده، ارجاع والد آزاد می‌شود و مقدار `-1` بازگردانده می‌شود.

---

#### ۲.۳.۳ راه‌اندازی پردازه در `start_process()` (خطوط ۱۰۹ تا ۱۶۳)
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
- **شماره خطوط:** خطوط ۱۰۹ تا ۱۶۳ در فایل `src/userprog/process.c`.
- **علت ضرورت:** فرزند باید بلافاصله اشاره‌گر `cur->cp` را به ساختار وضعیت مشترک متصل کند، صفحه خط فرمان را پس از تجزیه آرگومان‌ها آزاد کند، وضعیت موفقیت یا شکست بارگذاری را ثبت نماید و با `sema_up(&cp->load_sema)` والد را بیدار کند.
- **اثر اجرایی:** والد بدون قفل شدن بیدار می‌شود؛ در صورت شکست، فرزند با `-1` خارج شده و در صورت موفقیت به Ring 3 پرش می‌کند.

---

#### ۲.۳.۴ همگام‌سازی انتظار در `process_wait()` (خطوط ۱۷۱ تا ۱۹۹)
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
- **شماره خطوط:** خطوط ۱۷۱ تا ۱۹۹ در فایل `src/userprog/process.c`.
- **علت ضرورت:** پیاده‌سازی صحیح سازوکار `wait`؛ اگر پردازه فرزند یافت نشود یا قبلاً روی آن `wait` صورت گرفته باشد، مقدار `-1` بازگردانده می‌شود.
- **اثر اجرایی:** والد روی `wait_sema` به خواب می‌رود تا فرزند در خروج خود سمافور را بالا ببرد. والد کد وضعیت خروج را برداشته، ارجاع خود را آزاد می‌کند و کد وضعیت را تحویل می‌دهد.

---

#### ۲.۳.۵ پاکسازی منابع در `process_exit()` (خطوط ۲۰۲ تا ۲۴۳)
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
- **شماره خطوط:** خطوط ۲۰۲ تا ۲۴۳ در فایل `src/userprog/process.c`.
- **علت ضرورت:** چاپ پیام رسمی خروج، ثبت نهایی وضعیت، بیدار کردن والد منتظر با `sema_up(&cur->cp->wait_sema)`، و آزادسازی کلیه فرزندان یتیم باقی‌مانده در لیست `cur->children`.
- **اثر اجرایی:** پاکسازی کامل جدول صفحه و آزادسازی تمامی صفحات فیزیکی کاربر، بدون برجا ماندن حتی یک نشت حافظه در هیپ یا استخر صفحات.

---

### ۲.۴ فایل: `src/userprog/syscall.c`

#### ۲.۴.۱ توزیع‌کننده فراخوانی‌های سیستمی (خطوط ۹۳ تا ۱۶۱)
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
- **شماره خطوط:** خطوط ۹۳ تا ۱۶۱ در فایل `src/userprog/syscall.c`.
- **علت ضرورت:**
  - `SYS_HALT`: خاموش کردن سخت‌افزار ماشین شبیه‌ساز با `shutdown_power_off()`.
  - `SYS_EXIT`: اعتبارسنجی آرگومان وضعیت روی پشته، تنظیم `cur->exit_status` و فراخوانی `thread_exit()`.
  - `SYS_EXEC`: اعتبارسنجی اشاره‌گر و تمامی کاراکترهای رشته دستور تا رسیدن به `\0` توسط `validate_user_string()` و اجرای برنامه با `process_execute()`.
  - `SYS_WAIT`: اعتبارسنجی شناسه پردازه روی پشته و فراخوانی `process_wait()`.
  - `SYS_OPEN` و `SYS_CLOSE`: در فاز ۳ هنوز سیستم فایل پیاده‌سازی نشده است؛ آزمون `multi-oom` تابع `open()` را برای مصرف منابع تا ۱۲۶ بار فراخوانی می‌کند و در یک حالت خطای آدرس `PHYS_BASE` را می‌آزماید. این بخش با اعتبارسنجی رشته مانع از نفوذ به هسته شده و با بازگرداندن `-1` شرایط شکست امن را بدون کرش پردازه فراهم می‌سازد.

---

## ۳. آموزش مفاهیم سیستم‌عامل مرتبط با این فاز (Operating System Concepts & Deep-Dive Study Guide)

### ۳.۱ ساختار درختی پردازه‌ها و مدل چرخه حیات
در سیستم‌های عامل، پردازه‌ها در یک سلسله‌مراتب درختی سازمان‌دهی می‌شوند:
- در استاندارد پوزیکس (POSIX)، ایجاد پردازه با تکثیر پردازه جاری توسط `fork()` آغاز شده و سپس با جایگزینی فضای آدرس توسط `exec()` تکمیل می‌گردد.
- در سیستم پینتوس (مشابه ویندوز `CreateProcess` یا مدل `spawn`)، ایجاد پردازه و بارگذاری تصویر اجرایی در قالب یک عملیات یکپارچه توسط `exec(cmd_line)` صورت می‌پذیرد.

```
                      [ پردازه ریشه (PID 1) ]
                            /         \
                           /           \
                 [ فرزند اول (PID 2) ]  [ فرزند دوم (PID 3) ]
                       /
                      /
            [ نوه (PID 4) ]
```

قوانین حاکم بر این سلسله‌مراتب عبارتند از:
1. **قلمرو انتظار (Scope of Wait):** هر پردازه تنها می‌تواند بر روی **فرزندان مستقیم** خود منتظر بماند. هیچ پردازه‌ای حق انتظار روی خواهر، برادر، والد یا نوه‌های خود را ندارد.
2. **دریافت قطعی کد وضعیت:** وضعیت خروج پردازه فرزند باید تا زمانی که والد آن را دریافت نکرده در حافظه هسته پایدار بماند.
3. **عدم امکان انتظار مکرر:** کد خروج یک فرزند تنها یک بار قابل دریافت است؛ فراخوانی مجدد `wait` روی همان فرزند باید شکست بخورد (`-1`).

---

### ۳.۲ معماری پردازه‌های زامبی (Zombie) و شمارش ارجاع (Reference Counting)
هنگامی که یک پردازه فرزند به کار خود پایان می‌دهد، سیستم‌عامل با یک دوگانگی کلاسیک مواجه می‌شود:
- اگر هسته بلافاصله تمامی اطلاعات و ساختارهای فرزند را نابود کند، کد خروج آن از بین رفته و والد در آینده نمی‌تواند از نتیجه کار فرزند باخبر شود.
- اگر هسته ریسه و پشته فرزند را در حافظه نگه دارد، پردازه‌های مرده (زامبی‌ها) منابع حافظه سیستم را اشغال می‌کنند و در برنامه‌های بازگشتی نظیر `multi-oom` حافظه به سرعت تمام می‌شود.

#### راه‌حل شمارش ارجاع در پینتوس
پینتوس **زمینه اجرایی ریسه** (صفحه ۴ کیلوبایتی `struct thread` و پشته هسته) را از **ساختار رهگیری وضعیت خروج** (`struct child_status` روی هیپ) کاملاً تفکیک می‌کند:

```
+--------------------------------------------------------------------------------+
|                         struct child_status (روی هیپ)                          |
|  - tid: 42                                                                     |
|  - exit_status: 81                                                             |
|  - ref_count: 2  <---+ ارجاع اول (در دست والد)        <---+ ارجاع دوم (در دست فرزند)|
|  - load_sema: [0]    |                                   |                     |
|  - wait_sema: [0]    |                                   |                     |
+----------------------|-----------------------------------|---------------------+
                       |                                   |
            ساختار ریسه والد (struct thread)       ساختار ریسه فرزند (struct thread)
          +--------------------+                  +--------------------+
          | children: list     |                  | cp: pointer        |
          +--------------------+                  +--------------------+
```

1. **حالت اول: فرزند زودتر از والد خاتمه می‌یابد (Scenario A):**
   - فرزند تابع `process_exit()` را اجرا می‌کند.
   - وضعیت خروج را در `cp->exit_status` نوشته و با `sema_up(&cp->wait_sema)` سیگنال پایان می‌دهد.
   - فرزند تابع `release_child_status(cp)` را صدا می‌زند؛ شمارنده `ref_count` از ۲ به ۱ کاهش می‌یابد.
   - صفحه ریسه و پشته فرزند بلافاصله توسط زمان‌بند در `thread_schedule_tail()` به استخر حافظه بازمی‌گردد.
   - ساختار `struct child_status` بر روی هیپ باقی می‌ماند (`ref_count == 1`).
   - بعداً، والد تابع `process_wait()` را صدا می‌زند. سمافور بلافاصله عبور کرده، والد کد خروج را برمی‌دارد و `release_child_status(cp)` را فراخوانی می‌کند.
   - شمارنده `ref_count` از ۱ به ۰ رسیده و والد حافظه هیپ را با `free(cp)` آزاد می‌سازد.

2. **حالت دوم: والد زودتر از فرزند خاتمه می‌یابد (Scenario B - پردازه‌های یتیم):**
   - والد در حال خروج است در حالی که فرزندان هنوز در حال اجرا هستند یا والد هرگز برای آن‌ها `wait` نکرده است.
   - در `process_exit()`، والد لیست `cur->children` را پیمایش کرده و برای تک‌تک آن‌ها `release_child_status(cp)` را فراخوانی می‌کند.
   - شمارنده `ref_count` برای هر فرزند از ۲ به ۱ کاهش می‌یابد.
   - زمانی که بعداً هر فرزند به پایان کار خود می‌رسد، در `process_exit()` خود تابع `release_child_status(cur->cp)` را صدا می‌زند.
   - شمارنده `ref_count` از ۱ به ۰ رسیده و آخرین فرزند خارج‌شده حافظه هیپ را با `free(cp)` آزاد می‌کند.

هیچ‌گونه نشت حافظه‌ای در هیچ‌یک از دو حالت رخ نمی‌دهد!

---

### ۳.۳ نمودارهای زمان‌بندی و جریان کنترل همگام‌سازی

#### نمودار ۱: خط زمان همگام‌سازی در `process_execute()`
```
ریسه والد (پردازه A)                                  ریسه فرزند (پردازه B)
-------------------                                  --------------------
process_execute("child")
  |-- تخصیص صفحه fn_copy
  |-- تخصیص ساختار هیپ child_status (cp)
  |-- فراخوانی thread_create("child", ..., start_process)
  |     |-- ایجاد ریسه B
  |     +-- انتقال به صف آماده اجرا
  |-- افزودن به لیست فرزندان: list_push_back(&children, &cp->elem)
  |-- مسدود شدن والد: sema_down(&cp->load_sema) [BLOCKS]
  .                                                  start_process(aux)
  .                                                    |-- اتصال وضعیت: cur->cp = cp
  .                                                    |-- تجزیه آرگومان‌ها
  .                                                    |-- بارگذاری: success = load(...)
  .                                                    |-- آزادسازی صفحه: palloc_free_page(fn_copy)
  .                                                    |-- ثبت موفقیت: cp->load_success = success
  .                                                    |-- بیدار کردن والد: sema_up(&cp->load_sema) ---+
  |<---------------------------------------------------+-----------------------------------------------+
  |-- بررسی نتیجه: if (!cp->load_success) -> return -1 |
  |-- بازگرداندن tid به کاربر                         +-- در صورت شکست بارگذاری: thread_exit()
  |                                                    +-- در صورت موفقیت: پرش به Ring 3 با intr_exit
```

#### نمودار ۲: خط زمان همگام‌سازی در `process_wait()` و `process_exit()`
```
ریسه والد                                            ریسه فرزند
--------                                            ----------
process_wait(child_tid)                              کد کاربر در حال اجرا...
  |-- جستجوی child_tid در لیست فرزندان                |
  |-- اگر قبلاً منتظر شده: return -1                  |
  |-- تنظیم پرچم: cp->waited = true                  |-- فراخوانی exit(81) یا رخداد خطا
  |-- مسدود شدن روی خروج: sema_down(&cp->wait_sema)   |-- فراخوانی thread_exit()
  .   [BLOCKS]                                         |-- فراخوانی process_exit()
  .                                                    |     |-- چاپ پیام: printf("child: exit(81)")
  .                                                    |     |-- ثبت کد: cur->cp->exit_status = 81
  .                                                    |     |-- بیدار کردن والد: sema_up(&wait_sema) ---+
  |<---------------------------------------------------+-----+-------------------------------------------+
  |-- خواندن کد خروج: status = cp->exit_status         |     |-- آزادسازی ارجاع: release_child_status(cp)
  |-- حذف از لیست: list_remove(&cp->elem)              |     |   (کاهش ref_count از ۲ به ۱)
  |-- آزادسازی ارجاع: release_child_status(cp)         |     +-- نابودی جدول صفحه: pagedir_destroy()
  |   (کاهش ref_count از ۱ به ۰ و فراخوانی free(cp))   +-- زمان‌بند صفحه ریسه را آزاد می‌کند
  +-- بازگرداندن کد وضعیت خروج (81)
```

---

### ۳.۴ خطاهای رایج و راهنمای اشکال‌زدایی (Common Pitfalls)

#### خطای ۱: تخصیص بی‌رویه صفحات حافظه در `process_execute()`
- **نشانه:** شکست آزمون `multi-oom` در عمق حدود ۱۵ با پیام خطای `FAIL: should have forked at least 30 times`.
- **علت ریشه‌ای:** تخصیص یک صفحه مجزای ۴ کیلوبایتی با `palloc_get_page(0)` فقط برای جدا کردن نام برنامه پیش از `thread_create()`. در پینتوس تنها حدود ۳۶۷ صفحه در استخر کاربر و ۳۶۷ صفحه در استخر هسته وجود دارد؛ تخصیص ۲ صفحه به ازای هر پردازه استخر را به سرعت تمام می‌کند.
- **راه‌حل:** استخراج نام برنامه با استفاده از یک آرایه محلی روی پشته (`char prog_name[16]`) مستقیماً از رشته `fn_copy`.

#### خطای ۲: آزادسازی حافظه در حین نگه داشتن قفل
- **نشانه:** بروز خطای Kernel Panic یا بن‌بست داخل `free()` یا `lock_acquire()`.
- **علت ریشه‌ای:** فراخوانی `free(cp)` در حالی که قفل `&cp->ref_lock` هنوز آزاد نشده است؛ حافظه آزاد شده در هیپ هنوز حاوی متغیرهای قفل فعال است.
- **راه‌حل:** تعیین وضعیت آزادسازی در متغیر محلی `bool should_free = (cp->ref_count == 0);`، آزاد کردن قفل با `lock_release(&cp->ref_lock)`، و سپس فراخوانی مشروط `free(cp)`.

#### خطای ۳: ایجاد بن‌بست در صورت شکست بارگذاری فرزند
- **نشانه:** قفل شدن همیشگی سیستم‌عامل در آزمون `exec-missing`.
- **علت ریشه‌ای:** در تابع `start_process()` اگر `load()` مقدار `false` برگرداند، شاخه خروج زودهنگام `thread_exit()` را پیش از صدا زدن `sema_up(&cp->load_sema)` فراخوانی کند؛ والد برای همیشه در `sema_down` معلق می‌ماند.
- **راه‌حل:** تنظیم قطعی مقدار `cp->load_success = success;` و فراخوانی بی‌قیدوشرط `sema_up(&cp->load_sema)` قبل از هرگونه بررسی شرط شکست.

#### خطای ۴: نشت حافظه ناشی از فرزندان یتیم در `multi-oom`
- **نشانه:** آزمون `multi-oom` در تکرار اول موفق می‌شود اما در تکرار دوم با خطای عدم تطابق عمق شکست می‌خورد.
- **علت ریشه‌ای:** عدم پیمایش و آزادسازی فرزندان باقی‌مانده در لیست `cur->children` در زمان خروج والد در `process_exit()`.
- **راه‌حل:** پیمایش کامل لیست فرزندان در `process_exit()` و فراخوانی `release_child_status(cp)` به ازای تمامی عناصر تا شمارنده ارجاع آن‌ها برای خروج فرزند آماده باشد.
