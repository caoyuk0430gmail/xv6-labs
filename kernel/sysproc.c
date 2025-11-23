#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sysinfo.h"

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
sys_trace(void)
{
  int n;
  // argint(0, &mask) means: 
  // Fetch the first syscall argument (argument index 0) and interpret it as an integer, store it in mask.
  // The arguments to the syscall (like file descriptor, buffer pointer, size, mask, etc.) are placed in registers (a0, a1, a2, … on RISC‑V).
  // These are indexed starting at 0 in xv6’s helper functions (argint(0, …) means “fetch the first syscall argument from a0”).
  if(argint(0, &n) < 0)
    return -1;
  myproc()->mask = n;
  return 0;
}

// lab2 sysinfo
// The system call takes one argument: a pointer to a struct sysinfo (see kernel/sysinfo.h).
// The kernel should fill out the fields of this struct: the freemem field should be set to the number 
// of bytes of free memory, and the nproc field should be set to the number of processes whose state is not UNUSED.
// User Space (process virtual memory)
// -----------------------------------
// struct sysinfo si;          <-- user allocates struct on its stack
// sysinfo(&si);               <-- passes pointer (&si) to kernel

//         |
//         v   (ecall instruction triggers trap)

// Kernel Space (kernel stack + code)
// -----------------------------------
// sys_sysinfo():
//     struct sysinfo info;    <-- kernel allocates its own struct
//     info.freemem = kfreemem();
//     info.nproc   = nproc();

//     // copy kernel struct into user struct
//     copyout(myproc()->pagetable, addr, (char *)&info, sizeof(info));

//         |
//         v   (copyout translates addr via user pagetable)

// Physical Memory
// -----------------------------------
// - Kernel struct `info` lives in kernel memory
// - User struct `si` lives in user memory
// - copyout copies bytes from kernel → user

//         |
//         v   (syscall returns)

// User Space (resumes execution)
// -----------------------------------
// printf("free mem=%d, procs=%d\n", si.freemem, si.nproc);
uint64
sys_sysinfo(void)
{
  uint64 addr; // User pointer to struct sysinfo. addr is fetched with argaddr(0, &addr). That means the user program passed a pointer (e.g. &info in user code) as the first argument to the syscall.
  // struct lives on the kernel stack of the current process
  struct sysinfo info;

  // Fetch the argument (the pointer to the user's sysinfo struct)
  if(argaddr(0, &addr) < 0)
    return -1;

  // implement these two functions in kalloc.c and proc.c and add their prototypes to defs.h
  info.freemem = kfreemem(); // Returns number of bytes of free memory
  info.nproc = nproc();      // Returns number of processes not in UNUSED state

  // Copy the struct data back to user space, copyout uses myproc()->pagetable to translate addr into the correct physical location in the user’s memory.
  // Then it copies the bytes from kernel memory into that user memory region.
  if(copyout(myproc()->pagetable, addr, (char *)&info, sizeof(info)) < 0)
    return -1;

  return 0;
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
