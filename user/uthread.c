/* simulating thread switching entirely in user space, without the kernel knowing anything about it. */
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

/* Possible states of a thread: */
#define FREE        0x0
#define RUNNING     0x1
#define RUNNABLE    0x2

#define STACK_SIZE  8192
#define MAX_THREAD  4

// Saved registers for kernel context switches.
struct context {
  uint64 ra;
  uint64 sp;

  // callee-saved
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};

struct thread {
  char       stack[STACK_SIZE]; /* the thread's stack */
  int        state;             /* FREE, RUNNING, RUNNABLE */
  struct context context;      // thread_switch() here to run process

};
// all_thread is a array sized 4
struct thread all_thread[MAX_THREAD];
struct thread *current_thread;
extern void thread_switch(uint64, uint64);
              
void 
thread_init(void)
{
  // main() is thread 0, which will make the first invocation to
  // thread_schedule().  it needs a stack so that the first thread_switch() can
  // save thread 0's state.  thread_schedule() won't run the main thread ever
  // again, because its state is set to RUNNING, and thread_schedule() selects
  // a RUNNABLE thread.
  current_thread = &all_thread[0];
  current_thread->state = RUNNING;
}

void 
thread_schedule(void)
{
  struct thread *t, *next_thread;

  /* Find another runnable thread and assign to next_thread. */
  next_thread = 0;
  t = current_thread + 1;
  for(int i = 0; i < MAX_THREAD; i++){
    if(t >= all_thread + MAX_THREAD)
    // round to start from beginning
      t = all_thread;
    if(t->state == RUNNABLE) {
      next_thread = t;
      break;
    }
    t = t + 1;
  }

  if (next_thread == 0) {
    printf("thread_schedule: no runnable threads\n");
    exit(-1);
  }

  if (current_thread != next_thread) {         /* switch threads?  */
    next_thread->state = RUNNING;
    t = current_thread;
    current_thread = next_thread;
    /* YOUR CODE HERE
     * Invoke thread_switch to switch from t to next_thread:
     * thread_switch(??, ??);
     */
     // lab we follow swtch() style
     thread_switch((uint64)&t->context, (uint64)&next_thread->context);
  } else
    next_thread = 0;
}

void 
thread_create(void (*func)())
{
  struct thread *t;

  for (t = all_thread; t < all_thread + MAX_THREAD; t++) {
    // FREE (0): This slot in the array is empty and available for a new thread.
    // start from thread 0, we first stop at thread a. the loost combination is t->context.ra is assigned to return addr of thread_a, this is only mapping between thread 1 -- a
    if (t->state == FREE) break;
  }
  t->state = RUNNABLE;
  // YOUR CODE HERE
  // we need to simulate forkret, because it said  when thread_schedule() runs a given thread for the first time, the thread executes the function passed to thread_create()
  // thread_switch: ld ra, 0(a1)  # CPU's 'ra' register becomes the address of thread_a! ret           # CPU jumps to the address in 'ra'
  t->context.ra = (uint64)func;
  // stack grow downwards
  t->context.sp = (uint64)&t->stack + STACK_SIZE;
}

void 
thread_yield(void)
{
  current_thread->state = RUNNABLE;
  thread_schedule();
}

volatile int a_started, b_started, c_started;
volatile int a_n, b_n, c_n;

// Yield 1 (Sync): "I'm waiting for the others. You go." (Saves state, stays RUNNABLE).
// Yield 2 (Work): "I did one step. Your turn." (Saves state, stays RUNNABLE).
// State = FREE: "I am completely done. Remove me from the roster."
// Schedule (Exit): "Goodbye." (Saves state, but since state is FREE, it will never be loaded again).
void 
thread_a(void)
{
  int i;
  printf("thread_a started\n");
  a_started = 1;
  // thread_a wants to make sure all three threads have officially "woken up" before it starts doing its real work (counting to 100).
  // Since main() scheduled A first, B and C haven't run yet! Their flags are 0.
  // The Yield (Call #1):
  // thread_a says: "I can't proceed yet. I will give the CPU to someone else."
  // It calls thread_yield(), which calls thread_schedule().
  // thread_schedule saves thread_a's state and switches to thread_b.
  // (Later, B runs and yields. Then C runs and yields. Eventually, it's A's turn again.)
  while(b_started == 0 || c_started == 0)
    thread_yield();
  
  for (i = 0; i < 100; i++) {
    printf("thread_a %d\n", i);
    a_n += 1;
    thread_yield();
  }
  printf("thread_a: exit after %d\n", a_n);
  // thread_a tells the scheduler: "I am dead. Do not ever schedule me again."
  // It also tells thread_create: "My slot in the all_thread array is now empty. You can reuse this slot to create a new thread if you want.
  current_thread->state = FREE;
  // It explicitly calls thread_schedule() one last time to hand the CPU over to the remaining threads (B or C).
  thread_schedule();
}

void 
thread_b(void)
{
  int i;
  printf("thread_b started\n");
  b_started = 1;
  while(a_started == 0 || c_started == 0)
    thread_yield();
  
  for (i = 0; i < 100; i++) {
    printf("thread_b %d\n", i);
    b_n += 1;
    thread_yield();
  }
  printf("thread_b: exit after %d\n", b_n);

  current_thread->state = FREE;
  thread_schedule();
}

void 
thread_c(void)
{
  int i;
  printf("thread_c started\n");
  c_started = 1;
  while(a_started == 0 || b_started == 0)
    thread_yield();
  
  for (i = 0; i < 100; i++) {
    printf("thread_c %d\n", i);
    c_n += 1;
    thread_yield();
  }
  printf("thread_c: exit after %d\n", c_n);

  current_thread->state = FREE;
  thread_schedule();
}

// 1.thread_init(): This runs first. It sets up the "Main Thread" (the thread currently running main). It sets its state to RUNNING so the scheduler knows it exists.
// 2. thread_create(...): main() calls this three times (for A, B, and C).
  // This does not run the threads! It just fills out their struct context (forging ra and sp) and marks them as RUNNABLE.
// 3. thread_schedule(): main() calls this. This triggers the first context switch!
  // The scheduler searches the array, finds thread_a, and calls thread_switch(main_thread, thread_a).
// 4. Execution of thread_a: thread_a wakes up, prints "thread_a started", and does some math.
// 5. thread_yield(): thread_a calls yield. Yield simply calls thread_schedule().
  // The scheduler finds thread_b, and calls thread_switch(thread_a, thread_b).
// 6. Looping: They keep yielding to each other until they finish 100 loops, exit, and the program terminates.
int 
main(int argc, char *argv[]) 
{
  a_started = b_started = c_started = 0;
  a_n = b_n = c_n = 0;
  thread_init();
  thread_create(thread_a);
  thread_create(thread_b);
  thread_create(thread_c);
  thread_schedule();
  exit(0);
}
