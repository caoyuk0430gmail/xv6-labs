// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

// lock lab The basic idea is to maintain a free list per CPU, each list with its own lock.
// Allocations and frees on different CPUs can run in parallel, because each CPU will operate on a different list.
struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];


void
kinit()
{
  // init per-CPU lock
  for (int i = 0; i < NCPU; i++) {
    initlock(&kmem[i].lock, "kmem");
  }
  freerange(end, (void*)PHYSTOP);
}

// When xv6 starts, it has a big block of physical RAM (from the end of the kernel code up to `PHYSTOP`). The kernel doesn't know how to use it yet. `freerange` takes this giant block and "shreds" it into 4KB pages so that `kalloc` can hand them out later.
void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  // 1. ALIGNMENT
  // pa_start might not be a perfect multiple of 4096. 
  // PGROUNDUP rounds it UP to the next clean 4KB boundary. 
  // This ensures we don't try to manage a partial page.
  p = (char*)PGROUNDUP((uint64)pa_start);
  // 2. THE LOOP
  // We step through the memory chunk, jumping forward by PGSIZE (4096 bytes) each time.
  // We stop once the next page would go beyond the end address (pa_end).
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    // 3. THE HANDOVER
    // For every 4KB block we find, we call kfree().
    // Even though the memory isn't "freed" (since it was never used), 
    // kfree() is the function that knows how to add a page into the freelist.
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  // lab lock
  // now we need to get cpuid that run the kfree, and acquire/release this cpu lock only
  // The function cpuid returns the current core number, but it's only safe to call it and use its result when interrupts are turned off.
  // cpuid() is only safe when interrupts are off because interrupts can cause your process to be moved to a different physical CPU core
  // You should use push_off() and pop_off() to turn interrupts off and on.
  push_off();
  int cpu_id = cpuid();
  acquire(&kmem[cpu_id].lock);
  r->next = kmem[cpu_id].freelist;
  kmem[cpu_id].freelist = r;
  release(&kmem[cpu_id].lock);
  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
// LOCK LAB
// we acquire our own lock to get page from our own cpu freelist
// if no free available, we loop over cpus to find freelist to steal.
// Optimization: If we steal just 1 page, we will constantly fight other CPUs for locks. It's much better to steal a batch of pages (e.g., up to 64 pages) so we don't have to steal again soon.
// 

void *
kalloc(void)
{
  struct run *r;
  push_off();
  int cpu_id = cpuid();
  acquire(&kmem[cpu_id].lock);
  // r is the return value, Returns a pointer that the kernel can use. the actual allocated page.
  r = kmem[cpu_id].freelist;
  if(r)
    kmem[cpu_id].freelist = r->next;
  release(&kmem[cpu_id].lock);
  // pop_off() can not be called now, it needs to wait at the end, when we have attached the freelist we stilled to cpu_id freelist.
  // because if we now turn on intrs, scheduler can kicks in, and this process running kalloc could later be picked by another cpu, cpu1. But our cpu_id is assigned already before, e.g. cpu0.
  // So it will lead to we attach freelist to cpu1.freelist, instead of cpu0.

  // now loop over cpus to find available freelist
  if (!r) {
    for (int i = 0; i < NCPU; i++) {
      if (i == cpu_id) {
        continue; // skip myself
      }
      acquire(&kmem[i].lock);
      if (kmem[i].freelist) {
        // since we already in !r, and we found the freelist, r is the head of it.
        // we need to save it to r first, before kmem[i].freelist start changing due to steal!
        r = kmem[i].freelist;
        kmem[i].freelist = r->next; // prepare fpr steal

        // we can not yet free kmem[i].lock beecause we need to iterate over the freelist to steal
        // it is possible that steal_head is null already, meaning we can just steal one page, ri. But 1 page is also fine
        struct run *steal_head = kmem[i].freelist;
        // we need tail, before after we steal, we will
        // 1. move steal_head to kmem[cpu_id].freelist 2. update kmem[i].freelist to tail->next
        struct run *steal_tail = steal_head;

        int count = 0;
        // we also check if steal_head is empty
        while (steal_tail && steal_tail->next && count < 64) {
          steal_tail = steal_tail->next;
          count++;
        }
        // now either steal_tail->next is null or we have stole 64 pages
        // bug if steal_head make sure we skip if there is nothing!
        if (steal_head) {
          // we should 1. update kmem[i].freelist 2. release kmem[i].lock 3. re-acquire cpu_id lock to update cpu_id list! order is important to ensure always ONE lock at any time!
          kmem[i].freelist = steal_tail->next;
          // IMPORTANT: since kmem[i].freelist is already pointed at new end, we need to cut off tail->next from kmem[i].freelist! Because from now on, tail belongs to cpu_id, and tail->next should be null for cpu_id freelist!
          steal_tail->next = 0;
        }

        // finish modify kmem[i].freelist, release lock
        release(&kmem[i].lock);

        if (steal_head) {
          // re-acquire cpu_id lock
          acquire(&kmem[cpu_id].lock);
          // bug: notice kmem[cpu_id].freelist is already null
          // kmem[cpu_id].freelist->next = steal_head;
          kmem[cpu_id].freelist = steal_head;
          release(&kmem[cpu_id].lock);
        }
        break; // we found ri! ok to return!
      }
      release(&kmem[i].lock); // if kmem[i].freelist is not found!
    }
  }

  // pop_off() can be called now, it needs to wait at the end, when we have attached the freelist we stilled to cpu_id freelist.
  pop_off();

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
