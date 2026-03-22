#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>

static int nthread = 1;
static int round = 0; // this is not used.

struct barrier {
  pthread_mutex_t barrier_mutex;
  pthread_cond_t barrier_cond;
  int nthread;      // GLOBAL Number of threads that have reached this round of the barrier, accumulating
  int round;     // GLOBAL Barrier round, used to judge which round it is now for all threads
} bstate;

static void
barrier_init(void)
{
  assert(pthread_mutex_init(&bstate.barrier_mutex, NULL) == 0);
  assert(pthread_cond_init(&bstate.barrier_cond, NULL) == 0);
  bstate.nthread = 0;
}

// 1. You should increment bstate.round each time all threads have reached the barrier.
// 2. thread A and B reachs barrier for round 0, reset global nthread = 0. A is fast, entering round 1, trying to update nthread++, but B is slow, it hasnt finished to exit barrier yet, and it will see global nthread = 1
// aka 如何确保在“上一波人”还没完全离开栅栏之前，“下一波人”不要进来乱动这个公共的计数器。
// how issue 2 is solved: mutex，pthread_cond_wait【最关键】重新获取锁 (Re-acquire)： 当线程被唤醒后，它并不立即返回。它必须先去抢回在第 1 步中释放掉的那个 mutex。
// 只有成功抢到了 mutex，pthread_cond_wait 函数才会结束运行并返回到你的 barrier() 代码中。
// 由于 cond_wait 返回时必须拿锁，这就强制所有被唤醒的线程必须排队通过那个 unlock。即使快跑者插入了排队序列（开始新的一轮），它也必须先等上一轮的“清理工作”（nthread = 0）完成后，才能修改 nthread。
// 所以，nthread 变量在每一轮之间被完美地隔离开了。
static void 
barrier()
{
  // YOUR CODE HERE
  //
  // Block until all threads have called barrier() and
  // then increment bstate.round.
  //
  pthread_mutex_lock(&bstate.barrier_mutex);
  // BUG WE can NOT put ++ inside if, aka it should be ++bstate.nthread;
  // because if we compare and then ++, every thread ends up in if clause, dead lock
  bstate.nthread++;
  // lock and check barrier.nthread < nthread
  if (bstate.nthread < nthread) {
    // current thread sleep and wait to be wake up
    pthread_cond_wait(&bstate.barrier_cond, &bstate.barrier_mutex);
  } else {
    // now every thread at the same stage, we can reset nthread = 0 and round++
    bstate.nthread = 0;
    bstate.round++;
    // wake up all threads!
    pthread_cond_broadcast(&bstate.barrier_cond);
  }
  pthread_mutex_unlock(&bstate.barrier_mutex);
}

static void *
thread(void *xa)
{
  long n = (long) xa;
  long delay;
  int i;

  for (i = 0; i < 20000; i++) {
    int t = bstate.round;
    assert (i == t);
    barrier();
    usleep(random() % 100); 
  }

  return 0;
}

int
main(int argc, char *argv[])
{
  pthread_t *tha;
  void *value;
  long i;
  double t1, t0;

  if (argc < 2) {
    fprintf(stderr, "%s: %s nthread\n", argv[0], argv[0]);
    exit(-1);
  }
  nthread = atoi(argv[1]);
  tha = malloc(sizeof(pthread_t) * nthread);
  srandom(0);

  barrier_init();

  for(i = 0; i < nthread; i++) {
    assert(pthread_create(&tha[i], NULL, thread, (void *) i) == 0);
  }
  for(i = 0; i < nthread; i++) {
    assert(pthread_join(tha[i], &value) == 0);
  }
  printf("OK; passed\n");
}
