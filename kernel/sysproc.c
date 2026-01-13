// System calls related to the Process itself.
// fork (create process)
// exit (destroy process)
// wait (manage child process)
// sleep (pause process)
// kill (signal process)
// sigalarm: This modifies process behavior (scheduling interrupts).

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  backtrace();

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

// The user writes sigalarm(10, my_handler).
// 10 is put into register a0.
// Address of my_handler is put into register a1.
// Syscall ID for sigalarm is put into a7.
// ecall is executed (Trap).
// Kernel Space Entry:
// The hardware saves all registers (a0, a1, etc.) into the Trapframe.
// Kernel Space Function:
// syscall() looks at a7 and calls sys_sigalarm().
// sys_sigalarm() takes void (no arguments in C).
// Why? Because C arguments are usually on the stack or registers, but we just switched stacks! The arguments are "frozen" back in the Trapframe.
// Retrieval:
// Inside sys_sigalarm, you must actively reach back into the Trapframe to grab the data.
// argint(0, ...) grabs the 10 from the saved a0.
// argaddr(1, ...) grabs the function address from the saved a1.
// sys_sigalarm is called by the user at the beginning of the program to configure the alarm
uint64
sys_sigalarm(void)
{
  int ticks_interval;
  uint64 handler; // user pointer
  if(argint(0, &ticks_interval) < 0 || argaddr(1, &handler) < 0)
    return -1;

  struct proc *p = myproc();
  p->ticks_interval = ticks_interval;
  p->alarm_handler = (void(*)())handler;
  // whenever alarm triggered, passes is reset to zero.
  p->ticks_passed = 0;
  // init alarm, not running yet. will be triggered in usertrap
  p->handler_running = 0;
  return 0;
}

// user alarm handlers are required to call the sigreturn system call when they have finished
uint64
sys_sigreturn(void)
{
  // Copy the backup *p->alarm_trapframe BACK into *p->trapframe.
  // This restores epc (Program Counter), sp (Stack Pointer), and all registers to exactly how they were before the alarm.
  struct proc *p = myproc();
  *p->trapframe = *p->alarm_trapframe;
  // alarm not running
  p->handler_running = 0;
  // sys_sigreturn usually returns the value in a0. If we restore the entire trapframe, we are restoring the old a0 too.
  return p->trapframe->a0;
}
