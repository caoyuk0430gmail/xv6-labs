// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

struct {
  struct spinlock lock;
  int count[PHYSTOP / PGSIZE];
} ref_struct;

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

// the struct run is physically located at the very beginning of the 4KB memory page, the address of the struct r is exactly the same as the physical address of the page.
struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  // lab COW
  initlock(&ref_struct.lock, "ref_struct");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
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

  // we only free when the ref_count is 0.
  acquire(&ref_struct.lock);
  int idx = (uint64)pa / PGSIZE;
  // here covers kref_down
  ref_struct.count[idx]--;
  if (ref_struct.count[idx] > 0) {
    // we should not kfree
    release(&ref_struct.lock);
    return;
  }
  // if == 0 we kfree
  release(&ref_struct.lock);

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  // the struct run is physically located at the very beginning of the 4KB memory page, the address of the struct r is exactly the same as the physical address of the page.
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  // lab COW
  // we allocate a new page, we need to set ref_count to 1, init
  acquire(&ref_struct.lock);
  ref_struct.count[(uint64)r / PGSIZE] = 1;
  release(&ref_struct.lock);
  return (void*)r;
}

// when fork is called, COW first increment old(parent) pa
// kref_down is covered in kfree
void
kref_up(void *pa) {
  acquire(&ref_struct.lock);
  ref_struct.count[(uint64)pa / PGSIZE]++;
  release(&ref_struct.lock);
}
