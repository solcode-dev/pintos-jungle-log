#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#ifdef VM
#include "vm/vm.h"
#endif

/* States in a thread's life cycle. */
enum thread_status
{
	THREAD_RUNNING, /* Running thread. */
	THREAD_READY,		/* Not running but ready to run. */
	THREAD_BLOCKED, /* Waiting for an event to trigger. */
	THREAD_DYING		/* About to be destroyed. */
};

typedef int tid_t;
#define TID_ERROR ((tid_t) - 1)
#define PRI_MIN 0
#define PRI_DEFAULT 31
#define PRI_MAX 63

struct thread
{
	// Owned by thread.c.
	tid_t tid;								 /* Thread identifier. */
	enum thread_status status; /* Thread state. */
	char name[16];						 /* Name (for debugging purposes). */
	int priority;							 /* Priority. */

	// donate 관련
	int original_priority;					// 기부받기 전 원래 우선순위
	struct list donators;						// 나한테 기부한 쓰레드 목록
	struct lock *waiting_lock;			// 내가 기다리는 locks
	struct list_elem donation_elem; // donatior용 리스트에 사용

	/* Shared between thread.c and synch.c. */
	struct list_elem elem; /* List element. */
	int64_t wakeup_tick;	 /* Wakeup tick. */

#ifdef USERPROG
	/* Owned by userprog/process.c. */
	uint64_t *pml4; /* Page map level 4 */
#endif
#ifdef VM
	/* Table for whole virtual memory owned by thread. */
	struct supplemental_page_table spt;
#endif

	/* Owned by thread.c. */
	struct intr_frame tf; /* Information for switching */
	unsigned magic;				/* Detects stack overflow. */
};

extern bool thread_mlfqs;

void thread_init(void);
void thread_start(void);

void thread_tick(void);
void thread_print_stats(void);

typedef void thread_func(void *aux);
tid_t thread_create(const char *name, int priority, thread_func *, void *);

void thread_block(void);
void thread_unblock(struct thread *);

struct thread *thread_current(void);
tid_t thread_tid(void);
const char *thread_name(void);
void thread_exit(void) NO_RETURN;
void thread_yield(void);

int thread_get_priority(void);
void thread_set_priority(int);

int thread_get_nice(void);
void thread_set_nice(int);
int thread_get_recent_cpu(void);
int thread_get_load_avg(void);

void do_iret(struct intr_frame *tf);

bool compare_ready_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);
bool compare_donation_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);
void preemption_by_priority(void);

#endif
