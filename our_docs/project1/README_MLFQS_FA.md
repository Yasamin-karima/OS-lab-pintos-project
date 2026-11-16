# گزارش جامع پروژه ۱ پینتوس: پیاده‌سازی زمان‌بند صف چندسطحی فیدبک‌دار (MLFQS / 4.4BSD Scheduler)

## ۱. مقدمه و اهداف پروژه

در فاز سوم و نهایی پروژه ۱ پینتوس (Threads)، هدف پیاده‌سازی **زمان‌بند صف چندسطحی فیدبک‌دار (Multi-Level Feedback Queue Scheduler - MLFQS)** است که از ساختار زمان‌بند سیستم‌عامل معروف 4.4BSD الگوبرداری شده است.

### ۱.۱ محدودیت‌های زمان‌بندی اولویت اکید و اهدای اولویت (Priority Donation)
در فاز پیشین، زمان‌بندی مبتنی بر اولویت به همراه اهدای اولویت پیاده‌سازی شد. با وجود حل مشکل وارونگی اولویت (Priority Inversion)، آن رویکرد محدودیت‌های اساسی داشت:
1. **نیاز به تعیین دستی اولویت‌ها:** اولویت هر ریسمان باید توسط برنامه‌نویس به صورت صریح و دستی تعیین می‌شد. در محیط واقعی، برنامه‌ها می‌توانستند با تعریف ناعادلانه اولویت بالا، منابع سیستم را تصاحب کنند.
2. **عدم انطباق با رفتار پردازه‌ها:** سیستم هیچ معیاری برای پاداش دادن به ریسمان‌های تعاملی (I/O-bound که عمدتاً خوابیده و منتظر کاربر هستند) و جریمه ریسمان‌های محاسباتی سنگین (CPU-bound) نداشت.
3. **پیچیدگی سربار همگام‌سازی:** سازوکار اهدای اولویت نیازمند پیمایش مداوم گراف مالکان قفل، ردگیری لیست قفل‌های تصاحب‌شده و بازسنجی‌های مکرر بود.

### ۱.۲ فلسفه زمان‌بند MLFQS
زمان‌بند 4.4BSD اهدای اولویت دستی را حذف کرده و اولویت ریسمان‌ها را به صورت **پویا و کاملاً ریاضی** تعیین می‌کند:
* **بازسنجی دوره‌ای اولویت:** اولویت هر ریسمان هر ۴ تیک بر اساس میزان کارکرد اخیر پردازنده (`recent_cpu`) و میزان سازگاری (`nice`) محاسبه می‌شود:
  $$\text{priority} = \text{PRI\_MAX} - \left(\frac{\text{recent\_cpu}}{4}\right) - (\text{nice} \times 2)$$
* **میرایی نمایی کارکرد پردازنده:** مقدار `recent_cpu` هر یک ثانیه دچار زوال نمایی (Decay) می‌شود تا بار پردازشی گذشته، مانع از اجرای مجدد ریسمان در آینده نشود.
* **محاسبه میانگین بار سیستم (`load_avg`):** تخمینی متحرک از میانگین تعداد ریسمان‌های آماده و در حال اجرا در طول ۶۰ ثانیه گذشته نگهداری می‌شود.
* **جداسازی کامل زمان‌بندها:** هنگامی که پرچم `-mlfqs` فعال است، تمامی بخش‌های اهدای اولویت و انتساب دستی اولویت غیرفعال شده و روابط ریاضی کنترل کامل زمان‌بندی را در دست می‌گیرند.

---

## ۲. موتور محاسبات ریاضی ممیز ثابت ۱۷.۱۴ (17.14 Fixed-Point Engine)

### ۲.۱ چرا استفاده از ممیز شناور (Floating-Point) در هسته ممنوع است؟
در معماری x86 و پردازنده‌های مدرن، محاسبات اعشاری و شناور نیازمند واحدهای FPU و ثبات‌های چندرسانه‌ای (مانند SSE) هستند:
* در کرنل پینتوس، دستورات ممیز شناور از طریق پرچم کامپایلر (`-mno-sse`) غیرفعال شده‌اند.
* در زمان تعویض متن (Context Switch)، ذخیره و بازیابی ده‌ها بایت از ثبات‌های FPU سربار سنگینی به سیستم تحمیل می‌کند، از این رو پینتوس این ثبات‌ها را در پشته هسته ذخیره نمی‌کند.
* استفاده مستقیم از `float` یا `double` در کدهای کرنل به خطای کامپایلر یا رفتارهای پیش‌بینی‌نشده در زمان اجرا منجر می‌شود.

برای حل این چالش، محاسبات بر پایه **حساب ممیز ثابت (Fixed-Point Arithmetic)** پیاده‌سازی شده است:
* نمایش داده: عدد صحیح علامت‌دار ۳۲ بیتی (`int32_t`) شامل:
  * ۱ بیت علامت (Sign)
  * ۱۷ بیت بخش صحیح (Integer)
  * ۱۴ بیت بخش اعشاری (Fractional)
* ضریب مقیاس (Scale Factor): $f = 2^{14} = 16384$. مقدار حقیقی $x$ در حافظه به شکل $x \times 16384$ ذخیره می‌شود.

### ۲.۲ فایل پیاده‌سازی: [`src/threads/fixed-point.h`](file:///home/yasamin/pintos/src/threads/fixed-point.h)

یک فایل هدر اختصاصی شامل ماکروهای ریاضی ممیز ثابت پیاده‌سازی شد:

```c
#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

#include <stdint.h>

#define FP_SHIFT 14
#define FP_F (1 << FP_SHIFT)       /* 16384 */
#define FP_HALF (FP_F / 2)         /* 8192 برای گرد کردن به نزدیک‌ترین صحیح */

/* تبدیل عدد صحیح به ممیز ثابت */
#define INT_TO_FP(n) ((int) ((n) * FP_F))

/* تبدیل ممیز ثابت به صحیح با حذف اعشار (گرد به سمت صفر) */
#define FP_TO_INT_ZERO(x) ((int) ((x) / FP_F))
#define FP_TO_INT(x) FP_TO_INT_ZERO(x)

/* تبدیل ممیز ثابت به صحیح با گرد کردن به نزدیک‌ترین عدد */
#define FP_TO_INT_ROUND(x) ((int) ((x) >= 0 ? (((x) + FP_HALF) / FP_F) : (((x) - FP_HALF) / FP_F)))

/* جمع دو عدد ممیز ثابت */
#define FP_ADD(x, y) ((x) + (y))

/* تفریق دو عدد ممیز ثابت */
#define FP_SUB(x, y) ((x) - (y))

/* جمع ممیز ثابت با عدد صحیح */
#define FP_ADD_INT(x, n) ((x) + ((n) * FP_F))

/* تفریق عدد صحیح از ممیز ثابت */
#define FP_SUB_INT(x, n) ((x) - ((n) * FP_F))

/* ضرب دو عدد ممیز ثابت با پیشگیری از سرریز */
#define FP_MUL(x, y) ((int) (((int64_t) (x)) * (y) / FP_F))

/* ضرب ممیز ثابت در عدد صحیح */
#define FP_MUL_INT(x, n) ((x) * (n))

/* تقسیم دو عدد ممیز ثابت با پیشگیری از سرریز */
#define FP_DIV(x, y) ((int) (((int64_t) (x)) * FP_F / (y)))

/* تقسیم ممیز ثابت بر عدد صحیح */
#define FP_DIV_INT(x, n) ((x) / (n))

#endif /* threads/fixed-point.h */
```

### ۲.۳ نکات و ملاحظات ریاضی ماکروها
1. **پیشگیری از سرریز با کستینگ ۶۴ بیتی (`int64_t`):**
   ضرب دو عدد ممیز ثابت مقیاس را به $f^2 = 2^{28}$ افزایش می‌دهد که می‌تواند از محدوده اعداد علامت‌دار ۳۲ بیتی ($\approx 2 \times 10^9$) فراتر برود. با تبدیل موقت به `int64_t`، ضرب با دقت کامل انجام شده و پس از تقسیم بر $f$ به ۳۲ بیت بازگردانده می‌شود.
2. **گرد کردن به نزدیک‌ترین عدد صحیح (`FP_TO_INT_ROUND`):**
   طبق سند پینتوس، اولویت‌ها و خروجی‌های آماری باید به نزدیک‌ترین عدد صحیح گرد شوند. با افزودن $f/2$ ($8192$) برای اعداد مثبت و کسر آن برای اعداد منفی پیش از تقسیم، گرد کردن به درستی صورت می‌گیرد.

---

## ۳. تغییرات ساختار داده‌ها و مقداردهی اولیه

### ۳.۱ تغییرات `struct thread`
فایل: [`src/threads/thread.h`](file:///home/yasamin/pintos/src/threads/thread.h#L100-L108)

```c
struct thread
  {
    /* ... سایر فیلدهای قبلی ... */
    int base_priority;                  /**< اولویت پایه */
    struct list locks_held;             /**< قفل‌های در اختیار */
    struct lock *lock_waiting_on;       /**< قفلی که منتظر آن است */

    int nice;                           /**< میزان سازگاری (-20 تا 20) */
    int recent_cpu;                     /**< مصرف اخیر پردازنده (ممیز ثابت) */
#ifdef USERPROG
    uint32_t *pagedir;
#endif
    unsigned magic;
  };
```

* `nice`: میزان گذشت یا سازگاری ریسمان نسبت به سایر ریسمان‌ها است که عددی بین ۲۰- (حداقل گذشت، بیشترین سهم پردازنده) تا ۲۰ (حداکثر گذشت، کمترین سهم پردازنده) می‌باشد. مقدار پیش‌فرض آن ۰ است.
* `recent_cpu`: برآوردگر ممیز ثابت ۱۷.۱۴ از میزان مصرف پردازنده در زمان‌های اخیر.

### ۳.۲ متغیر سراسری میانگین بار سیستم
فایل‌ها: [`src/threads/thread.h`](file:///home/yasamin/pintos/src/threads/thread.h#L118-L121) و [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L65-L68)

```c
/* میانگین بار سیستم به فرمت ممیز ثابت ۱۷.۱۴ */
int load_avg;
```

### ۳.۳ مقداردهی در زمان بوت سیستم (`thread_init`)
فایل: [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L92-L115)

```c
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);
  list_init (&sleep_list);

  load_avg = 0;  /* میانگین بار سیستم در آغاز بوت صفر است */

  /* مقداردهی ریسمان اولیه اصلی */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
  initial_thread->nice = 0;
  initial_thread->recent_cpu = 0;
  if (thread_mlfqs)
    thread_calculate_priority (initial_thread);
}
```

### ۳.۴ ارث‌بری مقادیر در زمان ساخت ریسمان (`thread_create`)
فایل: [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L195-L215)

هنگام ایجاد یک ریسمان جدید در حالت MLFQS، ریسمان فرزند باید مقادیر `nice` و `recent_cpu` را از والد خود به ارث ببرد و اولویت آن به صورت پویا محاسبه شود:
```c
  /* مقداردهی اولیه ریسمان */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  if (thread_mlfqs)
    {
      if (strcmp (name, "idle") == 0)
        {
          t->nice = 0;
          t->recent_cpu = 0;
          t->priority = PRI_MIN;
        }
      else
        {
          t->nice = thread_current ()->nice;
          t->recent_cpu = thread_current ()->recent_cpu;
          thread_calculate_priority (t);
        }
    }
```

**ملاحظه بسیار مهم درباره ریسمان `idle`:**
ریسمان بیکار (`idle`) هرگز نباید اولویتش محاسبه یا تغییر یابد؛ اولویت آن همواره باید روی `PRI_MIN` (۰) بماند و هرگز نباید مقدار `recent_cpu` بگیرد یا در شمارش ریسمان‌های آماده (`ready_threads`) محاسبه شود.

---

## ۴. روابط ریاضی و الگوریتم‌های محاسباتی MLFQS

### ۴.۱ محاسبه اولویت پویا (Priority Formula)
فایل: [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L455-L470)

فرمول اولویت طبق استاندارد 4.4BSD عبارت است از:
$$\text{priority} = \text{PRI\_MAX} - \left(\frac{\text{recent\_cpu}}{4}\right) - (\text{nice} \times 2)$$

کد پیاده‌سازی‌شده:
```c
void
thread_calculate_priority (struct thread *t)
{
  if (t == idle_thread)
    return;

  int p = PRI_MAX - FP_TO_INT_ROUND (t->recent_cpu / 4) - (t->nice * 2);
  if (p > PRI_MAX)
    p = PRI_MAX;
  if (p < PRI_MIN)
    p = PRI_MIN;

  t->priority = p;
}
```
**نکات کلیدی:**
* تقسیم `recent_cpu / 4` به دلیل بخش‌پذیر بودن ۱۶۳۸۴ بر ۴ ($16384 / 4 = 4096$) مقیاس ممیز ثابت را حفظ می‌کند.
* با ماکروی `FP_TO_INT_ROUND` به نزدیک‌ترین عدد صحیح تبدیل می‌شود.
* مقدار نهایی بین `PRI_MIN` (۰) و `PRI_MAX` (۶۳) برش داده (Clamp) می‌شود.

---

### ۴.۲ میانگین بار سیستم (`load_avg`)
فایل: [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L480-L510)

میانگین بار سیستم تخمینی نمایی از تعداد ریسمان‌های آماده اجرای سیستم در طول ۶۰ ثانیه گذشته است:
$$\text{load\_avg} = \left(\frac{59}{60}\right) \times \text{load\_avg} + \left(\frac{1}{60}\right) \times \text{ready\_threads}$$

تعریف دقیق `ready_threads`:
تعداد ریسمان‌هایی که در صف آماده هستند به علاوه ریسمان در حال اجرا (به شرطی که ریسمان بیکار نباشد):
$$\text{ready\_threads} = \text{list\_size}(\&\text{ready\_list}) + (\text{thread\_current}() \ne \text{idle\_thread} \; ? \; 1 : 0)$$

پیاده‌سازی در ممیز ثابت:
```c
int ready_threads = list_size (&ready_list);
if (thread_current () != idle_thread)
  ready_threads++;

load_avg = (int) (((int64_t) 59 * load_avg + (int64_t) ready_threads * FP_F) / 60);
```
محاسبه یکپارچه صورت کسری و انجام یک تقسیم واحد ۶۴ بیتی مانع از انباشت خطای برش در محاسبات پی‌درپی می‌گردد.

---

### ۴.۳ برآورد مصرف اخیر پردازنده (`recent_cpu`)
این مقدار میزان زمانی را که یک ریسمان اخیراً پردازنده را در اختیار داشته اندازه‌گیری می‌کند:
1. **افزایش در هر تیک:** در هر تیک وقفه تایمر، اگر ریسمان در حال اجرا ریسمان `idle` نباشد، مقدار `recent_cpu` آن ۱ واحد (در ممیز ثابت: ۱۶۳۸۴+) افزایش می‌یابد.
2. **میرایی یک بار در ثانیه (هر ۱۰۰ تیک):** برای **تمامی ریسمان‌های سیستم** (شامل در حال اجرا، آماده و مسدود)، مقدار کارکرد بر اساس فرمول زیر کاهش می‌یابد:
   $$\text{recent\_cpu} = \left(\frac{2 \times \text{load\_avg}}{2 \times \text{load\_avg} + 1}\right) \times \text{recent\_cpu} + \text{nice}$$

کد پیاده‌سازی‌شده:
```c
int twice_load = 2 * load_avg;
int coef = FP_DIV (twice_load, twice_load + FP_F);

struct list_elem *e;
for (e = list_begin (&all_list); e != list_end (&all_list); e = list_next (e))
  {
    struct thread *t = list_entry (e, struct thread, allelem);
    if (t != idle_thread)
      t->recent_cpu = FP_ADD_INT (FP_MUL (coef, t->recent_cpu), t->nice);
  }
```
**بهینه‌سازی کارایی:** ضریب $\frac{2 \times \text{load\_avg}}{2 \times \text{load\_avg} + 1}$ برای همه ریسمان‌ها یکسان است؛ با محاسبه یک‌باره `coef` پیش از حلقه، از صدها تقسیم سنگین ۶۴ بیتی زائد در هر ثانیه جلوگیری می‌شود.

---

## ۵. به‌روزرسانی‌ها در وقفه تایمر و قواعد زمان‌بندی

فایل: [`src/devices/timer.c`](file:///home/yasamin/pintos/src/devices/timer.c#L175-L225)

قلب تپنده مدیریت زمان‌بندی MLFQS در تابع وقفه تایمر `timer_interrupt()` قرار دارد:

```c
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
          if (thread_current () == thread_idle () || t->priority > thread_current ()->priority)
            yield_needed = true;
        }
      else
        {
          e = list_next (e);
        }
    }

  if (thread_mlfqs)
    {
      /* ۱. در هر تیک: افزایش recent_cpu ریسمان جاری */
      thread_mlfqs_tick ();

      /* ۲. در هر ۱۰۰ تیک (۱ ثانیه): به‌روزرسانی بار سیستم و میرایی recent_cpu */
      if (ticks % TIMER_FREQ == 0)
        thread_mlfqs_update_load_avg_and_recent_cpu ();

      /* ۳. در هر ۴ تیک: محاسبه مجدد اولویت همه ریسمان‌ها و مرتب‌سازی صف آماده */
      if (ticks % 4 == 0)
        {
          thread_mlfqs_update_priorities ();
          if (thread_mlfqs_should_yield ())
            yield_needed = true;
        }
    }

  if (yield_needed)
    intr_yield_on_return ();
}
```

### ملاحظات و تقدم‌های حیاتی در وقفه تایمر:
1. **تقدم بیدار کردن ریسمان‌های خوابیده:**
   ریسمان‌هایی که مهلت خواب آن‌ها در تیک فعلی سررسیده است، باید **قبل** از محاسبه بار سیستم از خواب بیدار شده و به `ready_list` منتقل شوند. اگر بعد از محاسبه بار بیدار می‌شدند، در شمارش `ready_threads` جا می‌افتادند و میانگین بار کمتر از واقعیت گزارش می‌شد.
2. **ترتیب تقدم در تیک ۱۰۰:**
   از آنجا که ۱۰۰ مضربی از ۴ است، در تیک ۱۰۰ هر دو عملیات یک‌ثانیه‌ای و ۴‌تیکی فعال می‌شوند. با اجرای اول `thread_mlfqs_update_load_avg_and_recent_cpu` و سپس `thread_mlfqs_update_priorities`، اولویت‌ها بر پایه مقادیر تازه میراشده `recent_cpu` محاسبه می‌گردند.
3. **مرتب‌سازی مجدد صف آماده (`ready_list`):**
   با تغییر اولویت ریسمان‌های حاضر در `ready_list`، ترتیب قبلی صف به هم می‌ریزد. فراخوانی `list_sort (&ready_list, thread_cmp_priority, NULL)` صف آماده را مجدداً به صورت نزولی مرتب می‌کند.
4. **پیش‌دستی آنی (`thread_mlfqs_should_yield`):**
   اگر اولویت ریسمان جاری کمتر از بالاترین اولویت در صف آماده شود، یا اگر پردازنده در حال اجرای ریسمان `idle` باشد در حالی که صف آماده حاوی ریسمان است، بلافاصله فلگ `yield_needed` ست شده و با `intr_yield_on_return()` تعویض متن اعمال می‌شود.

---

## ۶. جداسازی زمان‌بندها و خنثی‌سازی اهدای اولویت

برای ایزوله بودن کامل سیستم، تمامی سازوکارهای زمان‌بندی دستی قبلی تحت شرط `if (!thread_mlfqs)` قرار گرفتند.

### ۶.۱ نادیده گرفتن `thread_set_priority`
فایل: [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L368-L377)
```c
void
thread_set_priority (int new_priority) 
{
  if (thread_mlfqs)
    return;  /* در زمان‌بند MLFQS تغییر دستی اولویت کاملاً بی‌اثر است */

  struct thread *cur = thread_current ();
  cur->base_priority = new_priority;
  thread_recompute_priority (cur);
  thread_yield_if_needed ();
}
```

### ۶.۲ عبور از زنجیره اهدای اولویت در قفل‌ها (`synch.c`)
فایل: [`src/threads/synch.c`](file:///home/yasamin/pintos/src/threads/synch.c#L225-L255)
در تابع `lock_acquire()`:
```c
  struct thread *cur = thread_current ();

  /* اهدای اولویت در حالت MLFQS باید کاملاً غیرفعال باشد */
  if (!thread_mlfqs && lock->holder != NULL)
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

  lock->holder = cur;
  if (!thread_mlfqs)
    {
      cur->lock_waiting_on = NULL;
      list_push_back (&cur->locks_held, &lock->elem);
      lock->max_priority = lock_get_max_priority (lock);
    }
```

در تابع `lock_release()`:
```c
void
lock_release (struct lock *lock) 
{
  ASSERT (lock != NULL);
  ASSERT (lock_held_by_current_thread (lock));

  struct thread *cur = thread_current ();

  /* حذف از لیست قفل‌ها و بازسنجی اولویت تنها در زمان‌بند قبلی انجام می‌شود */
  if (!thread_mlfqs)
    {
      list_remove (&lock->elem);
      thread_recompute_priority (cur);
    }

  lock->holder = NULL;
  sema_up (&lock->semaphore);
}
```

### ۶.۳ توابع رابط (Getters و Setters)
فایل: [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L530-L565)
```c
void
thread_set_nice (int nice) 
{
  struct thread *cur = thread_current ();
  cur->nice = nice;
  thread_calculate_priority (cur);
  thread_yield_if_needed ();
}

int
thread_get_nice (void) 
{
  return thread_current ()->nice;
}

int
thread_get_load_avg (void) 
{
  int64_t val = (int64_t) load_avg * 100;
  return (int) (val >= 0 ? (val + FP_HALF) / FP_F : (val - FP_HALF) / FP_F);
}

int
thread_get_recent_cpu (void) 
{
  int64_t val = (int64_t) thread_current ()->recent_cpu * 100;
  return (int) (val >= 0 ? (val + FP_HALF) / FP_F : (val - FP_HALF) / FP_F);
}
```

---

## ۷. کالبدشکافی تست‌های MLFQS: هر تست چه هدفی دارد و چرا پاس می‌شود؟

| نام تست | هدف اصلی تست | چه عاملی در کد باعث پاس شدن آن می‌شود؟ |
| :--- | :--- | :--- |
| **`mlfqs-load-1`** | بررسی افزایش بار سیستم توسط یک پردازه فعال از ۰ به ۰.۵ در مدت ۳۸ تا ۴۵ ثانیه (مقدار تئوری: ۴۲ ثانیه) و سپس افت آن زیر ۰.۵ پس از ۱۰ ثانیه خواب. | تشخیص درست تعداد پردازه‌های فعال (`ready_threads = 1` در زمان چرخش، و ۰ در زمان خواب)، اعمال ضریب ۵۹/۶۰ در هر ثانیه بدون خطای گرد کردن اعشاری. |
| **`mlfqs-load-60`** | ۶۰ ریسمان ۱۰ ثانیه می‌خوابند، ۶۰ ثانیه به شدت پردازش می‌کنند و ۶۰ ثانیه دیگر می‌خوابند. بررسی منحنی تجمعی بار میانگین. | بیدار کردن ریسمان‌های خوابیده در وقفه تایمر پیش از بررسی تیک ۱۰۰ام؛ در نتیجه در ثانیه دهم همه ۶۰ ریسمان شمرده شده و بار میانگین دقیقاً برابر ۱.۰۰ در ثانیه ۰ آزمون ثبت می‌شود. |
| **`mlfqs-load-avg`** | ۶۰ ریسمان با تاخیرهای پله‌ای ($10+i$ ثانیه) بیدار شده و ۶۰ ثانیه کار می‌کنند. تست بار محاسباتی شدید و پیوسته وقفه تایمر. | بهینه‌سازی محاسبات و حداقل بودن سربار در `timer_interrupt` مانع از دست رفتن تیک‌های تایمر شده و ریسمان اصلی فرصت می‌کند به موقع بیدار شود و آمار را چاپ کند. |
| **`mlfqs-recent-1`** | صحت محاسبه مصرف اخیر پردازنده (`recent_cpu`) در طول ۱۸۰ ثانیه برای یک پردازه منفرد. | افزایش دقیق ۱ واحد در هر تیک به `recent_cpu` و اعمال دقیق ضریب میرایی $\frac{2 \times \text{load\_avg}}{2 \times \text{load\_avg} + 1}$ راس مرز تیک‌های ثانیه‌ای، همراه با گرد کردن صحیح در `thread_get_recent_cpu`. |
| **`mlfqs-fair-2`** | ۲ ریسمان با `nice = 0` به مدت ۳۰ ثانیه می‌چرخند؛ هر دو باید تقریباً به اندازه مساوی پردازنده را دریافت کنند (~۱۵۰۰ تیک هر کدام). | بازسنجی اولویت هر ۴ تیک؛ با افزایش مصرف ریسمان در حال اجرا، اولویت آن افت کرده و ریسمان دیگر به صورت عادلانه و متناوب پردازنده را در دست می‌گیرد. |
| **`mlfqs-fair-20`** | ۲۰ ریسمان با `nice = 0` به مدت ۳۰ ثانیه می‌چرخند؛ همه ۲۰ ریسمان باید سهم مساوی دریافت کنند (~۱۵۰ تیک هر کدام). | پایداری مرتب‌سازی `list_sort` در حفظ ترتیب FIFO برای اولویت‌های برابر، مانع از گرسنگی یا تبعیض میان ۲۰ ریسمان با اولویت یکسان می‌شود. |
| **`mlfqs-nice-2`** | ۲ ریسمان یکی با `nice = 0` و دیگری با `nice = 5` به مدت ۳۰ ثانیه اجرا می‌شوند. ریسمان اول باید سهم بیشتری (~۱۹۰۴ تیک) نسبت به دومی (~۱۰۹۶ تیک) بگیرد. | وجود عبارت $-(\text{nice} \times 2)$ در فرمول اولویت، ریسمان دوم را ۱۰ واحد در رده پایین‌تری نگه می‌دارد و مادامی که `recent_cpu` ریسمان اول رشد نکند، فرصت به ریسمان دوم نمی‌رسد. |
| **`mlfqs-nice-10`** | ۱۰ ریسمان با میزان سازگاری ۰ تا ۹ به مدت ۳۰ ثانیه رقابت می‌کنند و سهم دریافتی آن‌ها با مدل تئوری 4.4BSD سنجیده می‌شود. | توزیع خطی جریمه ناشی از `nice` در کنار کاهش یکسان کارکردها، سهم پردازنده را متناسب با گذشت ریسمان‌ها به شکل پلکانی و منظم تقسیم می‌کند. |
| **`mlfqs-block`** | ریسمان اصلی قفلی می‌گیرد و می‌خوابد. ریسمان بلاک‌شده سعی در گرفتن قفل دارد و ۱۰ ثانیه مسدود می‌ماند. پس از آزادسازی قفل، باید بلافاصله ریسمان مسدود پردازنده را تصاحب کند. | از آنجا که حلقه به‌روزرسانی `recent_cpu` بر روی تمام ریسمان‌های سیستم (`all_list` شامل ریسمان‌های مسدود) اجرا می‌شود، در مدت مسدودی مقدار کارکرد آن به شدت میرا شده و اولویتش بسیار بالا می‌رود؛ لذا با آزادسازی قفل، فوراً ریسمان اصلی را قبضه می‌کند. |

---

## ۸. نتایج آزمون‌ها و اعتبارسنجی نهایی

### آزمون‌های MLFQS (با پرچم `-mlfqs`)
```text
mlfqs-load-1: PASS
mlfqs-load-60: PASS
mlfqs-load-avg: PASS
mlfqs-recent-1: PASS
mlfqs-fair-2: PASS
mlfqs-fair-20: PASS
mlfqs-nice-2: PASS
mlfqs-nice-10: PASS
mlfqs-block: PASS
```

### آزمون‌های رگرسیون تایمر خواب و اولویت اکید (زمان‌بند استاندارد)
```text
alarm-single: PASS
alarm-multiple: PASS
alarm-simultaneous: PASS
alarm-priority: PASS
alarm-zero: PASS
alarm-negative: PASS
priority-change: PASS
priority-donate-one: PASS
priority-donate-multiple: PASS
priority-donate-multiple2: PASS
priority-donate-nest: PASS
priority-donate-sema: PASS
priority-donate-lower: PASS
priority-fifo: PASS
priority-preempt: PASS
priority-sema: PASS
priority-condvar: PASS
priority-donate-chain: PASS
```

**نتیجه کلی:** تمامی ۲۷ آزمون مربوط به فازهای ۱، ۲ و ۳ پروژه ۱ پینتوس با موفقیت ۱۰۰٪ پاس شدند.
هیچ دستور `git` اجرا نشده و کلیه محدودیت‌های پروژه رعایت گردید.
