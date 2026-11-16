# گزارش جامع پروژه ۱ پینتوس: زمان‌بندی مبتنی بر اولویت و اهدای اولویت (Priority Scheduling & Donation)

## ۱. مقدمه و بیان مسئله

در پیاده‌سازی اولیه سیستم‌عامل پینتوس (Pintos)، زمان‌بند پردازنده (Scheduler) به شکل چرخشی ساده (Round-Robin) و بر پایه صف خروج به ترتیب ورود (FIFO) کار می‌کند و هیچ توجهی به اولویت (Priority) ریسمان‌ها (Threads) ندارد. این نقص منجر به مشکلات بنیادین زیر می‌شود:
1. **عدم قبضه‌کردن (No Preemption):** زمانی که یک ریسمان با اولویت بالا آماده اجرا می‌شود، ریسمان جاری با اولویت پایین‌تر فورا متوقف نشده و به اجرای خود ادامه می‌دهد.
2. **بیداری بر اساس FIFO:** بدوی‌های همگام‌سازی (قفل‌ها، سمافورها و متغیرهای شرطی) ریسمان‌های مسدودشده را به جای اولویت، به ترتیب ورود بیدار می‌کنند.
3. **وارونگی اولویت (Priority Inversion):** مهم‌ترین نقص زمانی رخ می‌دهد که یک ریسمان با اولویت پایین قفلی را نگه داشته که یک ریسمان با اولویت بالا به آن نیاز دارد. در این وضعیت، ریسمان‌های با اولویت متوسط می‌توانند ریسمان با اولویت پایین را متوقف کرده و مانع از اجرای آن شوند؛ در نتیجه ریسمان با اولویت بالا به صورت نامحدود در انتظار می‌ماند (Starvation).

هدف این پروژه پیاده‌سازی موارد زیر بدون ایجاد هرگونه خرابی در آزمون‌های تایمر (`alarm-*`) است:
- **زمان‌بندی قطعی مبتنی بر اولویت (Strict Priority Scheduling)** همراه با قبضه‌کردن آنی پردازنده (Preemption).
- **همگام‌سازی آگاه از اولویت (Priority-Aware Synchronization)** در سمافورها و متغیرهای شرطی.
- **اهدای تودرتوی اولویت (Nested Priority Donation)** تا عمق ۸ و بازگردانی صحیح اولویت پس از آزادسازی قفل‌ها.

---

## ۲. تغییرات در ساختارهای داده (Data Structures)

### ۲.۱ تغییرات `struct thread` در فایل [`src/threads/thread.h`](file:///home/yasamin/pintos/src/threads/thread.h#L85-L109)

فیلدهای زیر به ساختار ریسمان افزوده شدند:

```c
struct thread
  {
    /* فیلدهای اختصاصی thread.c */
    tid_t tid;                          /**< شناسه ریسمان */
    enum thread_status status;          /**< وضعیت ریسمان */
    char name[16];                      /**< نام ریسمان */
    uint8_t *stack;                     /**< اشاره‌گر پشته ذخیره شده */
    int priority;                       /**< اولویت موثر ریسمان (با احتساب اهدا) */
    struct list_elem allelem;           /**< عنصر لیست کل ریسمان‌ها */

    /* فیلدهای مشترک بین thread.c و synch.c */
    struct list_elem elem;              /**< عنصر لیست صف آماده یا انتظار سمافور */

    int base_priority;                  /**< اولویت پایه ریسمان */
    struct list locks_held;             /**< لیست قفل‌هایی که این ریسمان در اختیار دارد */
    struct lock *lock_waiting_on;       /**< قفلی که ریسمان در انتظار دریافت آن است */
    ...
    unsigned magic;
  };
```

**علت و کاربرد فیلدها:**
- `base_priority`: اولویت اصلی ریسمان است. چنانچه ریسمانی اولویتی را به صورت قرضی (اهدایی) دریافت کرده باشد و تابع `thread_set_priority()` را فراخوانی کند، این اولویت پایه تغییر می‌کند اما اولویت موثر (`priority`) تا زمان آزادسازی قفل‌های مربوطه بالا می‌ماند.
- `locks_held`: لیستی از تمام قفل‌هایی است که ریسمان فعلا در دست دارد. هنگام آزادسازی یک قفل، ریسمان باید اولویت موثر خود را بر اساس اولویت دیگر ریسمان‌های منتظر روی «سایر قفل‌های در اختیارش» بازتعریف کند.
- `lock_waiting_on`: قفلی است که ریسمان جاری منتظر آن است. این فیلد برای پیمایش زنجیره اهدای اولویت تودرتو (Nested Donation) ضروری است.

### ۲.۲ تغییرات `struct lock` در فایل [`src/threads/synch.h`](file:///home/yasamin/pintos/src/threads/synch.h#L21-L27)

```c
struct lock 
  {
    struct thread *holder;      /**< ریسمان دارنده قفل */
    struct semaphore semaphore; /**< سمافور باینری کنترل‌کننده دسترسی */
    struct list_elem elem;      /**< عنصر لیست برای قرارگیری در locks_held ریسمان */
    int max_priority;           /**< بیشترین اولویت بین ریسمان‌های منتظر روی این قفل */
  };
```

**علت و کاربرد فیلدها:**
- `elem`: این امکان را می‌دهد که ساختار قفل مستقیما درون لیست `locks_held` ریسمان دارنده قرار گیرد بدون نیاز به تخصیص حافظه پویا (`malloc`).
- `max_priority`: نگه‌دارنده بالاترین اولویت در بین تمامی ریسمان‌های مسدودشده در صف انتظار این قفل است.

### ۲.۳ مقداردهی اولیه در `init_thread()` در فایل [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L529-L547)

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

**رفع باگ بحرانی که باعث پنیک می‌شد:**
در پیاده‌سازی اولیه، فیلدهای جدید فقط با `memset` صفر می‌شدند و دستور `list_init(&t->locks_held)` فراخوانی نشده بود. صفر بودن نشانگرهای سر و ته لیست باعث شد اولین باری که سیستم‌عامل در زمان بوت می‌خواست قفلی بگیرد (`allocate_tid()` برای `tid_lock`)، تابع `list_push_back` با شکست مواجه شده و کرنل پنیک زیر رخ دهد:
```text
assertion `is_interior (before) || is_tail (before)' failed.
```
با اضافه کردن مقداردهی صحیح به `base_priority` و `list_init(&t->locks_held)`، این مشکل به طور کامل برطرف گردید.

---

## ۳. مکانیزم‌های اصلی و تکه‌کدهای پیاده‌سازی‌شده

### ۳.۱ مقایسه‌گر اکید اولویت (`thread_cmp_priority`)
فایل: [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L347-L355)

```c
bool
thread_cmp_priority (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
  const struct thread *ta = list_entry (a, struct thread, elem);
  const struct thread *tb = list_entry (b, struct thread, elem);
  return ta->priority > tb->priority;
}
```

**قاعده بحرانی:**
توابع لیست در پینتوس مثل `list_insert_ordered` و `list_sort` نیاز به رابطه ترتیب ضعیف اکید (Strict Weak Ordering) دارند و باید منحصرا از عملگر بزرگ‌تر (`>`) استفاده شود، نه بزرگ‌تر مساوی (`>=`). استفاده از `>=` ساختار لیست پیوندی حلقوی را مختل کرده و ترتیب FIFO بین اولویت‌های برابر را برهم می‌زند.

### ۳.۲ صف آماده مرتب و محافظت از ریسمان بیگار (`idle_thread`)
فایل‌ها: [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L235-L246) و [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L304-L318)

در `thread_unblock`:
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

در `thread_yield`:
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

**قیود کلیدی رعایت‌شده:**
1. **عدم صدا زدن `thread_yield` در وقفه:** تابع `thread_unblock` به هیچ عنوان `thread_yield` را صدا نمی‌زند. چون این تابع در روتین وقفه ساعت (`timer_interrupt`) نیز فراخوانی می‌شود، تغییر زمینه در وقفه به کرش سیستم می‌انجامید. قبضه شدن پردازنده به توابع فراخواننده (مانند `thread_create` و `sema_up`) واگذار شده است.
2. **محافظت از `idle_thread`:** هنگام واگذاری پردازنده در `thread_yield`، شرط `if (cur != idle_thread)` تضمین می‌کند ریسمان بیگار وارد `ready_list` نشود، چرا که ورود آن باعث حلقه نامتناهی در زمان‌بند خواهد شد.

### ۳.۳ قبضه‌کردن آنی پردازنده (Preemption)
فایل: [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L204-L206)، [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L337-L345)، [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L397-L416)

هنگام ایجاد ریسمان در `thread_create`:
```c
  thread_unblock (t);

  if (thread_current ()->priority < t->priority)
    thread_yield ();
```

هنگام تغییر اولویت در `thread_set_priority`:
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

تابع کمکی ایمن برای بررسی قبضه `thread_yield_if_needed`:
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

**قیود کلیدی:**
- قبل از دسترسی به `list_front(&ready_list)` با `!list_empty(&ready_list)` خالی نبودن لیست چک می‌شود؛ خواندن سرلیست در لیست خالی باعث خطای نقص صفحه (Page Fault) و کرش می‌شد.
- شرط `if (intr_context()) return;` مانع از فراخوانی اشتباه `thread_yield` در وقفه سخت‌افزاری می‌شود.

### ۳.۴ همگام‌سازی مبتنی بر اولویت (سمافور و متغیر شرطی)
فایل: [`src/threads/synch.c`](file:///home/yasamin/pintos/src/threads/synch.c#L110-L127)، [`src/threads/synch.c`](file:///home/yasamin/pintos/src/threads/synch.c#L365-L404)

در `sema_up`:
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

در `cond_signal`:
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

**علت تغییرات:**
- در `sema_up`، مرتب‌سازی لیست انتظار با `thread_cmp_priority` باعث می‌شود ریسمان دارای بالاترین اولویت موثر بیدار شود. سپس با `thread_yield_if_needed` اگر اولویت آن از ریسمان جاری بیشتر بود، فورا پردازنده را قبضه می‌کند.
- در متغیرهای شرطی پینتوس، هر ریسمان منتظر دارای یک `semaphore_elem` است. تابع `cond_cmp_priority` بالاترین اولویت ریسمان‌های مسدودشده در هر سمافور را با هم مقایسه کرده و در نتیجه `cond_signal` سمافور مربوط به بااولویت‌ترین ریسمان را بالا می‌برد (`sema_up`).

### ۳.۵ منطق اهدای اولویت تودرتو (Nested Priority Donation)

#### الف) درخواست و اهدای اولویت در `lock_acquire`
فایل: [`src/threads/synch.c`](file:///home/yasamin/pintos/src/threads/synch.c#L218-L253)

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

#### ب) تابع کمکی اهدای اولویت `thread_donate_priority`
فایل: [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L383-L395)

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

#### ج) آزادسازی قفل و محاسبه مجدد اولویت در `lock_release`
فایل‌ها: [`src/threads/synch.c`](file:///home/yasamin/pintos/src/threads/synch.c#L284-L297) و [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L357-L381)

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

**علت پیاده‌سازی:**
- در `lock_acquire`، در صورتی که قفل صاحب داشته باشد، یک حلقه زنجیره دارندگان را تا حداکثر عمق ۸ دنبال می‌کند (`depth < 8`) و به هر ریسمانی در طول مسیر که اولویت پایین‌تری دارد، اولویت اهدا می‌کند.
- در `lock_release`، قبل از فراخوانی `sema_up`، قفل از لیست `locks_held` ریسمان خارج شده و سپس `thread_recompute_priority` اجرا می‌شود.
- در `thread_recompute_priority`، ابتدا اولویت برابر `base_priority` قرار می‌گیرد؛ سپس تمام قفل‌های باقیمانده در `locks_held` بررسی می‌شوند و بیشترین اولویت بین ریسمان‌های در انتظار آن‌ها به عنوان اولویت موثر ریسمان انتخاب می‌شود. در نهایت با اجرای `sema_up`، پردازنده در صورت لزوم به گیرنده بعدی قفل واگذار می‌گردد.

---

## ۴. تشریح آزمون‌های اولویت (`priority-*`) و علت پاس شدن هر کدام

در این بخش جزئیات تک‌تک ۱۲ آزمون اولویت و نحوه عبور موفقیت‌آمیز از آن‌ها تشریح شده است:

### ۱. آزمون `priority-change`
- **هدف آزمون:** بررسی اینکه اگر یک ریسمان در حال اجرا، اولویت خود را از طریق `thread_set_priority()` به مقداری کمتر از اولویت ریسمان‌های آماده (`ready_list`) کاهش دهد، باید **بلافاصله** پردازنده را به ریسمان با اولویت بالاتر واگذار کند.
- **علت پاس شدن:** در تابع `thread_set_priority()` پس از محاسبه مجدد اولویت با `thread_recompute_priority()`، تابع `thread_yield_if_needed()` فراخوانی می‌شود. این تابع بررسی می‌کند که آیا ریسمانی با اولویت بیشتر در صف آماده وجود دارد یا خیر، و در صورت وجود فورا `thread_yield()` می‌کند.

### ۲. آزمون `priority-donate-one`
- **هدف آزمون:** ریسمان اصلی (اولویت ۳۱) قفلی را می‌گیرد. دو ریسمان `acquire1` (اولویت ۳۲) و سپس `acquire2` (اولویت ۳۳) برای این قفل درخواست می‌دهند. اولویت ریسمان اصلی باید به ۳۲ و سپس به ۳۳ ارتقا یابد. در هنگام آزادسازی قفل، باید `acquire2` قبل از `acquire1` اجرا شود.
- **علت پاس شدن:** در `lock_acquire()`، اهدای اولویت به دارنده قفل، اولویت ریسمان اصلی را بالا می‌برد. در هنگام `lock_release()`، فراخوانی `sema_up()` صف انتظار را بر اساس اولویت مرتب می‌کند؛ در نتیجه `acquire2` (اولویت ۳۳) قبل از `acquire1` (اولویت ۳۲) بیدار شده و اجرا می‌شود.

### ۳. آزمون `priority-donate-multiple`
- **هدف آزمون:** ریسمان اصلی دو قفل $A$ و $B$ را می‌گیرد. ریسمان $A$ (اولویت ۳۲) منتظر قفل $A$ و ریسمان $B$ (اولویت ۳۳) منتظر قفل $B$ می‌شود. اولویت ریسمان اصلی ۳۳ می‌شود. وقتی قفل $B$ آزاد می‌شود، اولویت ریسمان اصلی نباید بلافاصله به ۳۱ برگردد، بلکه باید به ۳۲ تقلیل یابد (چون هنوز قفل $A$ را در دست دارد). در نهایت با آزادسازی قفل $A$ به ۳۱ بازمی‌گردد.
- **علت پاس شدن:** لیست `locks_held` هر دو قفل را نگه می‌دارد. با خروج قفل $B$ از لیست، تابع `thread_recompute_priority()` قفل $A$ باقیمانده را پیمایش کرده و اولویت منتظر روی آن (۳۲) را تشخیص می‌دهد و اولویت موثر را ۳۲ نگه می‌دارد.

### ۴. آزمون `priority-donate-multiple2`
- **هدف آزمون:** مشابه آزمون قبل، با این تفاوت که ترتیب آزادسازی برعکس است (اول قفل $A$ که اولویت اهدا شده ۳۴ دارد آزاد می‌شود در حالی که قفل $B$ دارای اولویت ۳۶ است) و یک ریسمان نامربوط $C$ با اولویت متوسط ۳۲ وجود دارد که نباید زودتر از موعد اجرا شود.
- **علت پاس شدن:** با آزادسازی قفل $A$، پیمایش کامل حلقه در `thread_recompute_priority()` روی قفل‌های باقیمانده باعث می‌شود بالاترین اولویت (قفل $B$ با اولویت ۳۶) حفظ شود و ریسمان $C$ پردازنده را نگیرد.

### ۵. آزمون `priority-donate-nest`
- **هدف آزمون:** اهدای اولویت تودرتو در زنجیره $H \rightarrow M \rightarrow L$. ریسمان با اولویت پایین $L$ قفل $A$ را دارد. ریسمان با اولویت متوسط $M$ قفل $B$ را گرفته و منتظر قفل $A$ می‌شود. ریسمان با اولویت بالا $H$ منتظر قفل $B$ می‌شود. اولویت $H$ باید از طریق $M$ به $L$ اهدا شود.
- **علت پاس شدن:** در `lock_acquire()`، حلقه `while (curr_lock != NULL && depth < 8)` از طریق فیلد `holder->lock_waiting_on` ساختار قفل بعدی را پیدا کرده و اولویت بالای $H$ را هم به $M$ و هم به دارنده قفل بعدی یعنی $L$ اهدا می‌کند.

### ۶. آزمون `priority-donate-sema`
- **هدف آزمون:** تقاطع اهدای اولویت با سمافور. ریسمان با اولویت پایین $L$ یک قفل می‌گیرد و سپس روی یک سمافور به خواب می‌رود (`sema_down`). ریسمان متوسط $M$ نیز روی همان سمافور می‌خوابد. ریسمان با اولویت بالا $H$ برای گرفتن قفل متعلق به $L$ مسدود شده و اولویتش را به $L$ اهدا می‌کند. وقتی سمافور `sema_up` می‌شود، $L$ باید زودتر از $M$ بیدار شود.
- **علت پاس شدن:** در `sema_up()` صف ریسمان‌های مسدودشده (`waiters`) مرتب می‌شود. از آنجا که اولویت موثر $L$ به دلیل اهدای اولویت توسط $H$ بالاتر از $M$ رفته است، $L$ در ابتدای صف قرار گرفته و اول بیدار می‌شود.

### ۷. آزمون `priority-donate-lower`
- **هدف آزمون:** ریسمان دارنده قفل در حالی که اولویت بالا به آن اهدا شده است، تابع `thread_set_priority()` را برای کاهش اولویت صدا می‌زند. این کاهش اولویت نباید در اولویت موثر در حین داشتن قفل اثر بگذارد و فقط پس از رها کردن قفل باید اعمال شود.
- **علت پاس شدن:** تفکیک `base_priority` و `priority` در ساختار ریسمان؛ `thread_set_priority()` مقدار `base_priority` را کم می‌کند، اما `thread_recompute_priority()` اولویت نهایی را به عنوان بیشینه `base_priority` و بیشترین اولویت اهدایی قفل‌های در اختیار تعیین می‌کند. پس از `lock_release()` اولویت به مقدار جدید کاهش یافته پایه برمی‌گردد.

### ۸. آزمون `priority-fifo`
- **هدف آزمون:** ۱۶ ریسمان با اولویت کاملا یکسان ساخته می‌شوند و بارها پردازنده را واگذار می‌کنند (`thread_yield`). ترتیب اجرای آن‌ها باید عینا مطابق نوبت ورود (FIFO Round-Robin) باقی بماند و تغییری نکند.
- **علت پاس شدن:** در مقایسه‌گر `thread_cmp_priority` اکیدا از `>` استفاده شده است. طبق رفتار `list_insert_ordered`، اگر شرط `a > b` برقرار نباشد، گره جدید پس از گره‌های دارای اولویت برابر درج می‌شود که ترتیب ورود را بدون تغییر حفظ می‌کند.

### ۹. آزمون `priority-preempt`
- **هدف آزمون:** ریسمانی با اولویت پایین یک ریسمان با اولویت بالاتر ایجاد می‌کند. ریسمان جدید باید بلافاصله پردازنده را در اختیار گرفته و کار خود را به پایان برساند، قبل از آنکه خط بعدی تابع سازنده اجرا شود.
- **علت پاس شدن:** در تابع `thread_create()` بلافاصله بعد از قرار دادن ریسمان جدید در صف آماده با `thread_unblock(t)`، مقایسه صورت گرفته و `thread_yield()` فراخوانی می‌شود:
  ```c
  if (thread_current ()->priority < t->priority)
    thread_yield ();
  ```

### ۱۰. آزمون `priority-sema`
- **هدف آزمون:** ۱۰ ریسمان با اولویت‌های گوناگون روی یک سمافور مسدود می‌شوند. با فراخوانی ۱۰ باره `sema_up()`، ریسمان‌ها باید دقیقا به ترتیب نزولی اولویت از خواب بیدار شوند.
- **علت پاس شدن:** در `sema_up()` قبل از بیرون کشیدن عضو از صف، `list_sort(&sema->waiters, thread_cmp_priority, NULL)` اجرا می‌شود که تضمین می‌کند همیشه بیشترین اولویت اول خارج شود.

### ۱۱. آزمون `priority-condvar`
- **هدف آزمون:** ۱۰ ریسمان با اولویت‌های مختلف روی یک متغیر شرطی با `cond_wait()` مسدود می‌شوند. با فراخوانی `cond_signal()`، ریسمانی که بیشترین اولویت را دارد باید پیش از سایرین بیدار شود.
- **علت پاس شدن:** تابع `cond_signal` صف `cond->waiters` را با استفاده از مقایسه‌گر اختصاصی `cond_cmp_priority` مرتب می‌کند که بیشترین اولویت درون سمافور هر عنصر را می‌سنجد، در نتیجه ریسمان با بالاترین اولویت بیدار می‌گردد.

### ۱۲. آزمون `priority-donate-chain`
- **هدف آزمون:** زنجیره‌ای عمیق با عمق ۸ سطح قفل متوالی ($T_7 \rightarrow \dots \rightarrow T_1 \rightarrow \text{Main}$) با ریسمان‌های مداخله‌گر (Interloper) با اولویت‌های میانی. آزادسازی قفل‌ها باید پله‌پله انجام شده و مانع از اجرای ریسمان‌های مداخله‌گر شود تا تمامی اهداکنندگان کار خود را تمام کنند.
- **علت پاس شدن:** پشتیبانی از پیمایش عمق ۸ در حلقه اهدای اولویت `lock_acquire`، به‌روزرسانی دینامیک `max_priority` در قفل‌ها و انتشار زنجیره‌ای اولویت در تمام دارندگان قفل در مسیر.

---

## ۵. خلاصه فایل‌های تغییر یافته

| نام فایل | تغییرات اعمال شده |
| :--- | :--- |
| [`src/threads/thread.h`](file:///home/yasamin/pintos/src/threads/thread.h) | افزودن `base_priority`، `locks_held`، `lock_waiting_on` به ساختار ریسمان و تعریف امضای توابع کمکی. |
| [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c) | پیاده‌سازی مقایسه‌گر `thread_cmp_priority`، توابع بازنگری اولویت `thread_recompute_priority`، اهدای اولویت `thread_donate_priority`، قبضه ایمن `thread_yield_if_needed`، اعمال قبضه در `thread_create` و `thread_set_priority`، و مقداردهی اولیه فیلدها در `init_thread`. |
| [`src/threads/synch.h`](file:///home/yasamin/pintos/src/threads/synch.h) | افزودن `elem` و `max_priority` به `struct lock` و معرفی تابع `lock_get_max_priority`. |
| [`src/threads/synch.c`](file:///home/yasamin/pintos/src/threads/synch.c) | پیاده‌سازی بیداری مبتنی بر اولویت در `sema_up` و `cond_signal` با مقایسه‌گر `cond_cmp_priority`. پیاده‌سازی اهدای اولویت تودرتو در `lock_acquire` و بازگردانی اولویت در `lock_release`. |

---

## ۶. نتیجه ارزیابی آزمون‌ها

تمامی ۱۲ آزمون اولویت (`priority-*`) و تمامی ۶ آزمون تایمر (`alarm-*`) با موفقیت ۱۰۰٪ پاس شدند:

```text
pass tests/threads/alarm-single
pass tests/threads/alarm-multiple
pass tests/threads/alarm-simultaneous
pass tests/threads/alarm-priority
pass tests/threads/alarm-zero
pass tests/threads/alarm-negative
pass tests/threads/priority-change
pass tests/threads/priority-donate-one
pass tests/threads/priority-donate-multiple
pass tests/threads/priority-donate-multiple2
pass tests/threads/priority-donate-nest
pass tests/threads/priority-donate-sema
pass tests/threads/priority-donate-lower
pass tests/threads/priority-fifo
pass tests/threads/priority-preempt
pass tests/threads/priority-sema
pass tests/threads/priority-condvar
pass tests/threads/priority-donate-chain
```
