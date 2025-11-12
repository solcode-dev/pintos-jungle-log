#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

#define THREAD_MAGIC 0xcd6abf4b
#define THREAD_BASIC 0xd42df210

static struct thread *idle_thread, *initial_thread; // idle 스레드와 최초(main) 스레드 포인터
static struct lock tid_lock;												// TID 중복 방지를 위한 락
static struct list dying_threads_queue;							// 종료 요청된 스레드(파괴 대기) 관리 리스트

static long long idle_ticks;	 // idle 상태 동안 누적된 타이머 틱 수
static long long kernel_ticks; // 커널 스레드가 실행된 동안 누적된 타이머 틱 수
static long long user_ticks;	 // 유저 프로그램이 실행된 동안 누적된 타이머 틱 수

/* 스케쥴링 */
#define TIME_SLICE 4					/// 각 스레드가 한 번 실행 시 부여되는 타이머 틱(스케줄 타임슬라이스)
static unsigned thread_ticks; /// 마지막 yield 이후 경과된 타이머 틱 수

bool thread_mlfqs; // MLFQ 방식 플래그
struct list ready_list;

static void kernel_thread(thread_func *, void *aux);

static void idle(void *aux UNUSED);
static struct thread *next_thread_to_run(void);
static void init_thread(struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule(void);
static tid_t allocate_tid(void);

#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC) // T가 올바른 스레드인가

// 현재 실행 중인 스레드를 반환
#define running_thread() ((struct thread *)(pg_round_down(rrsp())))

static uint64_t gdt[3] = {0, 0x00af9a000000ffff, 0x00cf92000000ffff};

/* 현재 실행 중인 코드를 스레드로 변환하여 스레드 시스템을 초기화하는 함수.
	이 함수가 끝나기 전까지는 thread_current()를 호출하면 안전하지 않다. */
void thread_init(void)
{
	ASSERT(intr_get_level() == INTR_OFF);

	struct desc_ptr gdt_ds = {
			.size = sizeof(gdt) - 1,
			.address = (uint64_t)gdt};
	lgdt(&gdt_ds);

	// 전역 스레드 컨텍스트 초기화
	lock_init(&tid_lock);
	list_init(&ready_list);
	list_init(&dying_threads_queue);

	// 현재 실행 중인 스레드 구조체 설정
	initial_thread = running_thread();
	init_thread(initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid();
}

/**
 * @brief 선점형 스레드 스케줄링을 시작하는 함수
 *
 * @details 이 함수는 idle 스레드를 생성하고, 인터럽트를 활성화하여
 *          스케줄러를 동작시키며, idle 스레드 초기화가 완료될 때까지 대기합니다.
 *          main()에서 thread_init() 이후에 호출됩니다.
 *
 * @note 이 함수가 반환되면 스레드 시스템이 정상 작동하며,
 *       타이머 인터럽트를 통해 스레드 스케줄링이 자동으로 이루어집니다.
 */
void thread_start(void)
{
	// idle 스레드 초기화 완료를 기다리기 위한 세마포어 선언
	struct semaphore idle_started;
	sema_init(&idle_started, 0);

	// 세마포어 초기값 검증
	ASSERT(idle_started.value == 0);

	// idle 스레드 생성 → ready_list에 추가
	tid_t idle_tid = thread_create("idle", PRI_MIN, idle, &idle_started);
	ASSERT(idle_tid != TID_ERROR);

	// 인터럽트를 활성화하여 타이머 인터럽트 기반 스케줄링 시작
	intr_enable();

	// 스케줄러가 정상 동작하려면 인터럽트가 켜진 상태(INTR_ON)여야 함
	ASSERT(intr_get_level() == INTR_ON);

	// idle 스레드가 초기화를 완료하고 sema_up()을 호출할 때까지 메인 스레드를 대기시킴
	sema_down(&idle_started);

	ASSERT(idle_thread != NULL);
}

/* 타이머 인터럽트 핸들러가 매 타이머 틱마다 호출합니다.
	따라서 이 함수는 외부 인터럽트 컨텍스트에서 실행됩니다. */
void thread_tick(void)
{
	struct thread *curr = thread_current();

	// Update statistics
	if (curr == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (curr->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	// 선점(Preemption) 강제 처리
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return();
}

// 스레드 통계 정보를 출력
void thread_print_stats(void)
{
}

/**
 * @brief 새로운 커널 스레드를 생성하고 ready queue에 추가하는 함수
 *
 * @param name 생성할 스레드의 이름
 * @param priority 스레드의 초기 우선순위 (높을수록 먼저 실행)
 * @param function 스레드가 실행할 함수 (스레드의 main 역할)
 * @param aux function에 전달될 인자 (보통 동기화용 세마포어 등)
 *
 * @return tid_t 생성된 스레드의 ID (TID_ERROR: 생성 실패)
 *
 * @details 이 함수는 새로운 커널 스레드를 생성하고 ready_list에 추가합니다.
 *          thread_start()가 호출된 후에는 thread_create()가 반환되기 전에
 *          새 스레드가 스케줄링될 수 있으며, 심지어 종료될 수도 있습니다.
 *          반대로 원래 스레드가 새 스레드가 스케줄되기 전에 계속 실행될 수도 있습니다.
 *          실행 순서를 보장하려면 세마포어 등의 동기화 기법을 사용해야 합니다.
 *
 * @warning function이 NULL이면 ASSERT 실패로 커널 패닉이 발생합니다.
 */
tid_t thread_create(const char *name, int priority,
										thread_func *function, void *aux)
{
	struct thread *curr;
	tid_t tid;

	// 실행할 함수가 NULL이 아닌지 검증
	ASSERT(function != NULL);

	// 페이지 단위로 메모리를 할당하고 0으로 초기화
	curr = palloc_get_page(PAL_ZERO);
	if (curr == NULL)
		return TID_ERROR;

	// 스레드 구조체 초기화
	init_thread(curr, name, priority);

	tid = curr->tid = allocate_tid();

	// 스레드 실행 컨텍스트 설정
	curr->tf.rip = (uintptr_t)kernel_thread;
	curr->tf.R.rdi = (uint64_t)function; // 첫 번째 인자: 실행할 함수 포인터
	curr->tf.R.rsi = (uint64_t)aux;			 // 두 번째 인자: 함수에 전달할 보조 파라미터

	// 세그먼트 레지스터 설정 (커널 모드)
	curr->tf.ds = SEL_KDSEG;
	curr->tf.es = SEL_KDSEG;
	curr->tf.ss = SEL_KDSEG;
	curr->tf.cs = SEL_KCSEG;

	// 인터럽트 플래그 활성화 (스케줄러는 인터럽트 비활성화 상태에서 실행)
	curr->tf.eflags = FLAG_IF;

	// 스레드를 READY 상태로 변경하고 ready_list에 추가
	thread_unblock(curr);

	enum intr_level old_level = intr_disable();
	preemption_by_priority(); // 우선순위 기반 선점 스케줄링 실행
	intr_set_level(old_level);

	return tid;
}

/* 현재 스레드를 잠자게(sleep) 만듭니다.
thread_unblock()에 의해 깨워질 때까지 스케줄되지 않습니다.

	이 함수는 반드시 인터럽트가 꺼진 상태에서 호출해야 합니다. */
void thread_block(void)
{
	ASSERT(!intr_context());
	ASSERT(intr_get_level() == INTR_OFF);
	thread_current()->status = THREAD_BLOCKED;
	schedule();
}

/* blocked 스레드를 ready상태로 전환한다
	(실행 중인 스레드를 ready로 만들려면 thread_yield()를 사용할 것)
	이 함수는 선점을 수행하지 않는다. 선점을 수행하는 것은 caller의 책임이다.*/
void thread_unblock(struct thread *curr)
{
	enum intr_level old_level;

	ASSERT(is_thread(curr));

	old_level = intr_disable();

	ASSERT(curr->status == THREAD_BLOCKED);
	curr->status = THREAD_READY;
	list_insert_ordered(&ready_list, &curr->elem, compare_ready_priority, NULL);

	intr_set_level(old_level);
}

//  현재 실행 중인 스레드의 이름을 반환
const char *thread_name(void)
{
	return thread_current()->name;
}

// 현재 실행 중인 스레드를 반환한다
struct thread *thread_current(void)
{
	struct thread *curr = running_thread();

	ASSERT(is_thread(curr));
	ASSERT(curr->status == THREAD_RUNNING);

	return curr;
}

//  현재 실행 중인 스레드의 tid를 반환한다
tid_t thread_tid(void)
{
	return thread_current()->tid;
}

// 현재 스레드를 스케줄에서 제외하고 파괴한다
void thread_exit(void)
{
	ASSERT(!intr_context());

#ifdef USERPROG
	process_exit();
#endif

	// 상태를 DYING으로 설정하고 다른 프로세스를 스케줄함
	intr_disable();
	do_schedule(THREAD_DYING);
	NOT_REACHED();
}

// CPU를 양보한다. 현재 스레드는 sleep 상태가 되지 않으며, 스케줄러에 의해 즉시 다시 스케줄될 수 있다.
void thread_yield(void)
{
	struct thread *curr = thread_current();
	enum intr_level old_level;

	ASSERT(!intr_context());

	old_level = intr_disable();
	if (curr != idle_thread)
		list_insert_ordered(&ready_list, &curr->elem, compare_ready_priority, NULL);

	do_schedule(THREAD_READY);

	intr_set_level(old_level);
}

/**
 * @brief 우선순위 기반 선점 스케줄링을 수행하는 함수
 *
 * @details 현재 실행 중인 스레드의 우선순위가 ready_list의 최상위(가장 높은)
 *          우선순위 스레드보다 낮은 경우, 즉시 CPU를 양보하여 선점 스케줄링을 수행한다.
 *
 * @note 이 함수는 다음 상황에서 호출되어야 한다다:
 *       - 새로운 스레드가 생성되어 ready_list에 추가될 때 (thread_create)
 *       - blocked 스레드가 unblock되어 ready_list에 추가될 때 (thread_unblock)
 *       - 현재 스레드의 우선순위가 동적으로 변경될 때 (thread_set_priority)
 *
 * @warning 이 함수를 호출하기 전에 반드시 인터럽트를 비활성화해야 한다.
 *          원자적(atomic) 연산을 보장하지 않으면 race condition이 발생할 수 있습니다.
 *
 * @see thread_yield()
 * @see thread_create()
 * @see thread_set_priority()
 */
void preemption_by_priority(void)
{
	// 실행 가능한 다른 스레드가 존재하는지 체크
	if (!list_empty(&ready_list) &&
			// 현재 스레드의 우선순위와 ready_list 최상위 스레드의 우선순위 비교
			thread_current()->priority < list_entry(list_front(&ready_list), struct thread, elem)->priority)
	{
		// 현재 스레드보다 우선순위가 높은 스레드가 있으면 즉시 CPU 양보
		thread_yield();
	}
}

/**
 * @brief 현재 스레드의 우선순위를 변경하는 함수
 *
 * @param new_priority 설정할 새로운 우선순위 값
 *
 * @details 이 함수는 현재 실행 중인 스레드의 우선순위를 new_priority로 설정하고,
 *          우선순위 기부 상황을 고려하여 실제 우선순위를 재계산한다.
 * 					우선순위 변경 후, 현재 스레드보다 높은 우선순위를 가진
 *          스레드가 ready_list에 있다면 즉시 CPU를 양보한다.
 *
 * @note Priority Donation 동작:
 *       - original_priority: 스레드 본래의 우선순위 (기부받지 않은 기본값)
 *       - priority: 실제 실행 우선순위 (기부받은 우선순위 중 최댓값)
 *       - 우선순위가 낮아져서 더 이상 최고 우선순위가 아니면 CPU를 즉시 양보
 *
 * @warning 이 함수는 현재 스레드에만 적용되며, 다른 스레드의 우선순위는 변경하지 않는다.
 *
 * @see recalculate_priority()
 * @see preemption_by_priority()
 * @see thread_get_priority()
 */
void thread_set_priority(int new_priority)
{
	// 스레드의 본래(original) 우선순위 업데이트
	thread_current()->original_priority = new_priority;

	// 우선순위 기부(donation) 상황을 고려하여 실제 우선순위 재계산
	recalculate_priority();

	enum intr_level old_level = intr_disable();
	preemption_by_priority(); // 우선순위 변경 후 선점 스케줄링 체크
	intr_set_level(old_level);
}

// 현재 스레드의 우선순위를 반환한다.
int thread_get_priority(void)
{
	return thread_current()->priority;
}

/* idle 스레드. 실행 가능한 다른 스레드가 없을 때 실행된다.

	idle 스레드는 thread_start()에 의해 처음 ready_list에 추가된다.
	최초로 스케줄될 때 idle_thread를 초기화하고,
	전달받은 세마포어를 up하여 thread_start()가 계속 진행될 수 있게 한 뒤 즉시 block된다.
	이후 idle 스레드는 ready_list에 다시 추가되지 않는다.
	ready_list가 비어 있을 때 next_thread_to_run()에서 특별히 반환됩니다. */
static void idle(void *idle_started_ UNUSED)
{
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current();
	sema_up(idle_started);

	for (;;)
	{
		// 다른 스레드에게 CPU 양보
		intr_disable();
		thread_block();

		// 인터럽트 재활성화 후 대기 (sti; hlt는 원자적으로 실행되어 시간 낭비 방지)
		asm volatile("sti; hlt" : : : "memory");
	}
}

// 커널 스레드의 기반이 되는 함수
static void kernel_thread(thread_func *function, void *aux)
{
	ASSERT(function != NULL);

	intr_enable();
	function(aux);
	thread_exit();
}

// 스레드로 기본 초기화
static void init_thread(struct thread *t, const char *name, int priority)
{
	ASSERT(t != NULL);
	ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT(name != NULL);

	memset(t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy(t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t)t + PGSIZE - sizeof(void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC;

	// donate 관련
	t->original_priority = priority;
	list_init(&t->donators);
	t->waiting_lock = NULL;
}

/* 다음에 스케줄될 스레드를 선택하여 반환한다.
	실행 큐가 비어 있지 않으면 그 중 하나를 반환하고, 비어 있으면 idle_thread를 반환 */
static struct thread *next_thread_to_run(void)
{
	if (list_empty(&ready_list))
		return idle_thread;
	else
	{
		// list_sort(&ready_list, compare_ready_priority, NULL);
		return list_entry(list_pop_front(&ready_list), struct thread, elem);
	}
}

/* 각 thread의 donation_elem 멤버를 기준으로 우선순위를 비교하여 내림차순 정렬 */
bool compare_donation_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{

	struct thread *thread_a = list_entry(a, struct thread, donation_elem);
	struct thread *thread_b = list_entry(b, struct thread, donation_elem);
	return thread_a->priority > thread_b->priority;
}

/* 각 thread의 elem 멤버를 기준으로 우선순위를 비교하여 내림차순 정렬 */
bool compare_ready_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
	ASSERT(a != NULL);
	ASSERT(b != NULL);
	struct thread *thread_a = list_entry(a, struct thread, elem);
	struct thread *thread_b = list_entry(b, struct thread, elem);
	return thread_a->priority > thread_b->priority;
}

/* iretq 명령어를 사용하여 스레드를 실행 */
void do_iret(struct intr_frame *tf)
{
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g"((uint64_t)tf) : "memory");
}

/* 새로운 스레드의 페이지 테이블을 활성화하여 스레드를 전환하고, 이전 스레드가 죽는 중이면 파괴한다.
	스레드 전환이 끝나기 전까지는 printf()를 호출하면 안전하지 않다. */
static void thread_launch(struct thread *th)
{
	uint64_t tf_cur = (uint64_t)&running_thread()->tf;
	uint64_t tf = (uint64_t)&th->tf;
	ASSERT(intr_get_level() == INTR_OFF);

	// 실행 컨텍스트 복원 후 do_iret로 스레드 전환, 스택 사용 금지
	__asm __volatile(
			// 사용할 레지스터 저장
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			// 입력값 한 번만 가져옴
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n" // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n" // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n" // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n" // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"	 // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g"(tf) : "memory");
}

/* 새로운 프로세스를 스케줄한다.
	schedule() 내에서는 printf()를 호출하면 안전하지 않다. */
static void
do_schedule(int status)
{
	ASSERT(intr_get_level() == INTR_OFF);
	ASSERT(thread_current()->status == THREAD_RUNNING);

	while (!list_empty(&dying_threads_queue))
	{
		struct thread *victim =
				list_entry(list_pop_front(&dying_threads_queue), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current()->status = status;
	schedule();
}

static void
schedule(void)
{
	struct thread *curr = running_thread();
	struct thread *next = next_thread_to_run();

	ASSERT(intr_get_level() == INTR_OFF);
	ASSERT(curr->status != THREAD_RUNNING);
	ASSERT(is_thread(next));

	next->status = THREAD_RUNNING;

	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate(next);
#endif

	if (curr != next)
	{
		// 종료 중인 스레드는 즉시 파괴하지 않고, 스택 사용이 끝난 뒤 schedule()에서 안전하게 메모리 해제한다
		if (curr && curr->status == THREAD_DYING && curr != initial_thread)
		{
			ASSERT(curr != next);
			list_push_back(&dying_threads_queue, &curr->elem);
		}

		// 스레드 전환 전에 현재 실행 중인 정보를 저장한다
		thread_launch(next);
	}
}

// 새 스레드에 사용할 tid를 반환
static tid_t
allocate_tid(void)
{
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire(&tid_lock);
	tid = next_tid++;
	lock_release(&tid_lock);

	return tid;
}

int thread_get_nice(void) {};
void thread_set_nice(int) {};
int thread_get_recent_cpu(void) {};
int thread_get_load_avg(void) {};
