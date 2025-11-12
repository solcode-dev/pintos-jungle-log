#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

static int64_t ticks;
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops(unsigned loops);
static void busy_wait(int64_t loops);
static void real_time_sleep(int64_t num, int32_t denom);
static struct list sleep_list; // 블락 된 스레드 관리 리스트

void timer_init(void)
{
   uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;
   list_init(&sleep_list); // 블락 된 스레드 관리 리스트 초기화

   outb(0x43, 0x34);
   outb(0x40, count & 0xff);
   outb(0x40, count >> 8);

   intr_register_ext(0x20, timer_interrupt, "8254 Timer");
}

void timer_calibrate(void)
{
   unsigned high_bit, test_bit;

   ASSERT(intr_get_level() == INTR_ON);
   printf("Calibrating timer...  ");

   loops_per_tick = 1u << 10;
   while (!too_many_loops(loops_per_tick << 1))
   {
      loops_per_tick <<= 1;
      ASSERT(loops_per_tick != 0);
   }

   high_bit = loops_per_tick;
   for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
      if (!too_many_loops(high_bit | test_bit))
         loops_per_tick |= test_bit;

   printf("%'" PRIu64 " loops/s.\n", (uint64_t)loops_per_tick * TIMER_FREQ);
}

int64_t timer_ticks(void)
{
   enum intr_level old_level = intr_disable();
   int64_t t = ticks;
   intr_set_level(old_level);
   barrier();

   return t;
}

// THEN 이후 경과된 타이머 틱 수를 반환
int64_t timer_elapsed(int64_t then)
{
   return timer_ticks() - then;
}

static bool compare_tick(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
   struct thread *thread_a = list_entry(a, struct thread, elem);
   struct thread *thread_b = list_entry(b, struct thread, elem);
   return thread_a->wakeup_tick < thread_b->wakeup_tick;
}

// 약 TICKS개의 타이머 틱 동안 실행을 일시 중단
void timer_sleep(int64_t ticks)
{
   int64_t start = timer_ticks(); // 현재 틱을 가져옮

   ASSERT(intr_get_level() == INTR_ON);
   struct thread *curr = thread_current();
   curr->wakeup_tick = start + ticks; // 기다려야하는 틱을 더해서 재시작해야되는 시간을 계산

   enum intr_level old_level = intr_disable();
   // 현재 스레드를 블록 리스트에 넣어줌 wakeup_tick 기준으로 정렬
   list_insert_ordered(&sleep_list, &curr->elem, compare_tick, NULL);
   thread_block(); // 현재 스레드를 블록 시킴
   intr_set_level(old_level);
}

// 약 MS 밀리초 동안 실행을 일시 중단한다
void timer_msleep(int64_t ms)
{
   // 약 MS 밀리초를 타이머 틱으로 변환하여 대기
   real_time_sleep(ms, 1000);
}

// 약 US 마이크로초 동안 실행을 일시 중단
void timer_usleep(int64_t us)
{
   real_time_sleep(us, 1000 * 1000);
}

// 약 NS 나노초 동안 실행을 일시 중단
void timer_nsleep(int64_t ns)
{
   real_time_sleep(ns, 1000 * 1000 * 1000);
}

// 타이머 통계를 출력
void timer_print_stats(void)
{
   printf("Timer: %" PRId64 " ticks\n", timer_ticks());
}

static void timer_interrupt(struct intr_frame *args UNUSED)
{
   ticks++;
   thread_tick();

   if (list_empty(&sleep_list))
      return;
   while (!list_empty(&sleep_list))
   {
      struct list_elem *slee_elem = list_front(&sleep_list);
      struct thread *t = list_entry(slee_elem, struct thread, elem);
      if (t->wakeup_tick <= ticks)
      {
         list_pop_front(&sleep_list);
         thread_unblock(t);
      }
      else
      {
         break;
      }
   }
}

static bool too_many_loops(unsigned loops)
{
   int64_t start = ticks;
   while (ticks == start)
      barrier();

   start = ticks;
   busy_wait(loops);

   barrier();
   return start != ticks;
}

static void NO_INLINE busy_wait(int64_t loops)
{
   while (loops-- > 0)
      barrier();
}

static void real_time_sleep(int64_t num, int32_t denom)
{
   int64_t ticks = num * TIMER_FREQ / denom;

   ASSERT(intr_get_level() == INTR_ON);
   if (ticks > 0)
   {
      timer_sleep(ticks);
   }
   else
   {
      ASSERT(denom % 1000 == 0);
      busy_wait(loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
   }
}
