# گزارش جامع پروژه ۱ پینتوس: تایمر بدون انتظار فعال (Alarm Clock)، زمان‌بندی مبتنی بر اولویت و اهدای اولویت

## ۱. مقدمه و اهداف پروژه

در اسکلت اولیه سیستم‌عامل پینتوس (Pintos) برای بخش ریسمان‌ها (Threads)، دو ناکارآمدی و نقص بنیادین وجود دارد:
1. **انتظار فعال در تایمر (Busy-Waiting Alarm Clock):** تابع پیش‌فرض `timer_sleep()` با اجرای یک حلقه `while` و فراخوانی مداوم `thread_yield()` پیاده‌سازی شده بود. این کار باعث می‌شد ریسمان در حال خواب هرگز صف آماده (`ready_list`) را ترک نکند، ۱۰۰٪ توان پردازنده بیهوده هدر برود، ریسمان بیکار (`idle_thread`) به هیچ وجه فرصت اجرا پیدا نکند (۰ تیک بیکاری) و امکان کاهش مصرف توان پردازنده (`hlt`) سلب شود.
2. **زمان‌بندی ناعادلانه و وارونگی اولویت (Priority Inversion):** زمان‌بند پیش‌فرض به صورت چرخه ساده (Round-Robin) بر مبنای خروج به ترتیب ورود (FIFO) بدون توجه به اولویت ریسمان‌ها کار می‌کرد. همچنین زمانی که یک ریسمان با اولویت پایین قفلی را در اختیار داشت که یک ریسمان با اولویت بالا به آن نیاز داشت، ریسمان‌های با اولویت متوسط مانع از اجرای ریسمان دارنده قفل شده و ریسمان با اولویت بالا دچار گرسنگی نامحدود می‌شد.

اهداف اصلی این پروژه عبارتند از:
- **پیاده‌سازی تایمر خواب بدون انتظار فعال (Non-Busy-Waiting `timer_sleep`)**: انتقال ریسمان‌های خوابیده به صف خواب (`sleep_list`)، مسدود کردن آن‌ها (`thread_block`)، بیدار کردن آن‌ها در وقفه تایمر (`timer_interrupt`) و اجازه دادن به پردازنده برای رفتن به حالت بیکار (`idle`).
- **زمان‌بندی اکید مبتنی بر اولویت (Strict Priority Scheduling)**: اجرای قطعی ریسمان‌ها بر اساس بالاترین اولویت و اعمال قبضه آنی پردازنده (Preemption).
- **اهدای اولویت تودرتو (Nested Priority Donation)**: انتقال موقت اولویت ریسمان‌های مسدودشده به دارندگان قفل تا عمق ۸ برای رفع مشکل وارونگی اولویت.
- **ادغام ایمن تایمر و اولویت**: تضمین این که بیدار شدن ریسمان‌های با اولویت بالا در زمان وقفه، منجر به پیش‌دستی و تعویض امن ریسمان شود بدون این که کرنل پنیک (Kernel Panic) در بستر وقفه رخ دهد.

---

## ۲. پیاده‌سازی تایمر خواب بدون انتظار فعال (`timer_sleep`)

### ۲.۱ ریشه مشکل انتظار فعال (Busy-Waiting)

در کد اولیه پینتوس، تابع `timer_sleep()` به شکل زیر پیاده‌سازی شده بود:
```c
/* پیاده‌سازی قدیمی بر پایه انتظار فعال */
void
timer_sleep (int64_t ticks) 
{
  int64_t start = timer_ticks ();

  ASSERT (intr_get_level () == INTR_ON);
  while (timer_elapsed (start) < ticks) 
    thread_yield ();
}
```

**چرا این پیاده‌سازی نادرست بود؟**
- ریسمان در حال خواب در وضعیت `THREAD_READY` در صف آماده باقی می‌ماند.
- زمان‌بند مدام آن را اجرا می‌کرد، ریسمان متوجه می‌شد زمان خوابش تمام نشده و با `thread_yield()` پردازنده را پس می‌داد.
- چون صف آماده هیچ‌گاه خالی نمی‌شد، پردازنده هرگز ریسمان بیکار (`idle_thread`) را اجرا نمی‌کرد. در نتیجه آمار خروجی به شکل `Thread: 0 idle ticks` گزارش می‌شد که نشان‌دهنده اتلاف حرارتی و مصرف بیهوده انرژی بود.

---

### ۲.۲ تغییرات در ساختارهای داده (Data Structures)

#### الف) افزودن فیلد `wake_up_tick` به `struct thread`
فایل: [`src/threads/thread.h`](file:///home/yasamin/pintos/src/threads/thread.h#L85-L112)

```c
struct thread
  {
    /* فیلدهای اختصاصی thread.c */
    tid_t tid;                          /**< شناسه ریسمان */
    enum thread_status status;          /**< وضعیت ریسمان */
    char name[16];                      /**< نام ریسمان */
    uint8_t *stack;                     /**< اشاره‌گر پشته ذخیره شده */
    int priority;                       /**< اولویت موثر ریسمان */
    struct list_elem allelem;           /**< عنصر لیست کل ریسمان‌ها */

    /* فیلدهای مشترک بین thread.c و synch.c */
    struct list_elem elem;              /**< عنصر لیست صف آماده یا انتظار */

    int64_t wake_up_tick;               /**< تیک سیستمی که ریسمان باید در آن بیدار شود */

    int base_priority;                  /**< اولویت پایه */
    struct list locks_held;             /**< قفل‌های در اختیار */
    struct lock *lock_waiting_on;       /**< قفلی که منتظر آن است */
    ...
```

**دلایل و منطق طراحی:**
- هر ریسمانی که به خواب می‌رود باید دقیقا بداند در کدام تیک مطلق ساعت سیستم (`timer_ticks() + ticks`) باید بیدار شود. ذخیره این مقدار در ساختار ریسمان امکان دسترسی در زمان $O(1)$ را فراهم می‌کند.
- استفاده مجدد از `elem` برای قرارگیری در صف خواب کاملا امن است؛ زیرا یک ریسمان خوابیده در وضعیت `THREAD_BLOCKED` است و به صورت همزمان نمی‌تواند در صف آماده (`ready_list`) یا صف انتظار سمافور قرار داشته باشد.

#### ب) تعریف و مقداردهی اولیه `sleep_list`
فایل‌ها: [`src/threads/thread.h`](file:///home/yasamin/pintos/src/threads/thread.h#L117) و [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L31-L98)

در فایل `src/threads/thread.h`:
```c
extern struct list sleep_list;
```

در فایل `src/threads/thread.c`:
```c
/** لیست ریسمان‌هایی که در timer_sleep به خواب رفته‌اند */
struct list sleep_list;
```

مقداردهی اولیه در تابع `thread_init()` در فایل [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L90-L105):
```c
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);
  list_init (&sleep_list);
  ...
```

مقداردهی صفر در تابع `init_thread()` در فایل [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L535-L555):
```c
  t->priority = priority;
  t->base_priority = priority;
  t->wake_up_tick = 0;
  list_init (&t->locks_held);
```

**دلایل و منطق طراحی:**
- متغیر `sleep_list` به عنوان یک لیست سراسری برای نگه‌داری تمام ریسمان‌های خوابیده در سیستم عمل می‌کند.
- مقداردهی با `list_init(&sleep_list)` در مرحله بوت سیستم‌عامل (`thread_init`) از مقداردهی نامعتبر اشاره‌گرهای `head` و `tail` جلوگیری کرده و مانع از بروز کرنل پنیک در زمان اجرای اولین خواب می‌شود.

---

### ۲.۳ بازنویسی تابع `timer_sleep()`

فایل: [`src/devices/timer.c`](file:///home/yasamin/pintos/src/devices/timer.c#L89-L103)

```c
/** Sleeps for approximately TICKS timer ticks.  Interrupts must
   be turned on. */
void
timer_sleep (int64_t ticks) 
{
  if (ticks <= 0)
    return;

  ASSERT (intr_get_level () == INTR_ON);

  enum intr_level old_level = intr_disable ();
  struct thread *cur = thread_current ();
  cur->wake_up_tick = timer_ticks () + ticks;
  list_push_back (&sleep_list, &cur->elem);
  thread_block ();
  intr_set_level (old_level);
}
```

**تشریح گام‌به‌گام و علل پیاده‌سازی:**
1. **بررسی ورودی‌های نامعتبر (`ticks <= 0`)**:
   - اگر زمان خواب صفر یا منفی باشد (مانند آزمون‌های `alarm-zero` و `alarm-negative`)، تابع باید بلافاصله بدون هیچ تغییری و بدون معطل کردن ریسمان بازگردد.
2. **بررسی فعال بودن وقفه‌ها (`ASSERT (intr_get_level () == INTR_ON)`)**:
   - خوابیدن یک عمل مسدودکننده است. اگر وقفه‌ها قطع باشند، ریسمان مسدودشده هیچ‌گاه وقفه تایمری دریافت نخواهد کرد تا بیدار شود و کل سیستم قفل می‌شود.
3. **غیرفعال‌سازی اتمیک وقفه‌ها (`old_level = intr_disable ()`)**:
   - برای جلوگیری از شرایط رقابتی (Race Condition)، محاسبه زمان بیداری، درج در `sleep_list` و فراخوانی `thread_block()` باید به صورت کاملا اتمیک انجام شود. اگر حین این عملیات وقفه تایمر رخ می‌داد، وضعیت ریسمان ناسازگار می‌شد.
4. **محاسبه زمان بیداری و درج در لیست**:
   - `cur->wake_up_tick = timer_ticks () + ticks;` زمان مطلق بیداری در آینده را محاسبه و ذخیره می‌کند.
   - `list_push_back (&sleep_list, &cur->elem);` ریسمان را به صف خواب اضافه می‌کند.
5. **مسدودسازی ریسمان (`thread_block ()`)**:
   - وضعیت ریسمان جاری به `THREAD_BLOCKED` تغییر یافته و با فراخوانی `schedule()` پردازنده بلافاصله به ریسمان آماده بعدی یا ریسمان بیکار (`idle_thread`) واگذار می‌شود.
6. **بازگردانی سطح وقفه (`intr_set_level (old_level)`)**:
   - زمانی که موعد خواب ریسمان به پایان برسد و مجددا به اجرا درآید، دستور بعد از `thread_block()` اجرا شده و سطح وقفه به حالت فعال بازگردانده می‌شود.

---

### ۲.۴ بازنویسی اداره‌کننده وقفه `timer_interrupt()`

فایل: [`src/devices/timer.c`](file:///home/yasamin/pintos/src/devices/timer.c#L176-L202)

```c
/** Timer interrupt handler. */
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
          if (t->priority > thread_current ()->priority)
            yield_needed = true;
        }
      else
        {
          e = list_next (e);
        }
    }

  if (yield_needed)
    intr_yield_on_return ();
}
```

**تشریح گام‌به‌گام و علل پیاده‌سازی:**
1. **افزایش تیک و آمارگیری**: دستور `ticks++` ساعت سراسری سیستم را جلو می‌برد و `thread_tick()` آمار تیک‌های کرنل و بیکاری را به‌روزرسانی می‌کند.
2. **پیمایش امن لیست حین حذف عناصر**:
   - هنگام پیمایش لیست اگر شرط `t->wake_up_tick <= ticks` برقرار باشد، ریسمان باید از لیست حذف شود. تابع `list_remove(e)` در پینتوس عنصر جاری را جدا کرده و اشاره‌گر به «عنصر بعدی» را بازمی‌گرداند (`e = list_remove(e)`). این کار از باگ خطرناک اشاره‌گرهای نامعتبر جلوگیری می‌کند.
   - در صورتی که زمان بیداری نرسیده باشد، با `e = list_next (e)` به عنصر بعدی می‌رویم.
3. **خارج کردن ریسمان از حالت مسدود (`thread_unblock (t)`)**:
   - ریسمان از حالت مسدود خارج شده و به صف آماده بازگردانده می‌شود. در این تابع، ریسمان با دستور `list_insert_ordered (&ready_list, &t->elem, thread_cmp_priority, NULL)` بر اساس اولویت درج می‌گردد.
4. **بررسی نیاز به تعویض ریسمان (Preemption)**:
   - اگر اولویت ریسمان بیدارشده اکیدا از اولویت ریسمان جاری بیشتر باشد (`t->priority > thread_current ()->priority`)، متغیر `yield_needed` علامت‌گذاری می‌شود.
5. **پیش‌دستی امن پردازنده با `intr_yield_on_return()`**:
   - به جای فراخوانی مستقیم و خطرناک `thread_yield()`، تابع `intr_yield_on_return()` فراخوانی می‌شود تا سیستم‌عامل در لحظه خروج از وقفه پردازنده را به ریسمان با اولویت بالاتر بدهد.

---

## ۳. نکات و ملاحظات بحرانی ادغام با زمان‌بندی اولویت (Priority Integration)

به دلیل فعال بودن سیستم زمان‌بندی مبتنی بر اولویت و اهدای اولویت در این مخزن کد، ادغام تایمر با این سیستم نیازمند رعایت نکات امنیتی و سطح کرنل زیر است:

### ۳.۱ خطای مهلک فراخوانی `thread_yield()` در بستر وقفه
- اداره‌کننده وقفه تایمر در بستر وقفه سخت‌افزاری (`intr_context() == true`) اجرا می‌شود.
- در سیستم‌عامل پینتوس، فراخوانی مستقیم `thread_yield()` درون بستر وقفه به دلیل وجود بررسی امنیتی زیر بلافاصله منجر به **کرنل پنیک** و توقف سیستم می‌شود:
  ```c
  void
  thread_yield (void) 
  {
    ...
    ASSERT (!intr_context ()); // اگر درون وقفه صدا زده شود پنیک رخ می‌دهد!
    ...
  }
  ```
- به همین علت هرگز نباید درون `timer_interrupt()` یا درون تابع `thread_unblock()` دستور `thread_yield()` فراخوانی شود.

### ۳.۲ راهکار استاندارد: استفاده از `intr_yield_on_return()`
- برای تعویض ریسمان در صورت بیدار شدن ریسمان دارای اولویت بالاتر، پینتوس مکانیزم اختصاصی `intr_yield_on_return()` را تعبیه کرده است:
  ```c
  void
  intr_yield_on_return (void) 
  {
    ASSERT (intr_context ());
    yield_on_return = true;
  }
  ```
- در انتهای پردازش وقفه در فایل [`src/threads/interrupt.c`](file:///home/yasamin/pintos/src/threads/interrupt.c#L380-L388):
  ```c
  in_external_intr = false;
  pic_end_of_interrupt (frame->vec_no); 

  if (yield_on_return) 
    thread_yield (); 
  ```
- نکته بسیار ظریف این است که متغیر `in_external_intr` پیش از فراخوانی `thread_yield()` به مقدار `false` تغییر می‌کند؛ بنابراین در این لحظه تابع `intr_context()` مقدار غلط برمی‌گرداند و `thread_yield()` بدون هیچ مشکلی و با رعایت کامل شرایط ایمنی، ریسمان‌ها را جابجا می‌کند.

### ۳.۳ ترتیب بیداری ریسمان‌ها در تیک یکسان
- در صورتی که چند ریسمان همزمان در یک تیک بیدار شوند (مانند آزمون `alarm-priority`)، تابع `timer_interrupt()` تک‌تک آن‌ها را با `thread_unblock(t)` بیدار می‌کند.
- چون تابع `thread_unblock()` ریسمان‌ها را به صورت مرتب‌شده بر اساس اولویت (`list_insert_ordered`) در `ready_list` قرار می‌دهد، صف آماده همواره به ترتیب نزولی اولویت چیده می‌شود.
- سپس با خروج از وقفه و اجرای `thread_yield()`، بلافاصله ریسمان دارای بالاترین اولویت اجرا می‌گردد.

### ۳.۴ تعامل صحیح با ریسمان بیکار (`idle_thread`)
- زمانی که تمام ریسمان‌ها خواب هستند، سیستم‌عامل وارد حلقه ریسمان بیکار در [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L456-L492) می‌شود:
  ```c
  for (;;) 
    {
      intr_disable ();
      thread_block ();
      asm volatile ("sti; hlt" : : : "memory");
    }
  ```
- دستور اسمبلی `hlt` پردازنده را در وضعیت کم‌مصرف قرار می‌دهد تا وقفه بعدی برسد. وقتی وقفه تایمر رخ می‌دهد:
  - اگر اولویت ریسمان بیدارشده بیشتر باشد، با `intr_yield_on_return()` پردازنده پیش از بازگشت به بیکار تعویض می‌شود.
  - حتی اگر مستقیما به ریسمان بیکار بازگردد، اولین دستور بعدی در حلقه، `thread_block()` و سپس `schedule()` است که بلافاصله ریسمان بیدارشده درون صف آماده را انتخاب و اجرا می‌کند.

---

## ۴. بررسی دقیق آزمون‌های تایمر: هدف هر آزمون و دلایل موفقیت آن‌ها

### ۱. آزمون `alarm-single`
- **هدف آزمون**:
  - ایجاد ۵ ریسمان که هر کدام ۱ بار به ترتیب به مدت ۱۰، ۲۰، ۳۰، ۴۰ و ۵۰ تیک به خواب می‌روند.
  - بررسی ترتیب بیدار شدن صعودی بر اساس مدت خواب.
  - مهم‌ترین هدف: بررسی این که آیا پردازنده در حین خواب بودن ریسمان‌ها واقعا بیکار بوده است یا خیر (`idle ticks > 0`).
- **علت موفقیت آزمون**:
  - ریسمان‌ها به جای حلقه چرخش فعال، با `thread_block()` مسدود شدند.
  - در مدت زمان خواب این ریسمان‌ها، ریسمان `idle_thread` به اجرا درآمد و آمار **۲۵۰ تیک بیکاری (Idle Ticks)** ثبت شد (در مقایسه با ۰ تیک در نسخه قدیمی).
  - بیداری در تیک‌های ۱۰، ۲۰، ۳۰، ۴۰ و ۵۰ با نظم کامل صورت پذیرفت.

### ۲. آزمون `alarm-multiple`
- **هدف آزمون**:
  - ایجاد ۵ ریسمان که هر کدام ۷ بار متوالی به مدت‌های ۱۰، ۲۰، ۳۰، ۴۰ و ۵۰ تیک به خواب می‌روند (مجموعا ۳۵ بار چرخه خواب و بیداری).
  - ارزیابی ثبات ساختار صف خواب (`sleep_list`) در درج‌ها و حذف‌های متوالی و مکرر، و اطمینان از عدم گم شدن تیک‌ها یا جابجایی ترتیبی.
- **علت موفقیت آزمون**:
  - در هر دور از حلقه، زمان بیداری بر اساس ساعت جاری سیستم (`timer_ticks() + duration`) محاسبه شده و وضعیت وقفه به درستی ذخیره و بازیابی می‌شود.
  - هیچ نشت حافظه یا اشاره‌گر مخدوشی در صف خواب به وجود نیامده و ۳۵ بار بیداری بدون هیچ خطایی انجام گرفت.

### ۳. آزمون `alarm-simultaneous`
- **هدف آزمون**:
  - ایجاد ۳ ریسمان در ۵ تکرار، به‌گونه‌ای که هر ۳ ریسمان طوری تنظیم شده‌اند که دقیقا در یک تیک معین و یکسان بیدار شوند.
  - بررسی این که آیا سیستم می‌تواند چند ریسمان را در یک تیک واحد بیدار کند بدون اینکه ساعت سیستم جلو برود (`iteration X, thread Y: woke up 0 ticks later`).
- **علت موفقیت آزمون**:
  - در اداره‌کننده وقفه `timer_interrupt()`، حلقه `while` تمامی عناصر `sleep_list` را پیمایش می‌کند.
  - در تیک مورد نظر، شرط `t->wake_up_tick <= ticks` برای هر ۳ ریسمان در همان تک‌وقفه صادق است.
  - هر ۳ ریسمان در همان تیک از صف خارج و آماده شده و ساعت قبل از بیدار شدن همه آن‌ها جلو نمی‌رود.

### ۴. آزمون `alarm-priority`
- **هدف آزمون**:
  - ایجاد ۱۰ ریسمان با اولویت‌های متمایز از ۲۱ تا ۳۰ (`priority 21` تا `priority 30`).
  - تمام ۱۰ ریسمان دقیقا تا یک تیک مشترک (`wake_time`) به خواب می‌روند.
  - اولویت ریسمان اصلی به حداقل (`PRI_MIN = 0`) کاهش می‌یابد.
  - آزمون اعتبارسنجی می‌کند که به محض فرارسیدن زمان بیداری، ریسمان‌ها دقیقا به ترتیب نزولی اولویت بیدار شده و پیام خود را چاپ کنند:
    ```text
    (alarm-priority) Thread priority 30 woke up.
    (alarm-priority) Thread priority 29 woke up.
    ...
    (alarm-priority) Thread priority 21 woke up.
    ```
- **علت موفقیت آزمون**:
  - در تیک `wake_time`، اداره‌کننده وقفه تایمر هر ۱۰ ریسمان را بیدار می‌کند.
  - تابع `thread_unblock()` با استفاده از مقایسه‌گر اولویت (`thread_cmp_priority`) ریسمان‌ها را به شکل مرتب در `ready_list` قرار می‌دهد (از اولویت ۳۰ تا ۲۱).
  - چون اولویت این ریسمان‌ها از اولویت ریسمان جاری (که ۰ است) بیشتر است، متغیر `yield_needed` فعال شده و `intr_yield_on_return()` فراخوانی می‌شود.
  - با پایان یافتن وقفه، زمان‌بند ریسمان ۳۰ را اجرا می‌کند، سپس با اتمام کار آن ریسمان ۲۹، و به همین ترتیب تا ۲۱ اجرا می‌شوند.

### ۵. آزمون `alarm-zero`
- **هدف آزمون**:
  - فراخوانی `timer_sleep(0)`.
  - بررسی این که آیا سیستم‌عامل برای مدت خواب صفر فورا برمی‌گردد یا بدون دلیل یک تیک منتظر می‌ماند یا دچار خطا می‌شود.
- **علت موفقیت آزمون**:
  - شرط محافظ اولیه در تابع `timer_sleep()`:
    ```c
    if (ticks <= 0)
      return;
    ```
  - از هرگونه مسدودسازی بیهوده ریسمان جلوگیری کرده و تابع بلادرنگ به فراخواننده بازمی‌گردد.

### ۶. آزمون `alarm-negative`
- **هدف آزمون**:
  - فراخوانی `timer_sleep(-100)`.
  - اطمینان از این که مقادیر منفی تیک باعث سرریز منفی اعداد، به خواب رفتن ابدی، یا خطای کرنل نمی‌شود.
- **علت موفقیت آزمون**:
  - با همان شرط `if (ticks <= 0) return;` ورودی منفی بلافاصله شناسایی شده و بدون هیچ اقدام مخربی نادیده گرفته می‌شود.

---

## ۵. خلاصه زمان‌بندی مبتنی بر اولویت و اهدای اولویت (Priority Scheduling & Donation)

جهت یکپارچگی گزارش پروژه، اصول و مکانیزم‌های پیاده‌سازی‌شده در بخش اولویت نیز در ادامه مرور شده‌اند:

### ۵.۱ تغییرات `struct lock`
فایل: [`src/threads/synch.h`](file:///home/yasamin/pintos/src/threads/synch.h#L21-L27)
```c
struct lock 
  {
    struct thread *holder;      /**< ریسمان دارنده قفل */
    struct semaphore semaphore; /**< سمافور کنترل دسترسی */
    struct list_elem elem;      /**< عنصر لیست برای قرارگیری در locks_held */
    int max_priority;           /**< بیشترین اولویت بین منتظران این قفل */
  };
```

### ۵.۲ تابع مقایسه اولویت (`thread_cmp_priority`)
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
استفاده از علامت اکید بزرگتر (`>`) تضمین می‌کند که ریسمان‌های دارای اولویت برابر، به ترتیب ورود (FIFO) زمان‌بندی شوند (برای پاس شدن آزمون `priority-fifo`).

### ۵.۳ اهدای اولویت در `lock_acquire()`
فایل: [`src/threads/synch.c`](file:///home/yasamin/pintos/src/threads/synch.c#L193-L255)
- در زمان تلاش برای دریافت قفلی که در اختیار دیگری است، حلقه اهدای اولویت تودرتو تا عمق ۸ فعال می‌شود:
  ```c
  struct lock *curr_lock = lock;
  int depth = 0;
  while (curr_lock != NULL && depth < 8)
    {
      struct thread *holder = curr_lock->holder;
      if (holder == NULL)
        break;
      if (holder->priority < cur->priority)
        thread_donate_priority (holder, cur->priority);
      curr_lock = holder->lock_waiting_on;
      depth++;
    }
  ```
- این حلقه مانع از بروز وارونگی اولویت در زنجیره‌های قفل چندمرحله‌ای ($H \rightarrow M \rightarrow L$) می‌شود.

### ۵.۴ محاسبه مجدد اولویت در `lock_release()`
فایل: [`src/threads/synch.c`](file:///home/yasamin/pintos/src/threads/synch.c#L280-L305) و [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c#L357-L382)
- با آزادسازی یک قفل:
  1. قفل از لیست `locks_held` ریسمان خارج می‌شود.
  2. اولویت موثر ریسمان با فرمول زیر مجددا محاسبه می‌شود:
     $$\text{priority} = \max\left(\text{base\_priority}, \max_{l \in \text{locks\_held}} (\text{max priority waiting on } l)\right)$$
  3. با فراخوانی `sema_up()` ریسمان مسدودشده با بالاترین اولویت بیدار شده و در صورت نیاز پردازنده بلافاصله قبضه می‌شود.

---

## ۶. جدول تغییرات فایل‌های پروژه

| فایل | شرح تغییرات اعمال‌شده |
| :--- | :--- |
| [`src/devices/timer.c`](file:///home/yasamin/pintos/src/devices/timer.c) | حذف چرخه انتظار فعال در `timer_sleep()` و استفاده از `thread_block()`؛ پیاده‌سازی حلقه بیداری و بررسی قبضه با `intr_yield_on_return()` در `timer_interrupt()`. |
| [`src/threads/thread.h`](file:///home/yasamin/pintos/src/threads/thread.h) | افزودن `int64_t wake_up_tick` به `struct thread`؛ تعریف `extern struct list sleep_list;`. |
| [`src/threads/thread.c`](file:///home/yasamin/pintos/src/threads/thread.c) | تعریف متغیر سراسری `sleep_list`؛ مقداردهی آن در `thread_init()`؛ مقداردهی اولیه `wake_up_tick = 0` در `init_thread()`. |
| [`src/threads/synch.c`](file:///home/yasamin/pintos/src/threads/synch.c) | همگام‌سازی مبتنی بر اولویت در سمافورها، قفل‌ها و متغیرهای شرطی؛ اهدای اولویت تودرتو و بازگردانی اولویت. |
| [`src/threads/synch.h`](file:///home/yasamin/pintos/src/threads/synch.h) | افزودن فیلدهای `elem` و `max_priority` به ساختار `struct lock`. |

---

## ۷. ماتریس نتایج اعتبارسنجی کلیه آزمون‌ها

تمامی ۱۸ آزمون (۶ آزمون تایمر و ۱۲ آزمون اولویت) با موفقیت ۱۰۰٪ پاس شدند:

| نام آزمون | دسته‌بندی | وضعیت | نکته کلیدی ارزیابی آزمون |
| :--- | :--- | :--- | :--- |
| `tests/threads/alarm-single` | تایمر | **PASS** | اثبات عدم انتظار فعال؛ تیک‌های بیکاری از ۰ به ۲۵۰ رسید. |
| `tests/threads/alarm-multiple` | تایمر | **PASS** | ۳۵ بار خواب و بیداری بدون گم شدن تیک‌ها. |
| `tests/threads/alarm-simultaneous` | تایمر | **PASS** | بیداری دقیق چند ریسمان در یک تیک مشترک ("0 ticks later"). |
| `tests/threads/alarm-priority` | تایمر + اولویت | **PASS** | بیداری همزمان و اجرای قطعی به ترتیب نزولی اولویت (۳۰ تا ۲۱). |
| `tests/threads/alarm-zero` | تایمر | **PASS** | بازگشت فوری برای خواب صفر تیک. |
| `tests/threads/alarm-negative` | تایمر | **PASS** | مدیریت امن مقادیر منفی بدون هیچ خطایی. |
| `tests/threads/priority-change` | اولویت | **PASS** | قبضه آنی پردازنده با کاهش دستی اولویت ریسمان جاری. |
| `tests/threads/priority-donate-one` | اهدای اولویت | **PASS** | اهدای تک‌مرحله‌ای اولویت و بازگردانی آن پس از رهاسازی قفل. |
| `tests/threads/priority-donate-multiple` | اهدای اولویت | **PASS** | در اختیار داشتن چند قفل و حفظ اولویت اهدایی تا رهاسازی آخرین قفل. |
| `tests/threads/priority-donate-multiple2` | اهدای اولویت | **PASS** | رهاسازی نامرتب قفل‌ها و حفظ بالاترین اولویت اهدایی باقی‌مانده. |
| `tests/threads/priority-donate-nest` | اهدای اولویت | **PASS** | انتقال تودرتوی اولویت در زنجیره قفل ($H \rightarrow M \rightarrow L$). |
| `tests/threads/priority-donate-sema` | اهدای اولویت | **PASS** | اثرگذاری اولویت اهدایی بر صف انتظار سمافورها. |
| `tests/threads/priority-donate-lower` | اهدای اولویت | **PASS** | کاهش `base_priority` در حین داشتن قفل و اعمال آن پس از آزادسازی. |
| `tests/threads/priority-fifo` | اولویت | **PASS** | تضمین اجرای عادلانه FIFO برای ریسمان‌های هم‌اولویت. |
| `tests/threads/priority-preempt` | اولویت | **PASS** | قبضه آنی با ساخت ریسمان دارای اولویت بالاتر در `thread_create`. |
| `tests/threads/priority-sema` | اولویت | **PASS** | خروج ریسمان‌ها از صف سمافور به ترتیب نزولی اولویت. |
| `tests/threads/priority-condvar` | اولویت | **PASS** | بیداری متغیر شرطی بر اساس بالاترین اولویت ریسمان منتظر. |
| `tests/threads/priority-donate-chain` | اهدای اولویت | **PASS** | پیمایش زنجیره ۸ مرحله‌ای اهدای قفل بدون گرسنگی. |
